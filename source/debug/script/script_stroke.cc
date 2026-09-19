#include "../roughness.h"
#include "../scene.h"
#include "../state_dump.h"

#include "brush/brush_executor.h"
#include "brush/brushes/all.h"
#include "brush/grid_executor.h"
#include "brush/grid_gpu_session.h"
#include "brush/stroke_driver.h"
#include "brush/stroke_spacing.h"
#include "displace/compositor.h"
#include "displace/frames.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"
#include "mesh/attr_weights.h"
#include "mesh/attribute_builtin.h"
#include "mesh/mesh_serialize.h"
#include "mesh/mesh_shapes.h"
#include "mesh/utils/closest_point.h"
#include "mesh/utils/mesh_validate.h"
#include "mesh/utils/triangulate.h"
#include "remesh/field/cross_field.h"
#include "remesh/field/curvature.h"
#include "remesh/field/feature_tag.h"
#include "remesh/field/singularity_adjust.h"
#include "remesh/param/seamless_param.h"
#include "remesh/quantize/quantize_ilp.h"
#include "remesh/remesh.h"
#include "remesh/remesh_params.h"
#include "spatial/spatial.h"
#include "stb/stb_image.h"
#include "vdm/vdm_promote.h"
#include "vdm/vdm_splat.h"
#include "vdm/vdm_undo.h"
#ifdef SBRUSH_WEBGPU_COMPUTE
#include "webgpu/wgpu_compute.h"
#include "webgpu/wgpu_context.h"
#endif
#ifdef SBRUSH_GPU_DISPATCH
#include "vulkan/vk_compute.h"
#include "vulkan/vk_context.h"
#endif
#include "subdiv/grid_domain.h"
#include "subdiv/grid_stroke_log.h"
#include "subdiv/grid_tree.h"
#include "subdiv/grids.h"
#include "subdiv/multires.h"

#ifdef SBRUSH_GPU_DISPATCH
#include "../gpu_stroke.h"
#endif

#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include "script_util.h"

