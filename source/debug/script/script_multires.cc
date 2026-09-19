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

/* Grids-store displacement snapshots (save_disp / assert_disp): one level's
 * disp channel flattened in (grid, v, u, component) order. The S4 ride-along
 * gate compares these bitwise. */
std::map<std::string, std::vector<float>> g_dispSnapshots;

/* Flatten a level's disp channel (deterministic order) for the verbs above. */
static void gatherDisp(subdiv::Multires &mr, int level, std::vector<float> &out)
{
  out.clear();
  int w = subdiv::GridsStore::sideForLevel(level) + 1;
  for (int g = 0; g < mr.store.gridCount(); g++) {
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        const float *d = mr.store.elem(level, 0, g, u, v);
        out.push_back(d[0]);
        out.push_back(d[1]);
        out.push_back(d[2]);
      }
    }
  }
}

// SBRUSH_WGSL_DIR arrives unquoted from CMake (see gpu_stroke.cc's twin).
#define SBRUSH_SCRIPT_STRINGIZE_(x) #x
#define SBRUSH_SCRIPT_STRINGIZE(x) SBRUSH_SCRIPT_STRINGIZE_(x)

/* Grids-native stroke session for the grid_* verbs, owned by the Scene
 * (torn down with the multires stack). Rebuilt when the level changes;
 * re-synced to the current domain per use. */
static bool ensureGridSession(Scene &scene, int level, std::string &err)
{
  if (!scene.multires) {
    err = "grid verbs: multires not active (run multires_init)";
    return false;
  }
  if (level < 1 || level > scene.multires->maxLevel()) {
    err = "grid verbs: bad level";
    return false;
  }
  if (scene.gridExec && scene.gridLevel != level) {
    scene.clearGridSession();
  }
  if (!scene.gridExec) {
    scene.gridLevel = level;
    scene.gridLog = litestl::alloc::New<subdiv::GridStrokeLog>("grid verb log");
    scene.gridExec = litestl::alloc::New<brush::GridBrushExecutor>(
        "grid verb exec", scene.multires->gridDomain(level), &scene.brush, scene.gridLog);
  } else {
    // Fold points (mesh-path writeback, level ops) drop the domain — re-bind.
    subdiv::GridLevelDomain *d = scene.multires->gridDomain(level);
    if (d != scene.gridExec->domain) {
      scene.gridExec->attach(d);
    }
  }
  return true;
}

/* Interim ride-along mirror — the shared brush::gridsMirrorToSlot helper. */
static void gridMirrorSync(Scene &scene, int level, litestl::util::Vector<int> &verts)
{
  brush::gridsMirrorToSlot(
      scene.multires, level, std::span<const int>(verts.data(), verts.size()));
}

} // namespace

