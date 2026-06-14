/* Regression test: undo a dyntopo step that is no longer the newest entry.
 * The follow-up plain stroke freezes topology (freeTopo drops the live TOPO
 * link pages), so MeshLog::undo must thaw before replaying topo chunks. Run
 * with do_smooth off and on so undo fidelity is covered with the tangential
 * smooth pass (which moves verts with no topology event) in the mix. */
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

static void runCase(bool doSmooth, int &retval)
{
  printf("=== case: do_smooth=%d ===\n", int(doSmooth));

  Scene scene(256, 256, /*headless=*/true);
  const char *src = "make_cube subdivs=8 size=0.5\n"
                    "build_spatial leaf_limit=128 depth_limit=10\n"
                    "set_brush_tool tool=draw\n"
                    "set_brush radius=0.22 strength=0.5\n";
  auto r = script::run(scene, src, ".");
  test_assert(r.ok);
  if (!r.ok) {
    fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    return;
  }

  mesh::Mesh *m = scene.mesh;

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
  scene.dyntopoParams.do_smooth = doSmooth;

  const float3 normal(0, 0, 1);
  const float radius = scene.brush.radius;

  brush::CommandExecutor exec(scene.tree, &scene.brush);
  exec.meshLog = &scene.meshLog;
  exec.ctx.renderMatrix = scene.renderMatrix;

  /* Stroke 1: dyntopo dabs across the +Z face (one meshlog step). */
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
  /* Release the stroke-long thaw, as the app does at dyntopo-stroke end. The
   * next plain dab then freezes topology — the state that broke undo. */
  exec.endDynTopoStroke();

  int vMid = m->v.count, fMid = m->f.count;
  printf("  stroke1 (dyntopo): v %d->%d, f %d->%d\n", vBefore, vMid, fBefore, fMid);
  test_assert(vMid != vBefore || fMid != fBefore);

  /* Mirror the app's per-frame tree maintenance between strokes. */
  scene.tree->applyDeferredNodeSplit();

  /* Stroke 2: plain brush only (no dyntopo), its own meshlog step. */
  exec.beginStep(false);
  for (int d = 0; d < NDABS; d++) {
    float t = float(d) / float(NDABS - 1);
    float3 origin(-0.18f + 0.36f * t, -0.05f + 0.1f * t, 0.25f);
    litestl::util::Vector<spatial::SpatialNode *> nodes;
    scene.tree->filterNodes(origin, radius, nodes);
    if (nodes.size() == 0) {
      continue;
    }
    exec.execBrush(scene.mesh, scene.currentTool, &nodes, origin, normal);
    exec.clearIsFirstOfStep();
  }
  exec.endStep();

  printf("  stroke2 (plain): v=%d f=%d, entries=%d, frozen=%d\n",
         m->v.count,
         m->f.count,
         scene.meshLog.entryCount(),
         int(m->topo_frozen));
  test_assert(scene.meshLog.entryCount() == 2);
  /* The plain stroke must have frozen topology — that's the failing setup. */
  test_assert(m->topo_frozen);

  /* Undo the plain stroke, then the dyntopo stroke (now NOT the newest). */
  printf("  undo stroke2...\n");
  scene.meshLog.undo(m, scene.tree);
  printf("  undo stroke1 (dyntopo, non-newest)...\n");
  scene.meshLog.undo(m, scene.tree);
  printf("  after undo x2: v=%d (want %d), f=%d (want %d)\n",
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

  /* Redo both steps — counts must return to the post-stroke2 state. */
  scene.meshLog.redo(m, scene.tree);
  scene.meshLog.redo(m, scene.tree);
  printf("  after redo x2: v=%d (want %d), f=%d (want %d)\n",
         m->v.count,
         vMid,
         m->f.count,
         fMid);
  test_assert(m->v.count == vMid);
  test_assert(m->f.count == fMid);
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  runCase(/*doSmooth=*/false, retval);
  runCase(/*doSmooth=*/true, retval);

  printf("dyntopo_undo_nonnewest: ok\n");
  return retval;
}
