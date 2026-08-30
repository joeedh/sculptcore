#pragma once

/** Cage-dab entry for multires colour smoothing (CS2).
 *
 * Colour has no multires domain to live in — the store channel and the slot
 * column both die with the session — so a smoothing stroke over multires paint
 * has to run its dabs on the *cage* colour column, at cage resolution, and
 * re-derive the touched grids afterwards (the read direction of
 * Multires::scatterVertFloat4ToCage, inverted: the kernel writes the cage
 * first, then the grids collapse to it).
 *
 * The kernels are the ordinary generated mesh kernels run by CommandExecutor
 * over one synthetic SpatialNode holding the dab's visit set: the cage verts
 * owning the grids the dab reached (Multires::dabGrids → grid (0, 0) corner,
 * one cage vert per grid). Two kernel inputs are substituted at data level,
 * with no codegen change:
 *
 * - positions: the cage's `v.co` AttrData pages are swapped with a
 *   limit-position snapshot (each grid's sample (0, 0) on the active level,
 *   taken at begin()) for the duration of each exec() — falloff and the
 *   coPrev snapshot then grade against the surface the user sees, not the
 *   undisplaced cage. Colour kernels never write positions, so the swap is
 *   observation-only.
 * - mask: `.spatial.v.mask` on the cage is seeded at begin() from the store's
 *   `mask` channel grid-(0, 0) samples (zeros when absent).
 *
 * The executor runs tree-less (nullptr) and meshlog-less: the only tree deref
 * on this path is border propagation, gated on nodes.size() > 1, and capture
 * early-returns on a null meshLog (capture_policy.h) — undo stays the host's
 * cage-column snapshot, exactly as on the per-dab scatter path.
 */

#include "brush/brush.h"
#include "brush/brush_executor.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "spatial/node.h"
#include "spatial/spatial_attrs.h"
#include "subdiv/grid_draw_source.h"
#include "subdiv/multires.h"

namespace sculptcore::brush {

struct CageSmoothSession {
  subdiv::Multires *mr = nullptr;
  int level = 0;
  Brush *brush = nullptr;
  CommandExecutor exec;
  util::string attrName;

  CageSmoothSession(subdiv::Multires *mr, int level, Brush *brush)
      : mr(mr), level(level), brush(brush), exec(nullptr, brush),
        limitCo_("cage_smooth.limit_co")
  {
  }

  ~CageSmoothSession()
  {
    end();
  }

  CageSmoothSession(const CageSmoothSession &) = delete;
  CageSmoothSession(CageSmoothSession &&) = delete;

  /** Set the stroke up: visit-set map, cage colour column (white-flooded when
   * fresh, the scatter's rule), limit-position snapshot, mask seed, executor
   * step. False when the level has no materialized slot mesh + tree (the host
   * must materialize before stroking, as it already does for the mesh path). */
  bool begin(const char *name)
  {
    mesh::Mesh *cage = mr ? mr->cage() : nullptr;
    if (!cage || !brush || !name || level < 1 || level > mr->maxLevel() || begun_) {
      return false;
    }
    subdiv::MultiresSlot *slot = mr->findSlot(level);
    if (!slot || !slot->mesh || !slot->tree) {
      return false;
    }
    gridVert_.clear();
    if (!mr->gridCageVerts(gridVert_)) {
      return false;
    }
    attrName = name;

    const bool fresh = !cage->v.attrs.has(mesh::AttrType::FLOAT4, attrName);
    mesh::AttrRef &ref = cage->v.attrs.ensure(mesh::AttrType::FLOAT4, attrName, true);
    auto *cdata = static_cast<mesh::AttrData<math::float4> *>(ref.data);
    if (!cdata) {
      return false;
    }
    if (fresh) {
      // White, not the attribute system's zero — the scatter's own rule for a
      // first-ever colour column (see Multires::scatterVertFloat4ToCage).
      for (int vi : cage->v) {
        cdata->materialize(vi);
        (*cdata)[vi] = math::float4(1.0f, 1.0f, 1.0f, 1.0f);
      }
    }

    // Limit-position snapshot: grid corners take the slot mesh's sample
    // (0, 0), every other cage vert keeps its cage position.
    auto *coData = cage->v.co.get_data();
    const subdiv::SubdivLevel &lvl = mr->refiner.levels[level - 1];
    const int w = lvl.gridSide + 1;
    limitCo_.resize(size_t(coData->size()), true);
    for (int i = 0; i < coData->size(); i++) {
      limitCo_[i] = coData->safe_get(i);
    }
    for (int g = 0; g < int(gridVert_.size()); g++) {
      limitCo_[gridVert_[g]] = slot->mesh->v.co[lvl.gridVerts[size_t(g) * w * w]];
    }

    // Mask seed: store `mask` channel grid-(0, 0) samples onto the cage's
    // `.spatial.v.mask` (BasicVertexIter's source). Zeros when absent.
    treeMesh_.setup(cage);
    int ch = mr->store.findChannel(util::string("mask"));
    if (ch >= 0 && (mr->store.channelElemSize(ch) != 1 ||
                    !mr->store.channelLevelAllocated(level, ch)))
    {
      ch = -1;
    }
    for (int g = 0; g < int(gridVert_.size()); g++) {
      treeMesh_.v.mask[gridVert_[g]] =
          ch >= 0 ? mr->store.elem(level, ch, g, 0, 0)[0] : 0.0f;
    }

    node_.treeMesh = &treeMesh_;
    if (!node_.data) {
      node_.create_data();
    }
    node_.data->m = cage;

    // V1 policy for cage dabs: no cavity automask (it would grade against
    // cage curvature, not the displaced surface) and no UV reprojection
    // bookkeeping (positions never move here).
    savedCavity_ = brush->automask_cavity;
    savedReprojectUvs_ = brush->reproject_uvs;
    brush->automask_cavity = false;
    brush->reproject_uvs = false;

    exec.setStrokeGen(1);
    exec.beginStep(false);
    begun_ = true;
    return true;
  }