namespace sculptcore::debug_app::script {

using litestl::math::float3;
using litestl::util::Vector;

namespace {

#ifdef SBRUSH_GPU_DISPATCH
// Execute a brush stroke on the GPU via the SPIR-V compute kernel. Thin driver
// over GpuStrokeSession (source/debug/gpu_stroke.{h,cc}): begin() uploads the
// mesh and loads the kernel resolved from scene.currentTool, each origin is one
// dab, and end() reads the result back and snapshots the touched nodes for undo.
// Geometry must match the C++ path bit-modulo-fp; that is what
// `make.mjs sbrush-verify` asserts via the <brush>_ab.txt A/B scripts.
// --gpu-capture writes a JSON fixture per stroke for the Dawn/WebGPU harness.
bool runBrushStrokeGPU(Scene &scene,
                       const Vector<float3> &origins,
                       float3 normal,
                       std::string &err)
{
  GpuStrokeSession session;
  if (!scene.gpuCapturePrefix.empty()) {
    session.enableCapture(scene.gpuCapturePrefix);
  }
  if (!session.begin(scene, err)) {
    return false;
  }
  for (size_t di = 0; di < origins.size(); di++) {
    if (!session.dab(scene, origins[di], normal, err)) {
      return false;
    }
  }
  session.end(scene);
  return true;
}
#endif // SBRUSH_GPU_DISPATCH

/* Partition-independent consistency check of the incrementally-maintained
 * spatial tree against the live mesh. Returns true (consistent) or false with
 * @p msg naming the FIRST divergence (kind, index, owner id, invariant). Used
 * to localize the dyntopo undo/redo bug without a masking rebuild — see
 * documentation/plans/diff-tree-check.md. */
bool firstTreeDivergence(spatial::SpatialTree *tree, mesh::Mesh *m, std::string &msg)
{
  auto &fnode = tree->treeMesh.f.node;
  auto &vnode = tree->treeMesh.v.node;
  char buf[256];

  /* 1. Stale-owned: a dead (freed) element still owned by a leaf. Leading hang
   *    suspect — a killed face/vert whose slot is reused while the tree still
   *    owns the old occupant. */
  for (int f = 0; f < int(m->f.capacity()); f++) {
    if (m->f.freemap[f] && fnode[f] != 0) {
      std::snprintf(buf, sizeof(buf), "dead face %d still owned by leaf %d", f, fnode[f]);
      msg = buf;
      return false;
    }
  }
  for (int v = 0; v < int(m->v.capacity()); v++) {
    if (m->v.freemap[v] && vnode[v] != 0) {
      std::snprintf(buf, sizeof(buf), "dead vert %d still owned by leaf %d", v, vnode[v]);
      msg = buf;
      return false;
    }
  }

  /* 2. Dangling owner id: an owned element whose owner id resolves to no live
   *    leaf with data. */
  for (int f : m->f) {
    int id = fnode[f];
    if (id == 0) {
      continue; /* unowned handled in coverage */
    }
    spatial::SpatialNode *n = tree->node_from_id(id);
    if (!n || !n->data) {
      std::snprintf(
          buf, sizeof(buf), "live face %d owner id %d resolves to no live leaf", f, id);
      msg = buf;
      return false;
    }
  }
  for (int v : m->v) {
    int id = vnode[v];
    if (id == 0) {
      continue;
    }
    spatial::SpatialNode *n = tree->node_from_id(id);
    if (!n || !n->data) {
      std::snprintf(
          buf, sizeof(buf), "live vert %d owner id %d resolves to no live leaf", v, id);
      msg = buf;
      return false;
    }
  }

  /* 3. Leaf set <-> array agreement: every element a leaf claims must be live
   *    and point its owner array back at that leaf. */
  auto leaves = tree->leaves();
  for (auto *leaf : leaves) {
    if (!leaf->data) {
      continue;
    }
    for (int f : leaf->data->unique_faces) {
      if (m->f.freemap[f]) {
        std::snprintf(buf, sizeof(buf), "leaf %d claims dead face %d", leaf->id, f);
        msg = buf;
        return false;
      }
      if (fnode[f] != leaf->id) {
        std::snprintf(buf,
                      sizeof(buf),
                      "leaf %d claims face %d but owner array says %d",
                      leaf->id,
                      f,
                      fnode[f]);
        msg = buf;
        return false;
      }
    }
    for (int v : leaf->data->unique_verts) {
      if (m->v.freemap[v]) {
        std::snprintf(buf, sizeof(buf), "leaf %d claims dead vert %d", leaf->id, v);
        msg = buf;
        return false;
      }
      if (vnode[v] != leaf->id) {
        std::snprintf(buf,
                      sizeof(buf),
                      "leaf %d claims vert %d but owner array says %d",
                      leaf->id,
                      v,
                      vnode[v]);
        msg = buf;
        return false;
      }
    }
  }

  /* 4. Coverage: every live element is owned, and per-leaf set totals match the
   *    mesh's live counts (no dropped/duplicated elements). */
  for (int f : m->f) {
    if (fnode[f] == 0) {
      std::snprintf(buf, sizeof(buf), "live face %d unowned (dropped)", f);
      msg = buf;
      return false;
    }
  }
  for (int v : m->v) {
    if (vnode[v] == 0) {
      std::snprintf(buf, sizeof(buf), "live vert %d unowned (dropped)", v);
      msg = buf;
      return false;
    }
  }
  int ownedF = 0, ownedV = 0;
  for (auto *leaf : leaves) {
    if (!leaf->data) {
      continue;
    }
    ownedF += int(leaf->data->unique_faces.size());
    ownedV += int(leaf->data->unique_verts.size());
  }
  if (ownedF != m->f.count) {
    std::snprintf(buf,
                  sizeof(buf),
                  "leaf faces sum to %d but mesh has %d live",
                  ownedF,
                  m->f.count);
    msg = buf;
    return false;
  }
  if (ownedV != m->v.count) {
    std::snprintf(buf,
                  sizeof(buf),
                  "leaf verts sum to %d but mesh has %d live",
                  ownedV,
                  m->v.count);
    msg = buf;
    return false;
  }
  return true;
}

/* Check the incremental tree after an undo/redo step. On a clean step does NOT
 * rebuild (the tree persists and cascades, the faithful Electron repro). Only
 * on the first divergence runs a throwaway rebuild() as a positive-control
 * oracle: if the rebuilt tree is consistent the bug is in the incremental
 * undo/redo path; if it also diverges the fault is mesh-level. */
bool checkTreeVsRebuild(Scene &scene, const char *tag, std::string &err)
{
  std::string incMsg;
  if (firstTreeDivergence(scene.tree, scene.mesh, incMsg)) {
    return true; /* clean — no rebuild, let the incremental tree persist */
  }
  scene.tree->rebuild();
  std::string rebMsg;
  bool rebOk = firstTreeDivergence(scene.tree, scene.mesh, rebMsg);
  err = "incremental tree diverged after " + std::string(tag) + ": " + incMsg +
        " | rebuild oracle: " +
        (rebOk ? "CONSISTENT -> bug is in incremental undo/redo"
               : "ALSO INCONSISTENT: " + rebMsg + " -> mesh-level");
  return false;
}

/* Vertex-position snapshots for undo-fidelity checks (save_pos / assert_pos).
 * Keyed by name; mesh indices are persistent ids (IDMap is disabled), so a
 * vertex restored by undo lands back at the same index. */
std::map<std::string, std::vector<std::pair<int, float3>>> g_posSnapshots;

/* Stroke-verb epilogue when multires is active: fold the stroke's positions
 * into the store (frame-relative deltas) and record the level for the
 * level-aware undo/redo verbs. */
static void multiresStrokeEnd(Scene &scene)
{
  if (!scene.multires) {
    return;
  }
  int level = scene.multires->activeLevel();
  int changed = scene.multires->writeback(level);
  scene.mrUndoLevels.append(level);
  scene.mrRedoLevels.clear();
  std::fprintf(
      stdout, "[script] multires writeback level=%d changed=%d\n", level, changed);
}

} // namespace

bool execStrokeVerb(Scene &scene,
                    const std::string &verb,
                    ArgMap &args,
                    const char *out_dir,
                    std::string &err,
                    bool &handled)
{
  handled = true;

  if (verb == "stroke") {
    if (!scene.mesh || !scene.tree) {
      err = "stroke: no mesh/tree";
      return false;
    }
    float3 origin, normal{0, 0, 1};
    if (!parseFloat3(getArg(args, "origin"), origin)) {
      err = "stroke: missing origin=x,y,z";
      return false;
    }
    parseFloat3(getArg(args, "normal"), normal);

#ifdef SBRUSH_GPU_DISPATCH
    // GPU dispatch covers every tool with a GPU port (`@gpu` in its .sbrush;
    // gpuKernelForTool reads the same generated map GpuStrokeSession resolves
    // the kernel from). CPU-only tools fall back to the C++ executor below.
    brush::SculptBrushes t = scene.currentTool;
    bool gpuTool = brush::gpuKernelForTool(t) != nullptr;
    if ((scene.currentBackend == BrushBackend::Wgsl ||
         scene.currentBackend == BrushBackend::WgpuNative) &&
        gpuTool)
    {
      Vector<float3> origins;
      // repeat=N unifies N identical dabs into the one stroke (same as the
      // C++ branch below) — the discriminator for grab-class from-orig
      // semantics: repeated dabs with a fixed grabTo must re-base, not stack.
      int repeat = getInt(args, "repeat", 1);
      if (repeat < 1)
        repeat = 1;
      for (int i = 0; i < repeat; i++) {
        origins.append(origin);
      }
      if (!runBrushStrokeGPU(scene, origins, normal, err)) {
        return false;
      }
    } else
#endif
    {
      // One stroke verb = one stroke: advance the per-stroke generation every
      // stroke (mirrors the TS app's ++nextStrokeGen). It must be nonzero even in
      // accumulate mode — grab/kelvinlet always stamp the base, and gen 0 collides
      // with the fresh .brush.disp.gen default, which would make every stamped vert
      // read as unstamped. `repeat`ed dabs below share this one stamp.
      uint32_t gen = ++scene.strokeGen;
      int repeat = getInt(args, "repeat", 1);
      if (repeat < 1)
        repeat = 1;
      // One stroke verb = one meshlog step of `repeat` unified dabs through the
      // executor (same path as the TS app). Previously dyntopo was a separate undo
      // step, so one undo couldn't revert a stroke (tools/repro/repro_single_undo.txt).
      brush::CommandExecutor exec(scene.tree, &scene.brush);
      exec.meshLog = &scene.meshLog;
      exec.ctx.renderMatrix = scene.renderMatrix;
      if (scene.useCsrNeighbors) {
        exec.neighborMode = brush::CommandExecutor::NeighborMode::Csr;
      }
      exec.setNonAccum(scene.nonAccum);
      exec.setStrokeGen(int(gen));
      // layer=<name>: retarget the kernel's first declared attr handle (the
      // layerdraw brush's `slayer`) at the named sculpt layer.
      std::string layerName = getArg(args, "layer", "");
      if (!layerName.empty()) {
        int li = scene.mesh->findSculptLayer(litestl::util::string(layerName.c_str()));
        int attrIdx = li >= 0 ? scene.mesh->sculptLayerAttrIndex(li) : -1;
        if (attrIdx < 0) {
          err = "stroke: unknown sculpt layer '" + layerName + "'";
          return false;
        }
        exec.defaultAttrOverrides.append(brush::BrushAttrLayerOverride{0, attrIdx});
      }
      dyntopo::DynTopoParams *dtp = scene.dyntopoEnabled ? &scene.dyntopoParams : nullptr;
      // rough=1: print the §9.1 noise metrics after every dab, inside the one
      // stroke — a separate `roughness` verb per dab is impossible, since each
      // `stroke` verb bumps strokeGen and so resets the base.
      bool roughTrace = getBool(args, "rough", false);
      Vector<float3> roughCenters;
      char roughTag[64];
      exec.beginStep(scene.dyntopoEnabled);
      for (int i = 0; i < repeat; i++) {
        // Each repeat is a new logical dab's primary image (mirrors the TS
        // app's setGrabAccumAdd(false)): grab-class kernels re-base from the
        // stroke-start position per dab instead of adding with an idle
        // dab counter (curDabGen 0 == fresh stamps -> add mode).
        exec.setGrabAccumAdd(false);
        exec.applyDab(scene.currentTool,
                      origin,
                      normal,
                      scene.brush.radius,
                      dtp,
                      scene.dyntopoSeed + uint32_t(i));
        scene.cumSplits += exec.lastDynTopoStats.splits;
        scene.cumCollapses += exec.lastDynTopoStats.collapses;
        scene.cumFlips += exec.lastDynTopoStats.flips;
        if (roughTrace) {
          roughCenters.append(origin);
          std::snprintf(roughTag, sizeof(roughTag), "dab=%d", i);
          reportRoughness(
              scene, roughTag, roughCenters, scene.brush.radius, float3(0, 0, 1), 0.0f);
        }
      }
      if (scene.dyntopoEnabled) {
        exec.endDynTopoStroke();
      }
      /* Mechanism B: fold an incremental compaction into this still-open stroke
       * step when the layout has fragmented past the threshold. */
      if (scene.autoDefragRatio > 0.0) {
        auto t0 = std::chrono::steady_clock::now();
        bool did = scene.meshLog.compactIfFragmented(scene.tree, scene.autoDefragRatio);
        if (did) {
          std::printf("[auto_defrag] compacted at stroke end in %.2fms\n",
                      std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count());
          std::fflush(stdout);
        }
      }
      exec.endStep();
    }
    // Both backends: the stroke verb leaves the tree current. The GPU path only
    // flags its touched nodes (normals/GPU/bounds) in session.end(), so without
    // this its normals stay stale.
    scene.tree->update(&scene.gpu);
    multiresStrokeEnd(scene);

    scene.lastStroke.valid = true;
    scene.lastStroke.origin = origin;
    scene.lastStroke.normal = normal;
    scene.lastStroke.radius = scene.brush.radius;
    scene.lastStroke.centers.clear();
    scene.lastStroke.centers.append(origin);
    return true;
  }
  if (verb == "stroke_folded") {
    /* Faithful repro of the TS/Electron interactive stroke (SculptPaintOp): ONE
     * meshlog step holding many interleaved per-dab topo chunks + brush-deform
     * simple chunks — unlike `stroke`, which records a single isolated topo step
     * + a separate deform step. This is the structure the redo hang needs.
     *   stroke_folded p1=x,y,z [p2=x,y,z] [normal=x,y,z] [dabs=N]
     * Dabs are linearly spaced p1->p2 (a swipe); p2 defaults to p1 (in place). */
    if (!scene.mesh || !scene.tree) {
      err = "stroke_folded: no mesh/tree";
      return false;
    }
    float3 p1, p2, normal{0, 0, 1};
    if (!parseFloat3(getArg(args, "p1"), p1) && !parseFloat3(getArg(args, "origin"), p1))
    {
      err = "stroke_folded: missing p1=x,y,z";
      return false;
    }
    if (!parseFloat3(getArg(args, "p2"), p2)) {
      p2 = p1;
    }
    parseFloat3(getArg(args, "normal"), normal);
    int dabs = getInt(args, "dabs", getInt(args, "repeat", 1));
    if (dabs < 1)
      dabs = 1;

    // Nonzero every stroke (see the stroke verb): grab orig-stamps in any mode.
    uint32_t gen = ++scene.strokeGen;

    brush::CommandExecutor exec(scene.tree, &scene.brush);
    exec.meshLog = &scene.meshLog;
    exec.ctx.renderMatrix = scene.renderMatrix;
    if (scene.useCsrNeighbors) {
      exec.neighborMode = brush::CommandExecutor::NeighborMode::Csr;
    }
    exec.setNonAccum(scene.nonAccum);
    exec.setStrokeGen(int(gen));
    // One step for the whole stroke; beginStep pushes the first topo chunk when
    // dyntopo is on, matching meshLog.beginStep(hasDyntopo) on the TS side.
    exec.beginStep(scene.dyntopoEnabled);
    for (int i = 0; i < dabs; i++) {
      float t = dabs > 1 ? float(i) / float(dabs - 1) : 0.0f;
      float3 c;
      for (int k = 0; k < 3; k++) {
        c[k] = p1[k] + (p2[k] - p1[k]) * t;
      }
      // One unified dab (dyntopo → deform → per-dab topo-chunk seal), matching
      // SculptPaintOp.applyDab on the TS side.
      dyntopo::DynTopoParams *dtp = scene.dyntopoEnabled ? &scene.dyntopoParams : nullptr;
      exec.applyDab(scene.currentTool,
                    c,
                    normal,
                    scene.brush.radius,
                    dtp,
                    scene.dyntopoSeed + uint32_t(i));
    }
    if (scene.dyntopoEnabled) {
      exec.endDynTopoStroke();
    }
    exec.endStep();
    scene.tree->update(&scene.gpu);
    multiresStrokeEnd(scene);

    scene.lastStroke.valid = true;
    scene.lastStroke.origin = p1;
    scene.lastStroke.normal = normal;
    scene.lastStroke.radius = scene.brush.radius;
    return true;
  }
  if (verb == "stroke_path") {
    if (!scene.mesh || !scene.tree) {
      err = "stroke_path: no mesh/tree";
      return false;
    }
    float3 p1, p2, normal{0, 0, 1};
    if (!parseFloat3(getArg(args, "p1"), p1) || !parseFloat3(getArg(args, "p2"), p2)) {
      err = "stroke_path: missing p1/p2";
      return false;
    }
    parseFloat3(getArg(args, "normal"), normal);

    // Collect dab origins first so both backends drive the identical sequence.
    Vector<float3> origins;
    const char *spacingArg = getArg(args, "spacing");
    if (spacingArg) {
      /* spacing= overrides fixed-step mode: emit dabs every
       * radius * spacing world-space units along the segment. */
      float spacingFrac = float(std::atof(spacingArg));
      brush::StrokeSpacer spacer;
      spacer.spacing = scene.brush.radius * spacingFrac;
      auto collect = [&](float3 o) { origins.append(o); };
      spacer.advance(p1, collect);
      spacer.advance(p2, collect);
    } else {
      int steps = getInt(args, "steps", 8);
      if (steps < 1) {
        steps = 1;
      }
      for (int i = 0; i < steps; i++) {
        float t = (steps == 1) ? 0.0f : float(i) / float(steps - 1);
        origins.append(p1 * (1.0f - t) + p2 * t);
      }
    }

#ifdef SBRUSH_GPU_DISPATCH
    // Same predicate as the single-dab verb above, except GRAB stays on the
    // C++ executor here — deliberate: a grab-class dab re-bases from the
    // stroke-start snapshot, and this verb's multi-origin path would stack
    // the GPU dabs instead.
    brush::SculptBrushes t = scene.currentTool;
    bool gpuTool =
        brush::gpuKernelForTool(t) != nullptr && t != brush::SculptBrushes::GRAB;
    if ((scene.currentBackend == BrushBackend::Wgsl ||
         scene.currentBackend == BrushBackend::WgpuNative) &&
        gpuTool)
    {
      if (!runBrushStrokeGPU(scene, origins, normal, err)) {
        return false;
      }
    } else
#endif
    {
      // A path is one stroke = one meshlog step of unified dabs through the executor.
      // Advance the per-stroke generation once so every dab measures from the same
      // stroke-start snapshot; nonzero every stroke so grab orig-stamps in any mode.
      uint32_t gen = ++scene.strokeGen;
      brush::CommandExecutor exec(scene.tree, &scene.brush);
      exec.meshLog = &scene.meshLog;
      exec.ctx.renderMatrix = scene.renderMatrix;
      exec.setNonAccum(scene.nonAccum);
      exec.setStrokeGen(int(gen));
      dyntopo::DynTopoParams *dtp = scene.dyntopoEnabled ? &scene.dyntopoParams : nullptr;
      // rough=1: per-dab noise trace over the swept-so-far region (see `stroke`).
      bool roughTrace = getBool(args, "rough", false);
      Vector<float3> roughCenters;
      char roughTag[64];
      exec.beginStep(scene.dyntopoEnabled);
      for (size_t i = 0; i < origins.size(); i++) {
        exec.applyDab(scene.currentTool,
                      origins[i],
                      normal,
                      scene.brush.radius,
                      dtp,
                      scene.dyntopoSeed + uint32_t(i));
        if (roughTrace) {
          roughCenters.append(origins[i]);
          std::snprintf(roughTag, sizeof(roughTag), "dab=%zu", i);
          reportRoughness(
              scene, roughTag, roughCenters, scene.brush.radius, float3(0, 0, 1), 0.0f);
        }
      }
      if (scene.dyntopoEnabled) {
        exec.endDynTopoStroke();
      }
      exec.endStep();
    }
    // Both backends leave the tree current (see the stroke verb).
    scene.tree->update(&scene.gpu);
    multiresStrokeEnd(scene);

    scene.lastStroke.valid = true;
    scene.lastStroke.origin = p2;
    scene.lastStroke.normal = normal;
    scene.lastStroke.radius = scene.brush.radius;
    scene.lastStroke.centers.clear();
    for (const float3 &o : origins) {
      scene.lastStroke.centers.append(o);
    }
    return true;
  }
  if (verb == "stroke_screen") {
    //   stroke_screen p1=x,y [p2=x,y] [steps=N] [method=..] [rough=0/1]
    // A stroke as a pointer drag in view pixels, sampled by the same
    // BrushStrokeDriver the app uses — it raycasts, so no world p/normal here.
    if (!scene.mesh || !scene.tree) {
      err = "stroke_screen: no mesh/tree";
      return false;
    }
    litestl::math::float2 p1, p2;
    if (!parseFloat2(getArg(args, "p1"), p1)) {
      err = "stroke_screen: missing p1=x,y";
      return false;
    }
    if (!parseFloat2(getArg(args, "p2"), p2)) {
      p2 = p1;
    }
    int steps = getInt(args, "steps", 8);
    if (steps < 1) {
      steps = 1;
    }

    brush::BrushStrokeDriver driver(scene.tree);
    std::string method = getArg(args, "method", "path");
    if (method == "anchored") {
      driver.strokeMethod = brush::StrokeMethod::Anchored;
    } else if (method == "dragdot") {
      driver.strokeMethod = brush::StrokeMethod::DragDot;
    } else if (method != "path") {
      err = "stroke_screen: unknown method '" + method + "'";
      return false;
    }
    scene.configureStrokeDriver(driver);

    // Nonzero every stroke (see the stroke verb): grab orig-stamps in any mode.
    uint32_t gen = ++scene.strokeGen;
    brush::CommandExecutor exec(scene.tree, &scene.brush);
    exec.meshLog = &scene.meshLog;
    exec.ctx.renderMatrix = scene.renderMatrix;
    if (scene.useCsrNeighbors) {
      exec.neighborMode = brush::CommandExecutor::NeighborMode::Csr;
    }
    exec.setNonAccum(scene.nonAccum);
    exec.setStrokeGen(int(gen));
    dyntopo::DynTopoParams *dtp = scene.dyntopoEnabled ? &scene.dyntopoParams : nullptr;
    bool roughTrace = getBool(args, "rough", false);
    char roughTag[64];

    Vector<float3> centers;
    float3 lastNormal{0, 0, 1};
    bool stepOpen = false;
    // The driver hands back a variable number of dabs per poll (it walks the
    // spline at brush spacing, not at the pointer's sample rate), so drain it
    // after every event and again after end() flushes the trailing segment.
    auto drain = [&]() {
      int n = driver.poll();
      for (int i = 0; i < n; i++) {
        const brush::DabSample *ps = driver.sampleAt(i);
        if (!ps) {
          continue;
        }
        // Open the step on the first dab, not before: a drag that misses the
        // surface entirely must not leave an empty step for `undo` to eat.
        if (!stepOpen) {
          exec.beginStep(scene.dyntopoEnabled);
          stepOpen = true;
        }
        // No object matrix (see Scene::configureStrokeDriver), so the driver's
        // object-local sample space is world space.
        float3 center(ps->p[0], ps->p[1], ps->p[2]);
        exec.applyDab(scene.currentTool,
                      center,
                      ps->vec,
                      ps->radius,
                      dtp,
                      scene.dyntopoSeed + uint32_t(centers.size()));
        scene.cumSplits += exec.lastDynTopoStats.splits;
        scene.cumCollapses += exec.lastDynTopoStats.collapses;
        scene.cumFlips += exec.lastDynTopoStats.flips;
        centers.append(center);
        lastNormal = ps->vec;
        if (roughTrace) {
          std::snprintf(roughTag, sizeof(roughTag), "dab=%zu", centers.size() - 1);
          reportRoughness(
              scene, roughTag, centers, scene.brush.radius, float3(0, 0, 1), 0.0f);
        }
      }
    };

    for (int i = 0; i < steps; i++) {
      float t = (steps == 1) ? 0.0f : float(i) / float(steps - 1);
      litestl::math::float2 p = p1 * (1.0f - t) + p2 * t;
      driver.push(p[0],
                  p[1],
                  /*pressure=*/1.0f,
                  /*tiltX=*/0.0f,
                  /*tiltY=*/0.0f,
                  /*twist=*/0.0f,
                  scene.brush.invert,
                  /*useAltBrush=*/false,
                  scene.brush.radius,
                  scene.brush.strength,
                  scene.brush.spacing);
      drain();
    }
    driver.end();
    drain();

    if (stepOpen) {
      if (scene.dyntopoEnabled) {
        exec.endDynTopoStroke();
      }
      exec.endStep();
    }

    scene.tree->update(&scene.gpu);
    multiresStrokeEnd(scene);

    if (centers.size() == 0) {
      std::printf("[stroke_screen] no dabs (every pointer event missed the surface)\n");
      std::fflush(stdout);
      return true;
    }
    scene.lastStroke.valid = true;
    scene.lastStroke.origin = centers[centers.size() - 1];
    scene.lastStroke.normal = lastNormal;
    scene.lastStroke.radius = scene.brush.radius;
    scene.lastStroke.centers.clear();
    for (const float3 &c : centers) {
      scene.lastStroke.centers.append(c);
    }
    return true;
  }
  if (verb == "preview_stroke_path") {
    // Exercises the live-mutating preview/rollback primitive (MeshLog::
    // beginPreviewDab/rollbackPreviewDab, step 2a of the Anchored/Drag Dot
    // plan): every point but the last is applied as a rollback-able preview
    // dab that is undone before the next preview, and only the final point
    // is left committed -- one meshlog step, mirroring a single anchored or
    // drag-dot gesture where the pointer moves through many preview
    // positions before release. CPU executor only (no GPU dispatch branch);
    // GPU preview wiring is step 3 of the plan, not yet implemented.
    if (!scene.mesh || !scene.tree) {
      err = "preview_stroke_path: no mesh/tree";
      return false;
    }
    float3 p1, p2, normal{0, 0, 1};
    if (!parseFloat3(getArg(args, "p1"), p1) || !parseFloat3(getArg(args, "p2"), p2)) {
      err = "preview_stroke_path: missing p1/p2";
      return false;
    }
    parseFloat3(getArg(args, "normal"), normal);

    int steps = getInt(args, "steps", 8);
    if (steps < 1) {
      steps = 1;
    }
    Vector<float3> origins;
    for (int i = 0; i < steps; i++) {
      float t = (steps == 1) ? 0.0f : float(i) / float(steps - 1);
      origins.append(p1 * (1.0f - t) + p2 * t);
    }

    uint32_t gen = ++scene.strokeGen;
    brush::CommandExecutor exec(scene.tree, &scene.brush);
    exec.meshLog = &scene.meshLog;
    exec.ctx.renderMatrix = scene.renderMatrix;
    exec.setNonAccum(scene.nonAccum);
    exec.setStrokeGen(int(gen));
    dyntopo::DynTopoParams *dtp = scene.dyntopoEnabled ? &scene.dyntopoParams : nullptr;
    exec.beginStep(scene.dyntopoEnabled);
    for (size_t i = 0; i < origins.size(); i++) {
      bool isLast = (i + 1 == origins.size());
      exec.beginPreviewDab(origins[i], scene.brush.radius);
      exec.applyDab(scene.currentTool,
                    origins[i],
                    normal,
                    scene.brush.radius,
                    dtp,
                    scene.dyntopoSeed + uint32_t(i));
      // Per-dab spatial-query update mirrors a live pointer-move gesture,
      // where the tree must stay current between preview dabs for the next
      // dab's picking/raycast.
      scene.tree->updateQueries();
      if (!isLast) {
        exec.rollbackPreviewDab();
        scene.tree->updateQueries();
      }
    }
    if (scene.dyntopoEnabled) {
      exec.endDynTopoStroke();
    }
    exec.endStep();
    scene.tree->update(&scene.gpu);
    multiresStrokeEnd(scene);

    scene.lastStroke.valid = true;
    scene.lastStroke.origin = p2;
    scene.lastStroke.normal = normal;
    scene.lastStroke.radius = scene.brush.radius;
    return true;
  }
  if (verb == "assert_verts") {
    int n = getInt(args, "n", -1);
    int actual = scene.mesh ? scene.mesh->v.count : 0;
    if (n != actual) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "assert_verts: expected %d got %d", n, actual);
      err = buf;
      return false;
    }
    return true;
  }
  if (verb == "time_gather") {
    /* Time a whole-mesh gather (read every leaf's verts' co+no, leaf-by-leaf) —
     * the layout-sensitive streaming pattern of GPU buffer fill / normal recompute,
     * where the working set exceeds cache (a single brush dab fits cache regardless).
     * Run fragmented vs after `reorder` to measure the DRAM-locality cost. */
    if (!scene.tree || !scene.mesh) {
      err = "time_gather: no mesh/tree";
      return false;
    }
    int passes = getInt(args, "passes", 50);
    mesh::Mesh &m = *scene.mesh;
    util::Vector<spatial::SpatialNode *> leaves = scene.tree->leaves();
    double sink = 0.0;
    auto t0 = std::chrono::steady_clock::now();
    for (int p = 0; p < passes; p++) {
      for (spatial::SpatialNode *leaf : leaves) {
        for (int v : leaf->unique_verts()) {
          float3 co = m.v.co[v];
          float3 no = m.v.no[v];
          sink += double(co[0] + co[1] + co[2] + no[0] + no[1] + no[2]);
        }
      }
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    int64_t total = 0;
    for (spatial::SpatialNode *leaf : leaves)
      total += leaf->unique_verts().size();
    std::printf("[time_gather] leaves=%d verts/pass=%lld passes=%d total=%.2fms "
                "per_pass=%.4fms (sink=%.1f)\n",
                int(leaves.size()),
                (long long)total,
                passes,
                ms,
                ms / double(passes),
                sink);
    std::fflush(stdout);
    return true;
  }
  if (verb == "save_pos") {
    if (!scene.mesh) {
      err = "save_pos: no mesh";
      return false;
    }
    std::string name = getArg(args, "id", "default");
    auto &snap = g_posSnapshots[name];
    snap.clear();
    for (int v : scene.mesh->v) {
      snap.emplace_back(v, scene.mesh->v.co[v]);
    }
    std::fprintf(
        stdout, "[script] save_pos id=%s verts=%zu\n", name.c_str(), snap.size());
    return true;
  }
  if (verb == "assert_pos") {
    if (!scene.mesh) {
      err = "assert_pos: no mesh";
      return false;
    }
    std::string name = getArg(args, "id", "default");
    float eps = getFloat(args, "eps", 1e-5f);
    auto it = g_posSnapshots.find(name);
    if (it == g_posSnapshots.end()) {
      err = "assert_pos: no snapshot '" + name + "'";
      return false;
    }
    int dead = 0, moved = 0, worstIdx = -1, firstBad = -1;
    float worst = 0.0f;
    for (auto &pr : it->second) {
      int v = pr.first;
      if (v >= int(scene.mesh->v.capacity()) || scene.mesh->v.freemap[v]) {
        dead++;
        if (firstBad < 0)
          firstBad = v;
        continue;
      }
      float d = (scene.mesh->v.co[v] - pr.second).length();
      if (d > eps) {
        moved++;
        if (firstBad < 0)
          firstBad = v;
        if (d > worst) {
          worst = d;
          worstIdx = v;
        }
      }
    }
    std::fprintf(
        stdout,
        "[script] assert_pos id=%s checked=%zu dead=%d moved=%d worst=%g (vert %d)\n",
        name.c_str(),
        it->second.size(),
        dead,
        moved,
        worst,
        worstIdx);
    if (dead > 0 || moved > 0) {
      char buf[256];
      std::snprintf(buf,
                    sizeof(buf),
                    "assert_pos: %d dead, %d moved (worst %g at vert %d, first %d)",
                    dead,
                    moved,
                    worst,
                    worstIdx,
                    firstBad);
      err = buf;
      /* soft=1 reports the divergence but lets the script continue. */
      return getBool(args, "soft", false);
    }
    return true;
  }
  if (verb == "undo") {
    if (scene.multires) {
      /* Level-aware undo: the step was recorded on the level it was stroked
       * at — auto-switch there first, then re-sync the store from the undone
       * positions (writeback's baseline diff touches only the stroke verts). */
      if (scene.mrUndoLevels.size() == 0) {
        return true; /* nothing multires-recorded to undo */
      }
      int level = scene.mrUndoLevels.pop_back();
      if (level != scene.multires->activeLevel()) {
        /* propagate=false: this switch replays history, and a downward one
         * would push the very detail we are about to undo into the level
         * below. The debt survives for the user's next real switch. */
        scene.multires->setActiveLevel(level, /*propagate=*/false);
        scene.attachMultiresLevel();
      }
      scene.mesh->thawTopo();
      scene.meshLog.undo(scene.mesh, scene.tree);
      scene.multires->writeback(level);
      scene.mrRedoLevels.append(level);
      if (!checkTreeVsRebuild(scene, "undo", err)) {
        return false;
      }
      return true;
    }
    if (scene.mesh && scene.tree) {
      /* The meshlog recorded topology in the thawed state; a brush stroke
       * leaves the mesh frozen (live links freed/CSR-rebuilt), which would
       * mismatch the recorded links during replay. Thaw first. */
      scene.mesh->thawTopo();
      scene.meshLog.undo(scene.mesh, scene.tree);
      /* No masking rebuild: verify the incrementally-maintained tree against
       * the live mesh so it persists and cascades exactly as in Electron. */
      if (!checkTreeVsRebuild(scene, "undo", err)) {
        return false;
      }
    }
    return true;
  }
  if (verb == "redo") {
    if (scene.multires) {
      if (scene.mrRedoLevels.size() == 0) {
        return true;
      }
      int level = scene.mrRedoLevels.pop_back();
      if (level != scene.multires->activeLevel()) {
        /* propagate=false, as in undo: replaying history, not a user switch. */
        scene.multires->setActiveLevel(level, /*propagate=*/false);
        scene.attachMultiresLevel();
      }
      scene.mesh->thawTopo();
      scene.meshLog.redo(scene.mesh, scene.tree);
      scene.multires->writeback(level);
      scene.mrUndoLevels.append(level);
      if (!checkTreeVsRebuild(scene, "redo", err)) {
        return false;
      }
      return true;
    }
    if (scene.mesh && scene.tree) {
      scene.mesh->thawTopo();
      scene.meshLog.redo(scene.mesh, scene.tree);
      if (!checkTreeVsRebuild(scene, "redo", err)) {
        return false;
      }
    }
    return true;
  }
  if (verb == "check_tree") {
    /* Assert incremental tree consistency at an arbitrary script point (no-op
     * when clean). Handy for bisecting a divergence. */
    if (scene.mesh && scene.tree) {
      if (!checkTreeVsRebuild(scene, "check_tree", err)) {
        return false;
      }
    }
    return true;
  }

  handled = false;
  return true;
}

} // namespace sculptcore::debug_app::script
