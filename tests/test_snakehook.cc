// Regression gate for the snakehook brush. Snakehook's second term gathers
// toward `grabFrom + grabTo` — the advancing dab center — which is what forms
// the hook. If either grab vector fails to reach the kernel that gather point
// becomes the world origin and every vertex in range is pulled 25%*falloff
// toward (0,0,0) per dab, so the region collapses instead of hooking. The test
// drives a multi-dab stroke the way the TS host does (grabFrom = live center,
// grabTo = step since the last dab) and checks both halves: the region really
// hooks along the drag, and nothing walks toward the origin.
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

// Scoped so the Scene is torn down before test_end() runs its leak report.
static float runStroke(bool nonAccum)
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

  // The gather term pulls toward grabFrom+grabTo, which sits on the +Z face at
  // |co| ~ 0.25+. A vertex that instead moved toward the origin means the grab
  // vectors were lost and the kernel gathered to (0,0,0).
  float worstShrink = 0.0f; // largest inward move, as a fraction of the radius
  float maxMove = 0.0f;
  float meanDrift = 0.0f;
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
  }
  test_assert(touched > 0);
  meanDrift /= float(touched);
  fprintf(stderr, "snakehook: touched=%d maxMove=%.6f meanDriftX=%.6f shrink=%.6f\n",
          touched, maxMove, meanDrift, worstShrink);

  // The stroke actually deformed something, and did so at a scale set by the
  // drag (10 dabs * 0.01) rather than by the distance to the origin.
  test_assert(maxMove > 1e-3f);
  test_assert(maxMove < 4.0f * radius);
  // It hooked along the drag direction.
  test_assert(meanDrift > 1e-4f);
  // Nothing collapsed inward. The gather does pull slightly toward the dab
  // center, so allow a small inward component; a collapse to the origin would
  // be order 0.25 (a quarter of the way per dab, compounding).
  test_assert(worstShrink < 0.25f * radius);
  return maxMove;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  float accum = runStroke(/*nonAccum=*/false);
  float nonAccum = runStroke(/*nonAccum=*/true);
  // @incremental makes the kernel non-accumulable, so the ACCUMULATE flag is
  // inert for snakehook — both strokes build the same hook. Without it the
  // non-accumulate run replays each dab from base and only the last one
  // survives, collapsing the hook to a single step's worth of drag.
  fprintf(stderr, "snakehook: accum=%.6f nonAccum=%.6f\n", accum, nonAccum);
  test_assert(std::fabs(accum - nonAccum) < 1e-5f);
  return test_end();
}