  /** MeshStroke_dabBatch's per-dab prop cycle and mirror-image loop, with the
   * visit set from Multires::dabGrids instead of a tree filterNodes. `dabs` =
   * n x 7 {center.xyz, normal.xyz, radius}; `signs` = mirrorCount x 3.
   * Returns the total visited-vert count across images, -1 on a bad call. */
  int dabBatch(int tool,
               int n,
               const float *dabs,
               float strength,
               bool invert,
               float pressure,
               bool usePressure,
               const float *signs,
               int mirrorCount)
  {
    mesh::Mesh *cage = mr ? mr->cage() : nullptr;
    if (!begun_ || !cage || !dabs || n < 1) {
      return -1;
    }
    ensureOverrides(tool, cage);
    auto *coData = cage->v.co.get_data();
    int total = 0;
    auto oneImage = [&](math::float3 center, math::float3 normal, float radius) {
      dabGridsOut_.clear();
      const float row[4] = {center[0], center[1], center[2], radius};
      mr->dabGrids(level, row, 1, dabGridsOut_);
      if (dabGridsOut_.size() == 0) {
        return;
      }
      node_.data->unique_verts = util::OrderedSet<int>();
      for (int g : dabGridsOut_) {
        node_.data->unique_verts.add(gridVert_[g]);
      }
      node_.affected_verts.clear();

      nodes_.clear();
      nodes_.append(&node_);
      std::swap(coData->pages, limitCo_.pages);
      exec.setGrabAccumAdd(false);
      exec.execBrush(cage, SculptBrushes(tool), &nodes_, center, normal);
      exec.clearIsFirstOfStep();
      std::swap(coData->pages, limitCo_.pages);

      total += int(node_.data->unique_verts.size());
      if (node_.affected_verts.size() > 0) {
        epilogue();
      }
    };
    for (int i = 0; i < n; i++) {
      const float *d = dabs + i * 7;
      // Rewritten every dab: loadProps assigns post-dynamics values back into
      // the Brush fields (see mapping.apply_dab_state host-side).
      brush->strength = strength;
      brush->radius = d[6];
      brush->invert = invert;
      brush->writeProps();
      if (usePressure) {
        brush->clearDeviceInputs();
        brush->pushDeviceInput(int(props::DeviceType::PRESSURE), pressure);
      }
      oneImage(math::float3(d[0], d[1], d[2]), math::float3(d[3], d[4], d[5]), d[6]);
      for (int mi = 0; mi < mirrorCount; mi++) {
        const float *sg = signs + mi * 3;
        oneImage(math::float3(d[0] * sg[0], d[1] * sg[1], d[2] * sg[2]),
                 math::float3(d[3] * sg[0], d[4] * sg[1], d[5] * sg[2]),
                 d[6]);
      }
    }
    return total;
  }