bool execMultiresVerb(Scene &scene,
                    const std::string &verb,
                    ArgMap &args,
                    const char *out_dir,
                    std::string &err,
                    bool &handled)
{
  handled = true;

  if (verb == "dyntopo_stats") {
    /* Print cumulative dyntopo op counts since the last reset; `reset=1` zeroes. */
    std::printf("[dyntopo_stats] splits=%lld collapses=%lld flips=%lld\n",
                (long long)scene.cumSplits,
                (long long)scene.cumCollapses,
                (long long)scene.cumFlips);
    std::fflush(stdout);
    if (getBool(args, "reset", false)) {
      scene.cumSplits = scene.cumCollapses = scene.cumFlips = 0;
    }
    return true;
  }
  if (verb == "auto_defrag") {
    /* Enable mechanism-B stroke-end auto-compaction: `auto_defrag ratio=F`
     * (vert page-spread threshold; 0 disables). */
    scene.autoDefragRatio = getFloat(args, "ratio", 3.0f);
    return true;
  }
  if (verb == "reorder") {
    /* Full locality reorder (compaction) via the rebuild path. */
    if (!scene.tree) {
      err = "reorder: no spatial tree";
      return false;
    }
    auto t0 = std::chrono::steady_clock::now();
    scene.tree->reorderForLocality();
    auto t1 = std::chrono::steady_clock::now();
    std::printf("[reorder] full(rebuild) apply=%.2fms\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count());
    std::fflush(stdout);
    return true;
  }
  if (verb == "reorder_inc") {
    /* Incremental locality reorder (mechanism B): same permutation, but remap the
     * tree's cached indices in place instead of rebuilding. Should match
     * `reorder`'s locality gain at a fraction of the apply cost. */
    if (!scene.tree) {
      err = "reorder_inc: no spatial tree";
      return false;
    }
    util::Vector<int> vmap, emap, cmap, lmap, fmap;
    scene.tree->computeLocalityMaps(vmap, emap, cmap, lmap, fmap);
    auto t0 = std::chrono::steady_clock::now();
    scene.tree->applyReorderIncremental(vmap, emap, cmap, lmap, fmap);
    auto t1 = std::chrono::steady_clock::now();
    std::printf("[reorder_inc] incremental apply=%.2fms\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count());
    std::fflush(stdout);
    return true;
  }
  if (verb == "reorder_partial") {
    /* Phase 1b: partial (region-scoped) locality map, but still applied via the
     * full applyReorderIncremental. Selects fragmented leaves (thresh=ratio),
     * builds a mostly-identity closed permutation over just their slots, prints
     * the moved-vs-total counts (the "mostly identity" proof). Bracket with
     * frag_stats to measure whether frag holds vs the full reorder_inc. */
    if (!scene.tree) {
      err = "reorder_partial: no spatial tree";
      return false;
    }
    double thresh = getFloat(args, "thresh", 2.0f);
    util::Vector<sculptcore::spatial::SpatialNode *> dirty;
    scene.tree->selectFragmentedLeaves(thresh, dirty);

    bool scoped = getInt(args, "scoped", 1) != 0;
    util::Vector<int> vmap, emap, cmap, lmap, fmap;
    util::Vector<int> moved[5];
    auto tb0 = std::chrono::steady_clock::now();
    scene.tree->computeLocalityMapsPartial(dirty, vmap, emap, cmap, lmap, fmap, moved);
    auto tb1 = std::chrono::steady_clock::now();

    std::printf("[reorder_partial] thresh=%.2f scoped=%d dirtyLeaves=%d/%d build=%.2fms "
                "moved v=%d e=%d c=%d l=%d f=%d (liveV=%d liveF=%d)\n",
                thresh,
                int(scoped),
                int(dirty.size()),
                scene.tree->fragmentationStats().leaves,
                std::chrono::duration<double, std::milli>(tb1 - tb0).count(),
                int(moved[0].size()),
                int(moved[1].size()),
                int(moved[2].size()),
                int(moved[3].size()),
                int(moved[4].size()),
                scene.mesh->v.count,
                scene.mesh->f.count);

    auto t0 = std::chrono::steady_clock::now();
    if (scoped) {
      scene.tree->applyReorderIncremental(
          vmap, emap, cmap, lmap, fmap, moved[0], moved[1], moved[2], moved[3], moved[4]);
    } else {
      scene.tree->applyReorderIncremental(vmap, emap, cmap, lmap, fmap);
    }
    auto t1 = std::chrono::steady_clock::now();
    std::printf("[reorder_partial] apply=%.2fms\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count());
    std::fflush(stdout);
    return true;
  }
  if (verb == "frag_stats") {
    /* Print DRAM-locality fragmentation of the current tree (verts/faces:
     * distinct attribute pages per leaf vs ideal; ratio 1.0 = compact). */
    if (!scene.tree) {
      err = "frag_stats: no spatial tree";
      return false;
    }
    auto s = scene.tree->fragmentationStats();
    std::printf("[frag_stats] leaves=%d\n", s.leaves);
    std::printf(
        "[frag_stats]   verts: count=%lld pagesActual=%lld pagesIdeal=%lld ratio=%.3f\n",
        (long long)s.vertCount,
        (long long)s.vertPagesActual,
        (long long)s.vertPagesIdeal,
        s.vertRatio);
    std::printf(
        "[frag_stats]   faces: count=%lld pagesActual=%lld pagesIdeal=%lld ratio=%.3f\n",
        (long long)s.faceCount,
        (long long)s.facePagesActual,
        (long long)s.facePagesIdeal,
        s.faceRatio);
    std::fflush(stdout);
    return true;
  }
  if (verb == "assert_aabb") {
    if (!scene.mesh) {
      err = "assert_aabb: no mesh";
      return false;
    }
    float3 emn, emx;
    if (!parseFloat3(getArg(args, "min"), emn) || !parseFloat3(getArg(args, "max"), emx))
    {
      err = "assert_aabb: need min=x,y,z max=x,y,z";
      return false;
    }
    float eps = getFloat(args, "eps", 1e-4f);
    float3 amn, amx;
    scene.mesh->calcAABB(&amn, &amx);
    for (int i = 0; i < 3; i++) {
      if (std::fabs(amn[i] - emn[i]) > eps || std::fabs(amx[i] - emx[i]) > eps) {
        char buf[256];
        std::snprintf(buf,
                      sizeof(buf),
                      "assert_aabb mismatch: got [%g,%g,%g]..[%g,%g,%g]",
                      amn[0],
                      amn[1],
                      amn[2],
                      amx[0],
                      amx[1],
                      amx[2]);
        err = buf;
        return false;
      }
    }
    return true;
  }
  if (verb == "multires_init") {
    /* multires_init levels=N [level=L] [budget=B] [leaf_limit=..]
     * [depth_limit=..] [gpu_tri_target=..]: convert the current mesh into a
     * multires cage and attach level L (default: finest). The old mesh becomes
     * the cage (owned by the scene); mesh/tree become views of the active
     * level's slot. The three tree knobs stand in for build_spatial, which
     * refuses to run while the stack owns the trees; 0 keeps the
     * multiresAutoTune default for that knob. */
    if (!scene.mesh) {
      err = "multires_init: no mesh";
      return false;
    }
    if (scene.multires) {
      err = "multires_init: multires already active";
      return false;
    }
    int levels = getInt(args, "levels", 2);
    int level = getInt(args, "level", levels);
    if (levels < 1 || level < 1 || level > levels) {
      err = "multires_init: bad levels=/level=";
      return false;
    }
    mesh::Mesh *cage = scene.mesh;
    scene.mesh = nullptr;
    if (scene.tree) {
      litestl::alloc::Delete(scene.tree);
      scene.tree = nullptr;
    }
    scene.multiresCage = cage;
    scene.multires = litestl::alloc::New<subdiv::Multires>("debug multires");
    scene.multires->lruBudget = getInt(args, "budget", 3);
    scene.multires->treeLeafLimit = getInt(args, "leaf_limit", 0);
    scene.multires->treeDepthLimit = getInt(args, "depth_limit", 0);
    scene.multires->treeGpuTriTarget = getInt(args, "gpu_tri_target", 0);
    scene.multires->init(*cage, levels);
    scene.multires->setActiveLevel(level);
    scene.attachMultiresLevel();
    std::fprintf(stdout,
                 "[script] multires_init levels=%d level=%d verts=%d faces=%d\n",
                 levels,
                 level,
                 scene.mesh->v.count,
                 scene.mesh->f.count);
    return true;
  }
  if (verb == "multires_level") {
    /* multires_level level=L: write back the active level, switch to L. */
    if (!scene.multires) {
      err = "multires_level: multires not active (run multires_init)";
      return false;
    }
    int level = getInt(args, "level", 0);
    if (level < 1 || level > scene.multires->maxLevel()) {
      err = "multires_level: bad level=";
      return false;
    }
    scene.multires->setActiveLevel(level);
    scene.attachMultiresLevel();
    std::fprintf(stdout,
                 "[script] multires_level level=%d verts=%d\n",
                 level,
                 scene.mesh->v.count);
    return true;
  }
  if (verb == "multires_refit") {
    /* multires_refit [level=active]: least-squares-fit level-1 to level. */
    if (!scene.multires) {
      err = "multires_refit: multires not active (run multires_init)";
      return false;
    }
    int level = getInt(args, "level", scene.multires->activeLevel());
    if (level < 2 || level > scene.multires->maxLevel()) {
      err = "multires_refit: bad level= (need 2..maxLevel)";
      return false;
    }
    int changed = scene.multires->downRefit(level);
    scene.attachMultiresLevel();
    std::fprintf(stdout, "[script] multires_refit level=%d changed=%d\n", level, changed);
    return true;
  }
  if (verb == "grid_stroke") {
    /* grid_stroke origin=x,y,z normal=x,y,z [dabs=1] [step=x,y,z]
     * [level=active]: run the current tool through the grids-native executor
     * (no materialized mesh / meshlog on the hot path), then mirror the
     * touched region into the resident slot mesh. Uses scene.brush props. */
    if (!scene.multires) {
      err = "grid_stroke: multires not active (run multires_init)";
      return false;
    }
    int level = getInt(args, "level", scene.multires->activeLevel());
    if (!ensureGridSession(scene, level, err)) {
      return false;
    }
    if (!brush::GridBrushExecutor::supportsBrush(scene.currentTool,
                                                 &scene.multires->gridAttrs()))
    {
      err = "grid_stroke: tool not grids-capable (see GridBrushExecutor roster)";
      return false;
    }
    float3 origin{0, 0, 0}, normal{0, 0, 1}, step{0, 0, 0};
    if (!parseFloat3(getArg(args, "origin"), origin) ||
        !parseFloat3(getArg(args, "normal"), normal))
    {
      err = "grid_stroke: missing origin=/normal=";
      return false;
    }
    parseFloat3(getArg(args, "step"), step);
    int dabs = getInt(args, "dabs", 1);
    std::string backend = getArg(args, "backend") ? getArg(args, "backend") : "cpp";

    if (backend == "wgpu" || backend == "wgsl" || backend == "gpu") {
      // GPU grids path: the same kernels through a dispatcher — wgpu-native
      // (engine-owned device) or the Vulkan SPIR-V compute path; `gpu` takes
      // whichever is built, wgpu first. Shares the session's undo log with
      // the CPU path, so grid_undo mixes freely.
      const brush::GpuKernelInfo *ki = brush::GridGpuStrokeSession::kernelFor(
          scene.currentTool, &scene.multires->gridAttrs());
      if (!ki) {
        err = "grid_stroke: tool not grids-GPU-capable";
        return false;
      }
      auto runGpuStroke = [&](brush::IBrushComputeDispatch &d, const char *tag) -> bool {
        brush::GridGpuStrokeSession gs;
        std::string serr;
        if (!gs.begin(scene.multires->gridDomain(level),
                      &scene.brush,
                      scene.currentTool,
                      &d,
                      scene.gridLog,
                      serr))
        {
          err = "grid_stroke: " + serr;
          return false;
        }
        float3 o = origin;
        for (int i = 0; i < dabs; i++, o += step) {
          if (!gs.dab(o, normal, serr)) {
            err = "grid_stroke: " + serr;
            return false;
          }
        }
        if (!gs.end(serr)) {
          err = "grid_stroke: " + serr;
          return false;
        }
        litestl::util::Vector<int> touched;
        for (int v : gs.strokeTouchedVerts()) {
          touched.append(v);
        }
        gridMirrorSync(scene, level, touched);
        std::fprintf(stdout,
                     "[script] grid_stroke(%s) level=%d dabs=%d moved=%d "
                     "undo_bytes=%zu\n",
                     tag,
                     level,
                     dabs,
                     int(touched.size()),
                     scene.gridLog->bytes());
        return true;
      };
#ifdef SBRUSH_WEBGPU_COMPUTE
      if (backend == "wgpu" || backend == "gpu") {
        webgpu::WgpuContext wctx;
        if (!wctx.initNative()) {
          err = "grid_stroke: wgpu-native device init failed";
          return false;
        }
        webgpu::WgpuBrushComputeDispatch disp(&wctx);
        std::string wgsl = std::string(SBRUSH_SCRIPT_STRINGIZE(SBRUSH_WGSL_DIR)) + "/" +
                           ki->kernel + ".wgsl";
        if (!disp.loadKernel(wgsl.c_str())) {
          err = "grid_stroke: failed to load " + wgsl;
          return false;
        }
        return runGpuStroke(disp, "wgpu");
      }
#endif
#ifdef SBRUSH_GPU_DISPATCH
      if (backend == "wgsl" || backend == "gpu") {
        if (!scene.ensureGPU() || !scene.context) {
          err = "grid_stroke: GPU device init failed";
          return false;
        }
        vulkan::BrushComputeDispatch disp(scene.context);
        std::string spv = std::string(SBRUSH_SCRIPT_STRINGIZE(SBRUSH_SPV_DIR)) + "/" +
                          ki->kernel + ".spv";
        if (!disp.loadKernel(spv.c_str())) {
          err = "grid_stroke: failed to load " + spv;
          return false;
        }
        return runGpuStroke(disp, "wgsl");
      }
#endif
      err = "grid_stroke: no GPU compute backend built";
      return false;
    }

    brush::GridBrushExecutor &ex = *scene.gridExec;
    scene.brush.writeProps();
    ex.beginStep();
    int moved = 0;
    float3 o = origin;
    for (int i = 0; i < dabs; i++, o += step) {
      ex.setGrabAccumAdd(false);
      moved += ex.applyDab(scene.currentTool, o, normal);
    }
    ex.endStep();
    litestl::util::Vector<int> touched;
    for (int v : ex.strokeTouchedVerts()) {
      touched.append(v);
    }
    gridMirrorSync(scene, level, touched);
    std::fprintf(stdout,
                 "[script] grid_stroke level=%d dabs=%d moved=%d undo_bytes=%zu\n",
                 level,
                 dabs,
                 moved,
                 scene.gridLog->bytes());
    return true;
  }
  if (verb == "grid_undo" || verb == "grid_redo") {
    /* Seek the grids-native undo log one step and mirror the result. */
    if (!scene.multires) {
      err = "grid_undo/redo: multires not active";
      return false;
    }
    if (!scene.gridExec) {
      err = "grid_undo/redo: no grids session (run grid_stroke first)";
      return false;
    }
    bool ok = verb == "grid_undo" ? scene.gridLog->undo() : scene.gridLog->redo();
    if (ok) {
      // Mirror every vert of the level (seek granularity is leaf blocks; the
      // simple full sync keeps the debug mirror correct).
      subdiv::GridLevelDomain *d = scene.multires->gridDomain(scene.gridLevel);
      litestl::util::Vector<int> all;
      all.resize(d->vertCount());
      for (int v = 0; v < d->vertCount(); v++) {
        all[v] = v;
      }
      gridMirrorSync(scene, scene.gridLevel, all);
    }
    std::fprintf(stdout, "[script] %s ok=%d\n", verb.c_str(), int(ok));
    return true;
  }
  if (verb == "grid_bench") {
    /* grid_bench [subdivs=26] [levels=4] [strokes=19] [dabs=57] [radius=0.1]
     * [strength=0.5] [leaf=512]: self-contained grids-native benchmark — own
     * cube cage + Multires (the scene is untouched), draw strokes marching
     * across the top face, per-phase stats + the G3 perf gates printed. */
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point a, clock::time_point b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    int subdivs = getInt(args, "subdivs", 26);
    int levels = getInt(args, "levels", 4);
    int strokes = getInt(args, "strokes", 19);
    int dabs = getInt(args, "dabs", 57);
    float radius = getFloat(args, "radius", 0.1f);
    float strength = getFloat(args, "strength", 0.5f);
    int leaf = getInt(args, "leaf", 512);

    mesh::Mesh *cage = mesh::createCube(subdivs, 1.0f);
    subdiv::Multires *mrb = litestl::alloc::New<subdiv::Multires>("grid bench mr");
    auto t0 = clock::now();
    mrb->init(*cage, levels);
    auto t1 = clock::now();
    subdiv::GridLevelDomain *d = mrb->gridDomain(levels);
    auto t2 = clock::now();
    subdiv::GridTree *tree = d->ensureTree(leaf);
    auto t3 = clock::now();
    std::fprintf(stdout,
                 "[grid_bench] cage %d faces; level %d: %d verts, %d grids, %d "
                 "leaves\n",
                 cage->f.count,
                 levels,
                 d->vertCount(),
                 d->gridCount(),
                 int(tree->leaves.size()));
    std::fprintf(stdout,
                 "[grid_bench] init=%.1fms domain=%.1fms tree=%.1fms\n",
                 ms(t0, t1),
                 ms(t1, t2),
                 ms(t2, t3));

    brush::Brush benchBrush;
    benchBrush.radius = radius;
    benchBrush.strength = strength;
    benchBrush.writeProps();
    subdiv::GridStrokeLog log;

    std::string backend = getArg(args, "backend") ? getArg(args, "backend") : "cpp";
    if (backend == "wgpu" || backend == "wgsl" || backend == "gpu") {
      // GPU half of the "at what size does GPU win" question: same workload
      // through the grids GPU session, dispatch/readback split printed.
      auto runGpuBench = [&](brush::IBrushComputeDispatch &disp,
                             const char *tag) -> bool {
        brush::GridGpuStrokeSession gs;
        auto t4 = clock::now();
        int movedTotal = 0;
        for (int s = 0; s < strokes; s++) {
          std::string serr;
          if (!gs.begin(d, &benchBrush, brush::SculptBrushes::DRAW, &disp, &log, serr)) {
            std::fprintf(stdout, "[grid_bench] gpu begin failed: %s\n", serr.c_str());
            return false;
          }
          float y = -0.4f + 0.8f * float(s) / float(strokes > 1 ? strokes - 1 : 1);
          for (int i = 0; i < dabs; i++) {
            float x = -0.45f + 0.9f * float(i) / float(dabs > 1 ? dabs - 1 : 1);
            if (!gs.dab(float3(x, y, 0.5f), float3(0, 0, 1), serr)) {
              std::fprintf(stdout, "[grid_bench] gpu dab failed: %s\n", serr.c_str());
              return false;
            }
          }
          if (!gs.end(serr)) {
            std::fprintf(stdout, "[grid_bench] gpu end failed: %s\n", serr.c_str());
            return false;
          }
          movedTotal += int(gs.strokeTouchedVerts().size());
        }
        auto t5 = clock::now();
        const auto &gst = gs.stats;
        std::fprintf(stdout,
                     "[grid_bench] gpu(%s) %d strokes x %d dabs: total=%.1fms "
                     "moved=%d\n",
                     tag,
                     strokes,
                     dabs,
                     ms(t4, t5),
                     movedTotal);
        std::fprintf(stdout,
                     "[grid_bench] gpu per-dab ms: host=%.4f dispatch=%.4f "
                     "readback=%.4f\n",
                     gst.hostMs / gst.dabs,
                     gst.dispatchMs / gst.dabs,
                     gst.readbackMs / gst.dabs);
        return true;
      };
      bool ok = false;
#ifdef SBRUSH_WEBGPU_COMPUTE
      if (!ok && (backend == "wgpu" || backend == "gpu")) {
        const brush::GpuKernelInfo *ki =
            brush::GridGpuStrokeSession::kernelFor(brush::SculptBrushes::DRAW);
        webgpu::WgpuContext wctx;
        if (ki && wctx.initNative()) {
          webgpu::WgpuBrushComputeDispatch disp(&wctx);
          std::string wgsl = std::string(SBRUSH_SCRIPT_STRINGIZE(SBRUSH_WGSL_DIR)) + "/" +
                             ki->kernel + ".wgsl";
          if (disp.loadKernel(wgsl.c_str())) {
            ok = runGpuBench(disp, "wgpu");
          }
        }
      }
#endif
#ifdef SBRUSH_GPU_DISPATCH
      if (!ok && (backend == "wgsl" || backend == "gpu")) {
        const brush::GpuKernelInfo *ki =
            brush::GridGpuStrokeSession::kernelFor(brush::SculptBrushes::DRAW);
        if (ki && scene.ensureGPU() && scene.context) {
          vulkan::BrushComputeDispatch disp(scene.context);
          std::string spv = std::string(SBRUSH_SCRIPT_STRINGIZE(SBRUSH_SPV_DIR)) + "/" +
                            ki->kernel + ".spv";
          if (disp.loadKernel(spv.c_str())) {
            ok = runGpuBench(disp, "wgsl");
          }
        }
      }
#endif
      if (!ok) {
        std::fprintf(stdout, "[grid_bench] gpu backend unavailable\n");
      }
      litestl::alloc::Delete(mrb);
      litestl::alloc::Delete(cage);
      return true;
    }

    brush::GridBrushExecutor ex(d, &benchBrush, &log);
    ex.stats.reset();

    auto t4 = clock::now();
    int moved = 0;
    for (int s = 0; s < strokes; s++) {
      ex.beginStep();
      // March across the top face; alternate rows per stroke so the touched
      // region varies like the interactive bench.
      float y = -0.4f + 0.8f * float(s) / float(strokes > 1 ? strokes - 1 : 1);
      for (int i = 0; i < dabs; i++) {
        float x = -0.45f + 0.9f * float(i) / float(dabs > 1 ? dabs - 1 : 1);
        moved +=
            ex.applyDab(brush::SculptBrushes::DRAW, float3(x, y, 0.5f), float3(0, 0, 1));
      }
      ex.endStep();
    }
    auto t5 = clock::now();

    const auto &st = ex.stats;
    double totalMs = ms(t4, t5);
    std::fprintf(stdout,
                 "[grid_bench] %d strokes x %d dabs: total=%.1fms moved=%d\n",
                 strokes,
                 dabs,
                 totalMs,
                 moved);
    std::fprintf(stdout,
                 "[grid_bench] per-dab ms: query=%.4f capture=%.4f coPrev=%.4f "
                 "stamp=%.4f automask=%.4f kernel=%.4f normals=%.4f bounds=%.4f\n",
                 st.queryMs / st.dabs,
                 st.captureMs / st.dabs,
                 st.coPrevMs / st.dabs,
                 st.stampMs / st.dabs,
                 st.automaskMs / st.dabs,
                 st.kernelMs / st.dabs,
                 st.normalsMs / st.dabs,
                 st.boundsMs / st.dabs);
    double core = st.perDabCoreMs();
    double wb = st.strokes > 0 ? st.writebackMs / st.strokes : 0.0;
    double undoMB = double(log.bytes()) / (1024.0 * 1024.0);
    std::fprintf(stdout,
                 "[grid_bench] gates: per-dab core %.4fms (<=0.35) %s | writeback "
                 "%.2fms/stroke (<=3) %s | undo %.1fMB (<=20) %s\n",
                 core,
                 core <= 0.35 ? "PASS" : "FAIL",
                 wb,
                 wb <= 3.0 ? "PASS" : "FAIL",
                 undoMB,
                 undoMB <= 20.0 ? "PASS" : "FAIL");

    litestl::alloc::Delete(mrb);
    litestl::alloc::Delete(cage);
    return true;
  }
  if (verb == "save_disp") {
    /* save_disp id=NAME level=L: snapshot a level's disp channel. */
    if (!scene.multires) {
      err = "save_disp: multires not active";
      return false;
    }
    int level = getInt(args, "level", scene.multires->activeLevel());
    std::string name = getArg(args, "id", "default");
    gatherDisp(*scene.multires, level, g_dispSnapshots[name]);
    std::fprintf(stdout,
                 "[script] save_disp id=%s level=%d floats=%zu\n",
                 name.c_str(),
                 level,
                 g_dispSnapshots[name].size());
    return true;
  }
  if (verb == "assert_disp") {
    /* assert_disp id=NAME level=L [eps=0] [changed=0]: compare the level's
     * disp channel against a snapshot. eps=0 means bit-exact; changed=1
     * asserts the channel DIFFERS beyond eps instead of matching. */
    if (!scene.multires) {
      err = "assert_disp: multires not active";
      return false;
    }
    int level = getInt(args, "level", scene.multires->activeLevel());
    std::string name = getArg(args, "id", "default");
    auto it = g_dispSnapshots.find(name);
    if (it == g_dispSnapshots.end()) {
      err = "assert_disp: no snapshot '" + name + "'";
      return false;
    }
    std::vector<float> cur;
    gatherDisp(*scene.multires, level, cur);
    if (cur.size() != it->second.size()) {
      err = "assert_disp: size mismatch";
      return false;
    }
    float eps = getFloat(args, "eps", 0.0f);
    bool wantChanged = getBool(args, "changed", false);
    int diffs = 0;
    float worst = 0.0f;
    for (size_t i = 0; i < cur.size(); i++) {
      float d = std::fabs(cur[i] - it->second[i]);
      if (d > eps || (eps == 0.0f && std::memcmp(&cur[i], &it->second[i], 4) != 0)) {
        diffs++;
        worst = d > worst ? d : worst;
      }
    }
    std::fprintf(stdout,
                 "[script] assert_disp id=%s level=%d diffs=%d worst=%g\n",
                 name.c_str(),
                 level,
                 diffs,
                 worst);
    if (wantChanged ? diffs == 0 : diffs > 0) {
      char buf[160];
      std::snprintf(buf,
                    sizeof(buf),
                    "assert_disp: %s (diffs=%d worst=%g)",
                    wantChanged ? "expected change, none found" : "unexpected diffs",
                    diffs,
                    worst);
      err = buf;
      return false;
    }
    return true;
  }
  if (verb == "roughness") {
    if (!scene.mesh) {
      err = "roughness: no mesh";
      return false;
    }
    // Region: an explicit center=, else every dab origin of the last stroke.
    Vector<float3> centers;
    float3 c;
    if (parseFloat3(getArg(args, "center"), c)) {
      centers.append(c);
    } else if (scene.lastStroke.valid) {
      for (const float3 &o : scene.lastStroke.centers) {
        centers.append(o);
      }
      if (centers.size() == 0) {
        centers.append(scene.lastStroke.origin);
      }
    } else {
      err = "roughness: no center= and no prior stroke";
      return false;
    }
    float radius =
        getFloat(args,
                 "radius",
                 scene.lastStroke.valid ? scene.lastStroke.radius : scene.brush.radius);
    float3 up{0, 0, 1};
    parseFloat3(getArg(args, "up"), up);
    reportRoughness(scene,
                    getArg(args, "tag", "region"),
                    centers,
                    radius,
                    up,
                    getFloat(args, "rest", 0.0f));
    return true;
  }

  handled = false;
  return true;
}

} // namespace sculptcore::debug_app::script
