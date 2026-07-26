/* Regression: a COLLAPSE-DOMINATED dyntopo stroke on a dense mesh must complete
 * without aborting, stay topologically manifold, and undo/redo cleanly with
 * correct (recomputed, not stale) normals.
 *
 * Two defects are guarded here:
 *  - HEAVY-COLLAPSE CRASH: collapse can empty a spatial leaf of all its verts
 *    while its (loose, not-yet-regenerated) AABB still overlaps the brush sphere,
 *    so filterNodes returns it and the brush kernel builds a vertex iterator on an
 *    empty node — a wild read. The executor now skips empty leaves.
 *  - REDO NORMAL CORRUPTION: undo/redo swaps co/no rows back via the element store
 *    without driving add_face/remove_*, so the restored leaves must be flagged
 *    Spatial_UpdateNormals or they regenerate from stale normals. We assert redo
 *    reproduces the forward stroke's post-update normals exactly.
 *
 * Driven through the unified CommandExecutor::applyDab — the one dab sequence every
 * client now shares — exactly as the app does (one meshlog step per stroke). */
#include "test_util.h"

#include "brush/brush_executor.h"
#include "debug/scene.h"
#include "debug/script.h"
#include "gpu/vbo.h"
#include "mesh/mesh.h"
#include "mesh/utils/mesh_validate.h"
#include "spatial/spatial.h"

#include "litestl/math/vector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::debug_app;
using litestl::math::float3;

/* Manifold topology must survive heavy collapse (a few degenerate/inverted faces
 * are acceptable geometric quality from aggressive collapse — checkTopology, not
 * structurallyOk, is the invariant). */
static bool topologyOk(mesh::Mesh *m)
{
  mesh::RemeshReport r = mesh::remeshValidate(*m);
  return r.manifold && r.consistent_winding && r.non_manifold_edges == 0;
}

/* The per-corner position multiset that actually feeds the GPU buffer: every
 * owned leaf triangle contributes its three corner positions. This is what the
 * WASM repro reads back from gpu.buffers["position"] — built here straight from
 * the spatial tris so it bypasses GPU upload and isolates mesh/tri currency. */
static void collectCorners(spatial::SpatialTree *tree, mesh::Mesh *m,
                           litestl::util::Vector<float3> &out)
{
  out.clear();
  for (spatial::SpatialNode *node : tree->leaves()) {
    if (!node->data) {
      continue;
    }
    auto &node_fattr = node->treeMesh->f.node;
    for (const auto &tri : node->data->tris) {
      if (node_fattr[tri.f] != node->id) {
        continue; /* owned-once, matching the GPU fill */
      }
      out.append(m->v.co[m->c.v[tri.c[0]]]);
      out.append(m->v.co[m->c.v[tri.c[1]]]);
      out.append(m->v.co[m->c.v[tri.c[2]]]);
    }
  }
  std::sort(out.begin(), out.end(), [](const float3 &a, const float3 &b) {
    if (a[0] != b[0]) return a[0] < b[0];
    if (a[1] != b[1]) return a[1] < b[1];
    return a[2] < b[2];
  });
}

/* The ACTUAL uploaded position bytes: every GPU node's pos buffer as it stands
 * after tree->update(). Unlike collectCorners (which reads live mesh co), this
 * sees per-slice staleness — a leaf whose slice was not re-uploaded after a vert
 * it shares moved keeps the OLD corner position here. This is what the GPU
 * actually draws, so it's the true test of upload currency on undo/redo. */
