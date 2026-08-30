#pragma once

/** Grids-native GPU brush stroke session (grids-native brush path, G4).
 *
 * Runs the same WGSL/SPIR-V kernels the mesh GPU path runs, against a
 * GridLevelDomain instead of a materialized mesh, through the backend-neutral
 * IBrushComputeDispatch (wgpu-native or Vulkan — the caller creates the
 * dispatcher and loads the kernel; this session owns every byte layout).
 * Scene-free by construction: it touches only the domain, the tree, the
 * brush, and the grids stroke log.
 *
 * Marshaling reuse is near-total: geometry packing is trivial (the domain's
 * buffers are already flat and dense), the neighbor CSR is the domain's
 * lattice ring1 verbatim (both grids backends share it, so CPU-grids vs
 * GPU-grids parity carries no neighbor-order caveat), chunking walks leaf
 * owned-vert lists, and the uniform/stroke-path packing is the shared
 * gpu_marshal code untouched.
 *
 * Contracts carried over from the mesh GPU path:
 *  - undo ordering: leaves are captured into GridStrokeLog at dab marshal,
 *    BEFORE any readback overwrites domain positions;
 *  - per-dab moved-verts readback keeps the domain current for raycast and
 *    bounds (kernel-side buffers stay authoritative across dabs);
 *  - kernel-visible normals are stroke-static (uploaded at begin), exactly
 *    like the mesh batch path — the domain's CPU normals refresh per dab for
 *    host-side queries only. */

#include "compute_dispatch.h"
#include "gpu_marshal.h"
#include "grid_executor.h"

#include <chrono>
#include <cstring>
#include <string>

namespace sculptcore::brush {

struct GridGpuStrokeSession {
  subdiv::GridLevelDomain *domain = nullptr;
  subdiv::GridTree *tree = nullptr;
  Brush *brush = nullptr;
  subdiv::GridStrokeLog *log = nullptr;
  IBrushComputeDispatch *disp = nullptr; // caller-owned, kernel already loaded
  SculptBrushes tool = SculptBrushes::DRAW;
  const GpuKernelInfo *info = nullptr;
  bool nonAccum = false;

  /** True while begin() succeeded and end() has not run. */
  bool active = false;

  /** Per-phase wall time (ms), for the "at what size does GPU win" bench. */
  struct Stats {
    double dispatchMs = 0, readbackMs = 0, hostMs = 0;
    int dabs = 0;
  };
  Stats stats;

  /** The stroke's accumulated moved-vert set (dense ids, deduped). */
  const Vector<int> &strokeTouchedVerts() const
  {
    return strokeTouched_;
  }

  /** Kernel stem for `tool` when it can run grids-native on the GPU, else
   * null — the dispatch rule (both the grids roster and the GPU kernel map
   * must carry it; face-stage kernels are excluded by the roster already). */
  static const GpuKernelInfo *kernelFor(SculptBrushes tool,
                                        const subdiv::MultiresAttrs *attrs = nullptr)
  {
    if (!GridBrushExecutor::supportsBrush(tool, attrs)) {
      return nullptr;
    }
    return gpuKernelForTool(tool);
  }

