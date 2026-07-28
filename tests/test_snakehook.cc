// Regression gate for the snakehook brush. The kernel drags each vert by
// `grabTo` and, when `pinch` is non-zero, moves it perpendicular to that drag
// axis relative to `grabFrom` — so both grab vectors have to reach the kernel
// for either term to mean anything. If they are lost they default to the
// origin: the drag becomes zero (nothing moves at all) and any pinch is
// measured around (0,0,0) instead of the dab center. The test drives a
// multi-dab stroke the way the hosts do (grabFrom = live center, grabTo = step
// since the last dab) and checks the region hooks along the drag, that nothing
// walks toward the origin, and that pinch narrows the hook while a negative
// pinch inflates it.
#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"

#include "brush/brush_executor.h"
#include "mesh/mesh.h"

#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>

test_init;

using namespace sculptcore::debug_app;
using namespace sculptcore::mesh;
using namespace litestl::math;
namespace brush = sculptcore::brush;

struct StrokeResult {
  float maxMove;
  // Mean |y| of the moved verts: their spread away from the drag axis, which
  // runs along +X through y = 0. Pinch pulls this in, negative pinch pushes it
  // out; the drag itself leaves it alone.
  float spread;
};

// Scoped so the Scene is torn down before test_end() runs its leak report.
static StrokeResult runStroke(bool nonAccum, float pinch)
{
  Scene scene(64, 64, /*headless=*/true);
  auto r = script::run(scene,
                       "make_cube subdivs=32 size=0.5\n"
                       "build_spatial leaf_limit=64 depth_limit=10\n"
                       "set_brush_tool tool=snakehook\n",
                       ".");
  test_assert(r.ok);
  test_assert(scene.currentTool == brush::SculptBrushes::SNAKEHOOK);

  Mesh *m = scene.mesh;
  litestl::util::Vector<float3> start;
  start.resize(m->v.count);
  for (int i = 0; i < m->v.count; i++) {
    start[i] = m->v.co[i];
  }

  const float radius = 0.12f;
  scene.brush.radius = radius;
  scene.brush.strength = 1.0f;
  // Signed pinch amount (0 = off), the engine convention; the Blender host
  // remaps crease_pinch_factor onto it.
  scene.brush.pinch = pinch;

  // Drag across the +Z face along +X, the way a user hooks a spike out.
  const float3 normal{0.0f, 0.0f, 1.0f};
  const float3 step{0.01f, 0.0f, 0.0f};
  float3 center{-0.05f, 0.0f, 0.25f};

  brush::CommandExecutor exec(scene.tree, &scene.brush);
  exec.meshLog = &scene.meshLog;
  exec.setStrokeGen(1);
  // Snakehook's kernel is @incremental, so the executor must ignore this: a dab
  // driven by a per-dab delta has no stroke-start base to replay from, and
  // re-deriving would drop every earlier dab's drag.
  exec.setNonAccum(nonAccum);
  exec.beginStep(false);
  for (int dab = 0; dab < 10; dab++) {
    // Mirror applyGrabDabState: grabFrom is the live dab center, grabTo the
    // step taken since the previous dab (zero on the first).
    scene.brush.grabFrom = center;
    scene.brush.grabTo = dab == 0 ? float3{0.0f, 0.0f, 0.0f} : step;
    scene.brush.writeProps();
    exec.applyDab(scene.currentTool, center, normal, radius, nullptr, 0);
    center = center + step;
  }
  exec.endStep();

  // The drag runs along +X across the +Z face, so a vertex that moved toward
  // the origin means the grab vectors were lost.
  float worstShrink = 0.0f; // largest inward move, as a fraction of the radius
  float maxMove = 0.0f;
  float meanDrift = 0.0f;
  float spread = 0.0f;
  int touched = 0;
  for (int i = 0; i < m->v.count; i++) {
    float3 d = m->v.co[i] - start[i];
    float moved = d.length();
    if (moved < 1e-7f) {
      continue;
    }
    touched++;
    maxMove = std::fmax(maxMove, moved);
    worstShrink = std::fmax(worstShrink, start[i].length() - m->v.co[i].length());
    meanDrift += d[0];
    spread += std::fabs(m->v.co[i][1]);
  }
  test_assert(touched > 0);
  meanDrift /= float(touched);
  spread /= float(touched);
  fprintf(stderr,
          "snakehook: pinch=%.2f touched=%d maxMove=%.6f meanDriftX=%.6f shrink=%.6f "
          "spread=%.6f\n",
          pinch, touched, maxMove, meanDrift, worstShrink, spread);

  // The stroke actually deformed something, and did so at a scale set by the
  // drag (10 dabs * 0.01) rather than by the distance to the origin.
  test_assert(maxMove > 1e-3f);
  test_assert(maxMove < 4.0f * radius);
  // It hooked along the drag direction.
  test_assert(meanDrift > 1e-4f);
  // Nothing collapsed inward: the drag is tangential and the pinch moves verts
  // perpendicular to it, so neither term walks the region toward the origin.
  // A collapse would be order the distance to it (~0.25).
  test_assert(worstShrink < 0.25f * radius);
  return {maxMove, spread};
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  StrokeResult accum = runStroke(/*nonAccum=*/false, 0.0f);
  StrokeResult nonAccum = runStroke(/*nonAccum=*/true, 0.0f);
  // @incremental makes the kernel non-accumulable, so the ACCUMULATE flag is
  // inert for snakehook — both strokes build the same hook. Without it the
  // non-accumulate run replays each dab from base and only the last one
  // survives, collapsing the hook to a single step's worth of drag.
  fprintf(stderr, "snakehook: accum=%.6f nonAccum=%.6f\n", accum.maxMove, nonAccum.maxMove);
  test_assert(std::fabs(accum.maxMove - nonAccum.maxMove) < 1e-5f);

  // Pinch narrows the hook and a negative pinch inflates it, both measured
  // perpendicular to the drag; neither may stop it hooking (asserted above).
  StrokeResult pinched = runStroke(/*nonAccum=*/false, 1.0f);
  StrokeResult inflated = runStroke(/*nonAccum=*/false, -1.0f);
  fprintf(stderr, "snakehook: spread neutral=%.6f pinched=%.6f inflated=%.6f\n",
          accum.spread, pinched.spread, inflated.spread);
  test_assert(pinched.spread < accum.spread);
  test_assert(inflated.spread > accum.spread);
  return test_end();
}
