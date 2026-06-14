/* Regression: a dynamic-topology stroke folded into ONE meshlog step — the
 * dyntopo pre-pass (topology log) PLUS the brush deform (per-node position
 * LogChunkSimple) — must undo to the exact pre-stroke mesh. The interactive TS
 * sculpt op wraps the whole stroke (dyntopo + brush) in a single
 * beginStep/endStep; if the topo and simple chunks fight on undo, vertex
 * positions come back wrong and/or faces get corrupted. (The debug-app `stroke`
 * verb keeps the two as separate steps, so it does NOT exercise this — we drive
 * the CommandExecutor directly, one step, like the app does.) */
#include "test_util.h"

#include "brush/brush_executor.h"
#include "debug/scene.h"
#include "debug/script.h"
#include "mesh/mesh.h"
#include "spatial/spatial.h"

#include "litestl/math/vector.h"

#include <cmath>
#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::debug_app;
using litestl::math::float3;

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Scene scene(256, 256, /*headless=*/true);
  /* Coarse cube (edge ~0.0625) so dyntopo with l_max=0.035 actually splits under
   * the dab. size=0.5 spans [-0.25,0.25], so the +Z face sits at z=0.25. The cube
   * is left as QUADS on purpose: dyntopo's in-stroke triangulateFaceFanCb must
   * fire the meshlog callbacks so undo restores the original quads exactly (the
   * "auto-triangulation hooks into meshlog" path). */
  const char *src = "make_cube subdivs=8 size=0.5\n"
                    "build_spatial leaf_limit=128 depth_limit=10\n"
                    "set_brush_tool tool=draw\n"
                    "set_brush radius=0.22 strength=0.5\n";
  auto r = script::run(scene, src, ".");
  test_assert(r.ok);
  if (!r.ok) {
    fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    return 1;
  }

  mesh::Mesh *m = scene.mesh;

  /* Snapshot pre-stroke state: counts + per-vertex positions. Original verts keep
   * their indices (0..vBefore-1) — undo restores killed originals to those same
   * indices, so the post-undo check can compare position by index. */
  int vBefore = m->v.count, fBefore = m->f.count;
  litestl::util::Vector<float3> orig;
  orig.resize(m->v.capacity());
  for (int v : m->v) {
    orig[v] = m->v.co[v];
  }

  scene.dyntopoEnabled = true;
  scene.dyntopoParams.l_max = 0.035f;
  scene.dyntopoParams.l_min = 0.012f;
  scene.dyntopoParams.mode = dyntopo::DynTopoMode::Both;

  const float3 normal(0, 0, 1);
  const float radius = scene.brush.radius;

  /* ONE meshlog step holding a MULTI-DAB stroke across the +Z face: each dab is
   * dyntopo pre-pass + brush deform (mirrors SculptPaintOp). Multiple dabs matter
   * — a later dab can collapse a vert an earlier dab's brush already captured in a
   * LogChunkSimple, which the undo must still handle. */
  brush::CommandExecutor exec(scene.tree, &scene.brush);
  exec.meshLog = &scene.meshLog;
  exec.ctx.renderMatrix = scene.renderMatrix;
  exec.beginStep(true);
  const int NDABS = 6;
  for (int d = 0; d < NDABS; d++) {
    float t = float(d) / float(NDABS - 1);
    float3 origin(-0.18f + 0.36f * t, -0.05f + 0.1f * t, 0.25f);
    exec.applyDynTopoDab(
        origin, radius, &scene.dyntopoParams, scene.dyntopoSeed + uint32_t(d));
    litestl::util::Vector<spatial::SpatialNode *> nodes;
    scene.tree->filterNodes(origin, radius, nodes);
    if (nodes.size() == 0) {
      continue;
    }
    exec.execBrush(scene.mesh, scene.currentTool, &nodes, origin, normal);
    exec.clearIsFirstOfStep();
  }
  exec.endStep();

  int vAfter = m->v.count, fAfter = m->f.count;
  printf("  stroke: v %d->%d, f %d->%d\n", vBefore, vAfter, fBefore, fAfter);
  test_assert(vAfter != vBefore || fAfter != fBefore); /* dyntopo changed topology */
  bool moved = false;
  for (int v = 0; v < vBefore; v++) {
    if (!m->v.freemap[v] && std::fabs(m->v.co[v][2] - orig[v][2]) > 1e-5f) {
      moved = true;
      break;
    }
  }
  test_assert(moved); /* the brush actually deformed geometry */

  /* Undo the whole stroke — must reproduce the exact pre-stroke mesh. */
  scene.meshLog.undo(m, scene.tree);

  printf("  after undo: v=%d (want %d), f=%d (want %d)\n",
         m->v.count,
         vBefore,
         m->f.count,
         fBefore);
  test_assert(m->v.count == vBefore);
  test_assert(m->f.count == fBefore);

  int badPos = 0;
  for (int v = 0; v < vBefore; v++) {
    if (m->v.freemap[v]) {
      badPos++;
      continue;
    }
    if ((m->v.co[v] - orig[v]).length() > 1e-5f) {
      badPos++;
    }
  }
  if (badPos) {
    fprintf(stderr,
            "  %d/%d original verts NOT restored to pre-stroke position\n",
            badPos,
            vBefore);
  }
  test_assert(badPos == 0);

  /* Redo must reproduce the post-stroke mesh (counts + the deform returns). */
  scene.meshLog.redo(m, scene.tree);
  printf("  after redo: v=%d (want %d), f=%d (want %d)\n",
         m->v.count,
         vAfter,
         m->f.count,
         fAfter);
  test_assert(m->v.count == vAfter);
  test_assert(m->f.count == fAfter);

  printf("dyntopo_stroke_undo: ok\n");
  /* Return retval directly (not test_end()): the debug Scene/GPU infrastructure
   * leaves allocations live at exit, so test_end()'s leak gate would false-fail —
   * test_live_stroke returns its code the same way. */
  return retval;
}