  /** Upload the stroke-static state: geometry from the domain's dense
   * buffers, lattice-CSR neighbors, cavity automask, brush texture. `d`'s
   * tree is built on demand. `dispatch` must have the tool's kernel loaded. */
  bool begin(subdiv::GridLevelDomain *d,
             Brush *b,
             SculptBrushes t,
             IBrushComputeDispatch *dispatch,
             subdiv::GridStrokeLog *lg,
             std::string &err)
  {
    domain = d;
    tree = d->ensureTree();
    brush = b;
    log = lg;
    disp = dispatch;
    tool = t;
    info = kernelFor(t, d->multires() ? &d->multires()->gridAttrs() : nullptr);
    if (!info) {
      err = "grid gpu stroke: tool not grids-GPU-capable";
      return false;
    }
    {
      // The @unbounded node-filter floor comes from the kernel's own def
      // (GpuKernelInfo has no unbounded bit) — same rule as the CPU path.
      GridBrushExecutor::brush_command def;
      GridBrushExecutor::createCommandSwitch<AccumLive>(t, b, def);
      unbounded_ = def.unbounded;
    }
    const int vc = d->vertCount();
    vcount_ = vc;

    if (log && log->domain() != d) {
      log->attach(d);
    }
    if (info->writesMask) {
      d->ensureMaskChannel();
    }

    // Pack co/no/mask flat (the "trivial packGeometry" — dense already).
    beginCo_.resize(size_t(vc) * 3);
    Vector<float> no, mask;
    no.resize(size_t(vc) * 3);
    mask.resize(vc);
    const auto &pos = d->pos();
    for (int v = 0; v < vc; v++) {
      beginCo_[size_t(v) * 3 + 0] = pos[v][0];
      beginCo_[size_t(v) * 3 + 1] = pos[v][1];
      beginCo_[size_t(v) * 3 + 2] = pos[v][2];
      no[size_t(v) * 3 + 0] = d->no[v][0];
      no[size_t(v) * 3 + 1] = d->no[v][1];
      no[size_t(v) * 3 + 2] = d->no[v][2];
      mask[v] = d->mask[v];
    }
    beginMask_ = mask;
    if (!disp->beginStroke(beginCo_.data(), no.data(), mask.data(), vc)) {
      err = "grid gpu stroke: geometry upload failed";
      return false;
    }

    // Cavity automask over the domain CSR (view-normal is evaluated
    // dynamically in-kernel from no_buf, which holds domain normals).
    if (brush->automask_cavity || brush->automask_view_normal) {
      Vector<float> am;
      am.resize(vc);
      if (brush->automask_cavity) {
        CavityParams cp;
        cp.enabled = true;
        cp.blur_steps = brush->cavity_blur_steps;
        cp.factor = brush->cavity_factor;
        cp.inverted = brush->cavity_inverted;
        cp.use_curve = brush->cavity_use_curve;
        cp.curve_lut = brush->cavity_curve.data();
        GridBrushExecutor::GridCavitySrc src{d};
        CavityScratch scr;
        for (int v = 0; v < vc; v++) {
          am[v] = cavityRemap(cp, cavityRawT(src, v, cp.blur_steps, scr));
        }
      } else {
        for (int v = 0; v < vc; v++) {
          am[v] = 1.0f;
        }
      }
      disp->setAutomask(am.data(), vc);
    }

    // Lattice-CSR neighbors (bindings 12/13) — the domain ring1, u32-typed.
    if (info->needsNeighbors) {
      nbrMeta_.resize(vc);
      for (int v = 0; v < vc; v++) {
        nbrMeta_[v].offset = uint32_t(d->ring1Offsets[v]);
        nbrMeta_[v].count = uint32_t(d->ring1Offsets[v + 1] - d->ring1Offsets[v]);
      }
      nbrFlat_.resize(d->ring1.size());
      for (int i = 0; i < int(d->ring1.size()); i++) {
        nbrFlat_[i] = uint32_t(d->ring1[i]);
      }
      if (!disp->setNeighbors(nbrMeta_.data(), vc, nbrFlat_.data(), int(nbrFlat_.size())))
      {
        err = "grid gpu stroke: neighbor upload failed";
        return false;
      }
    }

    if (brush->tex_width > 0 && brush->tex_height > 0 && brush->tex_pixels.size() > 0) {
      if (!disp->setBrushTexture(
              brush->tex_pixels.data(), brush->tex_width, brush->tex_height))
      {
        err = "grid gpu stroke: brush texture upload failed";
        return false;
      }
    }

    touchedStamp_.resize(vc);
    for (int v = 0; v < vc; v++) {
      touchedStamp_[v] = 0;
    }
    leafStamp_.resize(tree->leaves.size());
    for (int i = 0; i < int(leafStamp_.size()); i++) {
      leafStamp_[i] = 0;
    }
    strokeTouched_.clear();
    strokeLeaves_.clear();
    grabPinned_ = false;
    grabLeaves_.clear();
    dabGen_ = 0;
    brush->resetStrokePath();
    if (log) {
      log->beginStep();
    }
    active = true;
    return true;
  }