  /** Close the executor step and restore the brush's saved policy bits.
   * Idempotent; the destructor calls it too. */
  void end()
  {
    if (!begun_) {
      return;
    }
    exec.endStep();
    if (brush) {
      brush->automask_cavity = savedCavity_;
      brush->reproject_uvs = savedReprojectUvs_;
    }
    begun_ = false;
  }

private:
  /** The kernel's edits collapse back into the derived layers: all grids of
   * every cage face incident to an edited vert (a grid's samples interpolate
   * every corner of its cage face) plus the grids the dab itself reached,
   * re-derived, slot-stamped (so the host's trailing scatter is a no-op) and
   * flagged for the draw caches — the same four steps as the c-api scatter. */
  void epilogue()
  {
    mesh::Mesh *cage = mr->cage();
    moved_.resize(cage->v.capacity());
    for (size_t i = 0; i < moved_.size(); i++) {
      moved_[i] = 0;
    }
    for (int v : node_.affected_verts) {
      moved_[v] = 1;
    }
    touchedGrids_.clear();
    int g0 = 0;
    for (int fi : cage->f) {
      const int c0 = cage->l.c[cage->f.l[fi]];
      int cc = c0, cn = 0;
      bool hit = false;
      do {
        hit = hit || moved_[cage->c.v[cc]] != 0;
        cc = cage->c.next[cc];
        cn++;
      } while (cc != c0);
      if (hit) {
        for (int k = 0; k < cn; k++) {
          touchedGrids_.append(g0 + k);
        }
      }
      g0 += cn;
    }
    seen_.resize(size_t(mr->refiner.gridCount()));
    for (size_t i = 0; i < seen_.size(); i++) {
      seen_[i] = 0;
    }
    for (int g : touchedGrids_) {
      seen_[g] = 1;
    }
    for (int g : dabGridsOut_) {
      if (!seen_[g]) {
        seen_[g] = 1;
        touchedGrids_.append(g);
      }
    }
    if (touchedGrids_.size() == 0) {
      return;
    }
    mr->gridAttrs().refreshFromCage(
        level, attrName, touchedGrids_.data(), int(touchedGrids_.size()));
    mr->stampSlotVertFloat4(
        level, attrName.c_str(), touchedGrids_.data(), int(touchedGrids_.size()));
    mr->noteCageAttrEdit(level, true);
    if (auto *ds = mr->drawSource()) {
      ds->markGrids(std::span<const int>(touchedGrids_.data(), touchedGrids_.size()));
    }
    if (subdiv::MultiresSlot *slot = mr->findSlot(level); slot && slot->tree) {
      for (spatial::SpatialNode *leaf : slot->tree->leaves()) {
        leaf->flag |= spatial::Spatial_UpdateGPU;
      }
    }
  }

  /** Point the kernel's writable COLOR handle at `attrName`'s layer index,
   * rebuilt when the tool changes (dabBatch is the first place the tool id is
   * known). No matching manifest entry or layer → default by-name binding. */
  void ensureOverrides(int tool, mesh::Mesh *cage)
  {
    if (tool == toolHint_) {
      return;
    }
    toolHint_ = tool;
    exec.defaultAttrOverrides.clear();
    Vector<BrushAttrManifestEntry> manifest;
    if (!brushAttrManifestFor(SculptBrushes(tool), manifest)) {
      return;
    }
    int layerIndex = -1;
    for (int i = 0; i < int(cage->v.attrs.attrs.size()); i++) {
      const mesh::AttrRef &a = cage->v.attrs.attrs[i];
      if (a.type == mesh::AttrType::FLOAT4 && a.name == attrName) {
        layerIndex = i;
        break;
      }
    }
    if (layerIndex < 0) {
      return;
    }
    for (int ai = 0; ai < int(manifest.size()); ai++) {
      const BrushAttrManifestEntry &e = manifest[ai];
      if (e.kernelWrites && (e.use & int(mesh::AttrUse::COLOR)) &&
          e.boundName.size() == 0)
      {
        exec.defaultAttrOverrides.append(BrushAttrLayerOverride{ai, layerIndex});
      }
    }
  }

  spatial::SpatialTreeMesh treeMesh_;
  spatial::SpatialNode node_;
  mesh::AttrData<math::float3> limitCo_;
  Vector<int> gridVert_;
  Vector<spatial::SpatialNode *> nodes_;
  Vector<int> dabGridsOut_;
  Vector<int> touchedGrids_;
  Vector<uint8_t> moved_;
  Vector<uint8_t> seen_;
  int toolHint_ = -1;
  bool begun_ = false;
  bool savedCavity_ = false;
  bool savedReprojectUvs_ = false;
};

} // namespace sculptcore::brush
