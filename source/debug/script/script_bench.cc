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

bool execBenchVerb(Scene &scene,
                    const std::string &verb,
                    ArgMap &args,
                    const char *out_dir,
                    std::string &err,
                    bool &handled)
{
  handled = true;

  if (verb == "bench_spatial") {
    /* Sweep (leaf_limit x gpu_tri_target) on the current mesh and report the
     * cost curves that drive the two tunables:
     *   build_ms   — tree build time (depends on leaf_limit; gpu_tri irrelevant)
     *   leaves     — leaf count (query granularity)
     *   gpunodes   — GPU node count == draw-call count (depends on gpu_tri_target)
     *   filter_us  — avg filterNodes() time per query
     *   wset_v     — avg verts in the brush working set (sum of hit leaves'
     *                unique_verts) — the brush kernel touches all of these
     *   inr_v      — avg verts actually within the brush radius
     *   waste      — wset_v / inr_v: culling tightness (1.0 = perfect; lower
     *                leaf_limit -> tighter -> less wasted brush work)
     * Pure query benchmark — does not sculpt, so the mesh stays pristine and
     * every config is measured against identical geometry. */
    if (!scene.mesh) {
      err = "bench_spatial: no mesh (run make_cube first)";
      return false;
    }
    std::vector<int> leafs =
        parseCsvInts(getArg(args, "leafs"), {64, 128, 256, 512, 1024});
    std::vector<int> gputris =
        parseCsvInts(getArg(args, "gputris"), {512, 2048, 8192, 32768});
    int depth = getInt(args, "depth", 22);
    int dabs = getInt(args, "dabs", 16);
    float radius = getFloat(args, "radius", scene.brush.radius);
    if (radius <= 0.0f) {
      radius = 0.25f;
    }

    int vc = scene.mesh->v.count;
    if (vc <= 0 || dabs <= 0) {
      err = "bench_spatial: empty mesh or dabs<1";
      return false;
    }
    /* Sample dab origins from verts spread across the index range. */
    Vector<float3> origins;
    for (int k = 0; k < dabs; k++) {
      int idx = int((long long)k * vc / dabs);
      if (idx >= vc) {
        idx = vc - 1;
      }
      origins.append(scene.mesh->v.co[idx]);
    }
    float r2 = radius * radius;

    std::printf("[bench] mesh verts=%d faces=%d  radius=%.4f dabs=%d depth=%d\n",
                scene.mesh->v.count,
                scene.mesh->f.count,
                radius,
                dabs,
                depth);
    std::printf("[bench] %-6s %-7s | %9s %7s %8s | %10s %9s %9s %6s\n",
                "leaf",
                "gputri",
                "build_ms",
                "leaves",
                "gpunodes",
                "filter_us",
                "wset_v",
                "inr_v",
                "waste");

    for (int leaf : leafs) {
      for (int gt : gputris) {
        auto t0 = std::chrono::steady_clock::now();
        scene.buildSpatial(leaf, depth, gt);
        double build_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();

        int leafCount = int(scene.tree->leaves().size());
        /* buildAll doesn't assign GPU nodes (update() does); drive it here. */
        scene.tree->recompute_subtree_tri_counts();
        scene.tree->assign_gpu_nodes();
        int gpuNodeCount = int(scene.tree->gpu_nodes().size());

        long wset = 0, inr = 0;
        auto q0 = std::chrono::steady_clock::now();
        for (const float3 &o : origins) {
          Vector<spatial::SpatialNode *> hit;
          scene.tree->filterNodes(o, radius, hit);
          for (spatial::SpatialNode *nd : hit) {
            for (int v : nd->unique_verts()) {
              wset++;
              float3 d = scene.mesh->v.co[v] - o;
              if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= r2) {
                inr++;
              }
            }
          }
        }
        double filter_us = std::chrono::duration<double, std::micro>(
                               std::chrono::steady_clock::now() - q0)
                               .count() /
                           dabs;
        double waste = inr > 0 ? double(wset) / double(inr) : 0.0;
        std::printf("[bench] %-6d %-7d | %9.2f %7d %8d | %10.1f %9ld %9ld %6.2f\n",
                    leaf,
                    gt,
                    build_ms,
                    leafCount,
                    gpuNodeCount,
                    filter_us,
                    wset / dabs,
                    inr / dabs,
                    waste);
        std::fflush(stdout);
      }
    }
    return true;
  }
  if (verb == "bench_dyntopo") {
    /* A/B the per-dab spatial cost: a full tree rebuild (what the pre-M3 path
     * paid every dab) vs one incremental dyntopo dab (ops + incremental node
     * ownership + tree->update of only the dirty leaves). Run on increasingly
     * large meshes to see the rebuild cost grow while the incremental dab stays
     * roughly flat (local to the brush). */
    if (!scene.mesh || !scene.tree) {
      err = "bench_dyntopo: no mesh/tree (make_cube; triangulate; build_spatial)";
      return false;
    }
    int leaf = getInt(args, "leaf_limit", 0);
    int depth = getInt(args, "depth_limit", 16);
    float radius = getFloat(args, "radius", 0.2f);
    float detail = getFloat(args, "detail", 0.02f);
    float3 center{0, 0, 0};
    parseFloat3(getArg(args, "center"), center);

    /* rebuild=0 skips the full tree rebuild + its A/B baseline, reusing the
     * current tree (kept current by the dyntopo callbacks). Lets a script fire
     * several independent dabs on one (expensive) large build to map ops vs
     * split count without paying the O(mesh) rebuild each time. */
    bool doRebuild = getBool(args, "rebuild", true);
    double rebuild_ms = 0.0;
    if (doRebuild) {
      auto t0 = std::chrono::steady_clock::now();
      scene.buildSpatial(leaf, depth, 0);
      rebuild_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
              .count();

      /* Warm-up: build the GPU buffers once so the measured update() below is an
       * INCREMENTAL one (per-frame in the real app), not the cold first build. */
      scene.tree->update(&scene.gpu);
    }

    int fBefore = scene.mesh->f.count;
    scene.dyntopoParams.l_max = detail;
    scene.dyntopoParams.l_min = detail * 0.4f;
    scene.dyntopoParams.grade = getFloat(args, "grade", 0.0f);
    scene.dyntopoParams.do_flips = getBool(args, "flip", true);
    scene.dyntopoParams.max_splits = getInt(args, "max_splits", 0);
    scene.dyntopoParams.do_smooth = getBool(args, "smooth", false);
    scene.dyntopoParams.smooth_lambda = getFloat(args, "smooth_lambda", 0.5f);
    scene.dyntopoParams.mode = dyntopo::DynTopoMode::Subdivide;

    /* Break the incremental dab into its two phases: the remesh + incremental
     * node ownership (local to the brush) vs tree->update() (whose partition +
     * draw-batch phases are currently global). */
    bool useSpatial = getBool(args, "spatial", true);
    /* seed=1 (default): round-0 seed from the tree's in-region leaves (local);
     * seed=0: full-mesh scan. A/B confirms split-count parity + the round-0 win. */
    Vector<int> seedVerts;
    if (getBool(args, "seed", true)) {
      Vector<spatial::SpatialNode *> hit;
      scene.tree->filterNodes(center, radius, hit);
      for (spatial::SpatialNode *n : hit) {
        for (int v : n->unique_verts()) {
          seedVerts.append(v);
        }
      }
    }
    scene.mesh->thawTopo();
    auto t1 = std::chrono::steady_clock::now();
    dyntopo::DynTopoStats st = dyntopo::runDyntopoRemesh(
        *scene.mesh,
        center,
        radius,
        scene.dyntopoParams,
        7u,
        useSpatial ? scene.tree->getSpatialCallbacks() : nullptr,
        litestl::util::span<const int>(seedVerts.data(), seedVerts.size()));
    double ops_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1)
            .count();
    auto t2 = std::chrono::steady_clock::now();
    scene.tree->update(&scene.gpu);
    double update_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2)
            .count();
    double dab_ms = ops_ms + update_ms;

    /* Convergence check: any in-region edge still longer than its (graded)
     * target means the dab under-refined. Also accumulate edge-length mean/var
     * over the region for a uniformity metric (CV = stddev/mean; lower = more
     * even triangles — the thing tangential smoothing improves). */
    int leftover = 0;
    float grade = scene.dyntopoParams.grade;
    float r2 = radius * radius;
    double lsum = 0.0, lsq = 0.0;
    int lcount = 0;
    for (int e : scene.mesh->e) {
      if (scene.mesh->e.c[e] == ELEM_NONE)
        continue;
      float3 a = scene.mesh->v.co[scene.mesh->e.vs[e][0]];
      float3 b = scene.mesh->v.co[scene.mesh->e.vs[e][1]];
      float3 mid = (a + b) * 0.5f;
      float d2 = (mid - center).lengthSqr();
      if (d2 > r2)
        continue;
      float target = detail;
      if (grade > 0.0f && radius > 0.0f) {
        target *= 1.0f + grade * (std::sqrt(d2) / radius);
      }
      float len = (a - b).length();
      if (len > target * 1.001f) {
        leftover++;
      }
      lsum += len;
      lsq += double(len) * len;
      lcount++;
    }
    double lmean = lcount > 0 ? lsum / lcount : 0.0;
    double lvar = lcount > 0 ? lsq / lcount - lmean * lmean : 0.0;
    double lcv = lmean > 0.0 ? std::sqrt(lvar > 0.0 ? lvar : 0.0) / lmean : 0.0;

    /* Max valence over in-region verts (the cascade's high-valence symptom —
     * grading should keep this near the regular-mesh value of 6). */
    int maxVal = 0;
    mesh::Mesh &mm = *scene.mesh;
    for (int v : mm.v) {
      if ((mm.v.co[v] - center).lengthSqr() > r2 || mm.v.e[v] == ELEM_NONE) {
        continue;
      }
      int n = 0, e0 = mm.v.e[v], e = e0;
      do {
        n++;
        int side = mm.e.vs[e][0] == v ? 0 : 1;
        e = mesh::diskEdge(mm.e.disk[e][side * 2 + 1]);
      } while (e != e0 && n < 100000);
      if (n > maxVal)
        maxVal = n;
    }

    std::printf("[bench_dyntopo] faces %d->%d  splits=%d flips=%d smooths=%d "
                "rounds=%d leftover=%d maxValence=%d cv=%.3f%s%s | "
                "full_rebuild=%.2fms | incremental: ops=%.2fms update=%.2fms "
                "total=%.2fms  speedup=%.1fx\n",
                fBefore,
                scene.mesh->f.count,
                st.splits,
                st.flips,
                st.smooths,
                st.rounds,
                leftover,
                maxVal,
                lcv,
                st.budget_hit ? " BUDGET" : "",
                (st.capped && !st.budget_hit) ? " CAPPED" : "",
                rebuild_ms,
                ops_ms,
                update_ms,
                dab_ms,
                dab_ms > 0.0 ? rebuild_ms / dab_ms : 0.0);
    std::fflush(stdout);
    return true;
  }

  handled = false;
  return true;
}

} // namespace sculptcore::debug_app::script