  /** Dispatch one dab; keeps the domain current via a moved-verts readback. */
  bool dab(float3 origin, float3 normal, std::string &err)
  {
    using clock = std::chrono::steady_clock;
    auto msSince = [](clock::time_point t0) {
      return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    };
    stats.dabs++;
    auto tHost = clock::now();
    brush->pushStrokeSample(origin, normal);

    float floorR = unbounded_ ? brush->radius * brush->unboundedExtent : 0.0f;
    float r = std::fmax(brush->radius, floorR);
    dabLeaves_.clear();
    if (info->grabMode) {
      if (!grabPinned_) {
        tree->query(origin, r, grabLeaves_);
        grabPinned_ = true;
      }
      for (int li : grabLeaves_) {
        dabLeaves_.append(li);
      }
    } else {
      tree->query(origin, r, dabLeaves_);
    }
    if (dabLeaves_.size() == 0) {
      return true;
    }

    // Undo capture FIRST — before the currency readback below can overwrite
    // domain positions (the snapshot-before-readback ordering contract).
    if (log) {
      for (int li : dabLeaves_) {
        log->captureLeaf(li, !info->writesMask, info->writesMask);
      }
    }
    for (int li : dabLeaves_) {
      if (!leafStamp_[li]) {
        leafStamp_[li] = 1;
        strokeLeaves_.append(li);
      }
    }

    // Chunk the leaves' owned verts into <=64-wide workgroups (the grids
    // chunkNodes variant).
    uverts_.clear();
    chunks_.clear();
    for (int li : dabLeaves_) {
      const auto &owned = tree->leaves[li].ownedVerts;
      int at = 0;
      while (at < int(owned.size())) {
        int n = std::min(64, int(owned.size()) - at);
        ComputeNodeMeta meta;
        meta.vert_offset = uint32_t(uverts_.size());
        meta.vert_count = uint32_t(n);
        chunks_.append(meta);
        for (int k = 0; k < n; k++) {
          uverts_.append(uint32_t(owned[at + k]));
        }
        at += n;
      }
    }

    ComputeBrushUniforms bu;
    packBrushUniforms(*brush, tool, nonAccum, bu);
    if (info->grabMode) {
      bu.grab_dab_gen = ++dabGen_;
    }
    ComputeCtxUniforms cu;
    packCtxUniforms(*brush, tool, origin, normal, nullptr, cu);
    Vector<ComputeStrokeSample> sp;
    packStrokePath(*brush, sp);

    stats.hostMs += msSince(tHost);
    auto tDisp = clock::now();
    if (!disp->dab(bu,
                   cu,
                   uverts_.data(),
                   int(uverts_.size()),
                   chunks_.data(),
                   int(chunks_.size()),
                   brush->falloff_curve.data(),
                   sp.data(),
                   int(sp.size())))
    {
      err = "grid gpu stroke: compute dispatch failed";
      return false;
    }
    stats.dispatchMs += msSince(tDisp);
    auto tRead = clock::now();

    // Currency readback: this dab's region verts, folded into the domain so
    // raycast/bounds stay fresh (GPU buffers remain authoritative for the
    // kernel). Moved detection is a bit compare against the domain.
    int n = int(uverts_.size());
    coBack_.resize(size_t(n) * 3);
    if (disp->readbackVerts(uverts_.data(), n, coBack_.data(), nullptr)) {
      dabMoved_.clear();
      auto &pos = domain->pos();
      for (int i = 0; i < n; i++) {
        int v = int(uverts_[i]);
        float3 c(coBack_[size_t(i) * 3 + 0],
                 coBack_[size_t(i) * 3 + 1],
                 coBack_[size_t(i) * 3 + 2]);
        if (std::memcmp(&c, &pos[v], sizeof(float3)) != 0) {
          pos[v] = c;
          dabMoved_.append(v);
          if (touchedStamp_[v] == 0) {
            touchedStamp_[v] = 1;
            strokeTouched_.append(v);
          }
        }
      }
      if (dabMoved_.size() > 0) {
        domain->refreshNormals(std::span<const int>(dabMoved_.data(), dabMoved_.size()));
        tree->refreshBounds(std::span<const int>(dabLeaves_.data(), dabLeaves_.size()));
      }
    }
    stats.readbackMs += msSince(tRead);
    return true;
  }