static void collectGpuPos(spatial::SpatialTree *tree, litestl::util::Vector<float3> &out)
{
  out.clear();
  for (spatial::SpatialNode *node : tree->gpu_nodes()) {
    if (!node->gpu_data || !node->gpu_data->pos) {
      continue;
    }
    gpu::Buffer *pos = node->gpu_data->pos;
    float3 *d = pos->get_data<float3>();
    for (int i = 0; i < pos->size; i++) {
      out.append(d[i]);
    }
  }
  std::sort(out.begin(), out.end(), [](const float3 &a, const float3 &b) {
    if (a[0] != b[0]) return a[0] < b[0];
    if (a[1] != b[1]) return a[1] < b[1];
    return a[2] < b[2];
  });
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Scene scene(256, 256, /*headless=*/true);
  /* Dense cube (edge ~0.0104) so a large l_min collapses nearly every edge under
   * each dab — heavy, sustained collapsing across many dabs. */
  const char *src = "make_cube subdivs=48 size=0.5\n"
                    "build_spatial leaf_limit=128 depth_limit=10\n"
                    "set_brush_tool tool=draw\n"
                    "set_brush radius=0.3 strength=0.5\n";
  auto r = script::run(scene, src, ".");
  test_assert(r.ok);
  if (!r.ok) {
    fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    return 1;
  }

  mesh::Mesh *m = scene.mesh;
  int vBefore = m->v.count, fBefore = m->f.count;
  printf("  dense cube: v=%d f=%d\n", vBefore, fBefore);
  test_assert(topologyOk(m));

  /* Large goal edge length forces collapse-dominated remeshing: l_min (0.06) is
   * ~6x the mesh edge, so every edge under the dab is below it and collapses. */
  scene.dyntopoEnabled = true;
  scene.dyntopoParams.l_max = 0.12f;
  scene.dyntopoParams.l_min = 0.06f;
  scene.dyntopoParams.mode = dyntopo::DynTopoMode::Both;

  const float3 normal(0, 0, 1);
  const float radius = scene.brush.radius;

  brush::CommandExecutor exec(scene.tree, &scene.brush);
  exec.meshLog = &scene.meshLog;
  exec.ctx.renderMatrix = scene.renderMatrix;

  /* One meshlog step; sweep the +Z face so collapses keep firing dab after dab.
   * If the empty-leaf crash regressed, applyDab aborts here. */
  exec.beginStep(true);
  const int NDABS = 8;
  int totalTopo = 0;
  for (int d = 0; d < NDABS; d++) {
    float t = float(d) / float(NDABS - 1);
    float3 origin(-0.2f + 0.4f * t, -0.2f + 0.4f * t, 0.25f);
    totalTopo += exec.applyDab(scene.currentTool, origin, normal, radius,
                               &scene.dyntopoParams, scene.dyntopoSeed + uint32_t(d));
  }
  exec.endDynTopoStroke();
  exec.endStep();

  int vAfter = m->v.count, fAfter = m->f.count;
  printf("  stroke: v %d->%d, f %d->%d (topo ops=%d)\n",
         vBefore, vAfter, fBefore, fAfter, totalTopo);
  test_assert(totalTopo > 0);    /* dyntopo actually did work */
  test_assert(vAfter < vBefore); /* net collapse reduced the vert count */
  test_assert(topologyOk(m));    /* heavy collapse stayed manifold */

  /* Snapshot post-stroke positions by index (deterministic replay restores
   * created verts to the same indices), so redo can be checked per-vertex — the
   * actual "corrupted vertex positions on redo" bug, which a count-only check
   * misses. */
  litestl::util::Vector<float3> coForward;
  coForward.resize(m->v.capacity());
  litestl::util::Vector<bool> liveForward;
  liveForward.resize(m->v.capacity());
  for (int i = 0; i < m->v.capacity(); i++) {
    liveForward[i] = !m->v.freemap[i];
    if (liveForward[i]) {
      coForward[i] = m->v.co[i];
    }
  }

  /* Forward post-stroke normals: regen the dirty leaves, then snapshot the
   * spatial (per-leaf) normals. The spatial path gives leaf-boundary verts a
   * PARTIAL normal (only faces owned by the same leaf contribute), so it never
   * matches a global recompute exactly — the boundary deviation below is the
   * yardstick a correct redo must not exceed. */
  scene.tree->update(&scene.gpu);
  litestl::util::Vector<float3> spatialForward;
  spatialForward.resize(m->v.capacity());
  for (int v : m->v) {
    spatialForward[v] = m->v.no[v];
  }
  litestl::util::Vector<float3> cornersForward;
  collectCorners(scene.tree, m, cornersForward);
  /* Snapshot the actual uploaded GPU position bytes after the forward stroke. */
  litestl::util::Vector<float3> gpuForward;
  collectGpuPos(scene.tree, gpuForward);

  /* Global ground truth for the post-stroke geometry. Redo restores the SAME
   * positions+topology, so this also serves as redo's ground truth. (recalc
   * overwrites m->v.no, but undo/redo restore the rows from the log.) */
  litestl::util::Vector<float3> noGlobal;
  noGlobal.resize(m->v.capacity());
  m->recalc_normals();
  for (int v : m->v) {
    noGlobal[v] = m->v.no[v];
  }
  float maxForwardErr = 0.0f;
  for (int v : m->v) {
    maxForwardErr = std::max(maxForwardErr, (spatialForward[v] - noGlobal[v]).length());
  }

  /* Undo: exact pre-stroke topology back. */
  scene.meshLog.undo(m, scene.tree);
  scene.tree->update(&scene.gpu);
  printf("  after undo: v=%d (want %d), f=%d (want %d)\n",
         m->v.count, vBefore, m->f.count, fBefore);
  test_assert(m->v.count == vBefore);
  test_assert(m->f.count == fBefore);
  test_assert(topologyOk(m));

  /* Redo: post-stroke topology AND normals back. The restored leaves must be
   * flagged Spatial_UpdateNormals or redo regenerates GPU buffers from stale
   * normals — the bug. With the fix, redo's spatial normals are no less accurate
   * than the forward stroke's (a stale redo would deviate from noGlobal by up to
   * ~2.0 on flipped verts). */
  scene.meshLog.redo(m, scene.tree);
  scene.tree->update(&scene.gpu);
  printf("  after redo: v=%d (want %d), f=%d (want %d)\n",
         m->v.count, vAfter, m->f.count, fAfter);
  test_assert(m->v.count == vAfter);
  test_assert(m->f.count == fAfter);
  test_assert(topologyOk(m));

  /* Per-vertex redo POSITIONS must match the forward stroke exactly (same live
   * set, same indices, same co) — the redo position-corruption guard. */
  int badPos = 0, badLive = 0;
  for (int i = 0; i < m->v.capacity() && i < (int)liveForward.size(); i++) {
    bool live = !m->v.freemap[i];
    if (live != liveForward[i]) {
      badLive++;
      continue;
    }
    if (live && (m->v.co[i] - coForward[i]).length() > 1e-5f) {
      badPos++;
    }
  }
  if (badLive || badPos) {
    fprintf(stderr, "  redo positions: %d verts live-mismatch, %d verts moved\n",
            badLive, badPos);
  }
  test_assert(badLive == 0);
  test_assert(badPos == 0);

  /* GPU-feed currency: the per-corner position multiset after redo must match
   * the forward stroke's exactly (mirrors the WASM gpu.buffers["position"]
   * readback). A mismatch here with correct co/faces means stale tris feeding
   * the GPU. */
  litestl::util::Vector<float3> cornersRedo;
  collectCorners(scene.tree, m, cornersRedo);
  int cornerDiff = 0;
  float cornerMaxW = 0.0f;
  int cornerN = (int)std::min(cornersForward.size(), cornersRedo.size());
  for (int i = 0; i < cornerN; i++) {
    float d = (cornersForward[i] - cornersRedo[i]).length();
    if (d > 1e-5f) {
      cornerDiff++;
    }
    cornerMaxW = std::max(cornerMaxW, d);
  }
  printf("  GPU corners: forward=%d redo=%d, diff=%d maxw=%.5f\n",
         (int)cornersForward.size(), (int)cornersRedo.size(), cornerDiff, cornerMaxW);
  test_assert(cornersForward.size() == cornersRedo.size());
  test_assert(cornerDiff == 0);

  /* The bytes the GPU actually draws: every leaf slice must have been
   * re-uploaded so a moved boundary vert is current in EVERY slice that
   * references it as a corner — not just its owner leaf's. A stale neighbor
   * slice (only the owner leaf flagged on replay) shows here as a position
   * present forward but missing/old after redo. This is the electron-visible
   * "corrupted positions/normals after redo". */
  litestl::util::Vector<float3> gpuRedo;
  collectGpuPos(scene.tree, gpuRedo);
  int gpuDiff = 0;
  float gpuMaxW = 0.0f;
  int gpuN = (int)std::min(gpuForward.size(), gpuRedo.size());
  for (int i = 0; i < gpuN; i++) {
    float d = (gpuForward[i] - gpuRedo[i]).length();
    if (d > 1e-5f) {
      gpuDiff++;
    }
    gpuMaxW = std::max(gpuMaxW, d);
  }
  printf("  GPU upload pos: forward=%d redo=%d, diff=%d maxw=%.5f\n",
         (int)gpuForward.size(), (int)gpuRedo.size(), gpuDiff, gpuMaxW);
  test_assert(gpuForward.size() == gpuRedo.size());
  test_assert(gpuDiff == 0);

  /* With full-fan normals (skirt + halo), redo's recompute must land exactly on
   * the forward stroke's spatial normals — not merely within the global-recalc
   * yardstick below. */
  float maxRedoVsForward = 0.0f;
  for (int v : m->v) {
    maxRedoVsForward =
        std::max(maxRedoVsForward, (m->v.no[v] - spatialForward[v]).length());
  }
  printf("  redo vs forward normals: max=%g\n", maxRedoVsForward);
  test_assert(maxRedoVsForward < 1e-5f);

  float maxRedoErr = 0.0f;
  int badNo = 0;
  for (int v : m->v) {
    float err = (m->v.no[v] - noGlobal[v]).length();
    maxRedoErr = std::max(maxRedoErr, err);
    if (err > maxForwardErr + 1e-3f) {
      badNo++;
    }
  }
  printf("  normal err vs global: forward max=%.5f, redo max=%.5f, redo-worse verts=%d\n",
         maxForwardErr, maxRedoErr, badNo);
  /* Redo must be no less accurate than forward (stale-normal regression guard). */
  test_assert(maxRedoErr <= maxForwardErr + 1e-3f);
  test_assert(badNo == 0);

  printf("dyntopo_collapse_crash: ok\n");
  /* Return retval directly (not test_end()): the debug Scene/GPU infrastructure
   * leaves allocations live at exit, so test_end()'s leak gate would false-fail. */
  return retval;
}