  /** Full readback + the shared grids stroke-end fold. */
  bool end(std::string &err)
  {
    if (!active) {
      return true;
    }
    active = false;
    const int vc = vcount_;
    Vector<float> coOut, maskOut;
    coOut.resize(size_t(vc) * 3);
    if (info->writesMask) {
      maskOut.resize(vc);
    }
    if (!disp->endStroke(
            coOut.data(), nullptr, info->writesMask ? maskOut.data() : nullptr))
    {
      err = "grid gpu stroke: readback failed";
      return false;
    }

    // Fold the final GPU state into the domain; the changed set is a bit
    // compare against the stroke-start upload (per-dab readbacks already
    // stamped most of it, but the final image is authoritative).
    auto &pos = domain->pos();
    Vector<int> finalMoved;
    for (int v = 0; v < vc; v++) {
      float3 c(
          coOut[size_t(v) * 3 + 0], coOut[size_t(v) * 3 + 1], coOut[size_t(v) * 3 + 2]);
      if (std::memcmp(&c, &pos[v], sizeof(float3)) != 0) {
        pos[v] = c;
        finalMoved.append(v);
      }
      bool changedFromBegin =
          std::memcmp(&c, &beginCo_[size_t(v) * 3], sizeof(float3)) != 0;
      if (changedFromBegin && touchedStamp_[v] == 0) {
        touchedStamp_[v] = 1;
        strokeTouched_.append(v);
      }
    }
    if (info->writesMask) {
      for (int v = 0; v < vc; v++) {
        if (maskOut[v] != domain->mask[v]) {
          domain->mask[v] = maskOut[v];
        }
        if (maskOut[v] != beginMask_[v] && touchedStamp_[v] == 0) {
          touchedStamp_[v] = 1;
          strokeTouched_.append(v);
        }
      }
    }
    if (finalMoved.size() > 0) {
      domain->refreshNormals(std::span<const int>(finalMoved.data(), finalMoved.size()));
    }
    tree->refreshBounds(std::span<const int>(strokeLeaves_.data(), strokeLeaves_.size()));

    gridsFoldStroke(domain,
                    log,
                    std::span<const int>(strokeTouched_.data(), strokeTouched_.size()),
                    /*wroteCo=*/!info->writesMask,
                    /*wroteMask=*/info->writesMask);
    return true;
  }

private:
  bool unbounded_ = false;
  int vcount_ = 0;
  uint32_t dabGen_ = 0;
  Vector<float> beginCo_;
  Vector<float> beginMask_;
  Vector<ComputeVertNbr> nbrMeta_;
  Vector<uint32_t> nbrFlat_;
  Vector<int> dabLeaves_;
  Vector<uint32_t> uverts_;
  Vector<ComputeNodeMeta> chunks_;
  Vector<float> coBack_;
  Vector<int> dabMoved_;
  Vector<uint32_t> touchedStamp_;
  Vector<int> strokeTouched_;
  Vector<uint8_t> leafStamp_;
  Vector<int> strokeLeaves_;
  bool grabPinned_ = false;
  Vector<int> grabLeaves_;
};

} // namespace sculptcore::brush
