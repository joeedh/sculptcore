// M2: cavity automasking end-to-end through a real draw stroke
// (documentation/plans/2026-07-14-2007-cavity-automasking.md).
//
// A grid is bent into a convex dome and stroked with the draw brush three ways:
//   * cavity off               — baseline displacement
//   * cavity on (default)      — convex surface is masked, so the apex moves LESS
//   * cavity on + inverted     — convexity is now unmasked, so the apex moves MORE
//     than the default-cavity run (toward the baseline).
// This exercises the executor pre-fill + the strength() seam threaded through the
// regenerated kernel (ctx.strength(v.co, v.v)), not just the standalone factor.
#include "test_util.h"

#include "brush/brush.h"
#include "debug/scene.h"
#include "debug/script.h"
#include "mesh/mesh.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

test_init;

using namespace sculptcore;
using namespace sculptcore::debug_app;
using namespace sculptcore::mesh;
using litestl::math::float3;

static int centerVert(Mesh *m)
{
  int best = 0;
  float bestR = 1e30f;
  for (int v = 0; v < m->v.count; v++) {
    float3 co = m->v.co[v];
    float r = co[0] * co[0] + co[1] * co[1];
    if (r < bestR) {
      bestR = r;
      best = v;
    }
  }
  return best;
}

// Build a domed grid, stroke it once with the draw brush, and return the apex
// vertex's displacement magnitude. `cavity`/`inverted` toggle automasking.
static float strokeApexDisp(bool cavity, bool inverted)
{
  Scene s(128, 128, /*headless=*/true);
  auto r = script::run(s, "make_shape kind=grid n=24 m=24 size=2\n", ".");
  test_assert(r.ok);
  Mesh *m = s.mesh;

  // Convex dome: z = -0.3(x^2+y^2), normals ~ +Z at the apex.
  for (int v = 0; v < m->v.count; v++) {
    float3 co = m->v.co[v];
    co[2] = -0.3f * (co[0] * co[0] + co[1] * co[1]);
    m->v.co[v] = co;
  }
  m->recalc_normals();

  int apex = centerVert(m);
  float3 before = m->v.co[apex];

  s.brush.automask_cavity = cavity;
  s.brush.cavity_factor = 1.0f;
  s.brush.cavity_blur_steps = 2;
  s.brush.cavity_inverted = inverted;

  r = script::run(s,
                  "build_spatial leaf_limit=256 depth_limit=8\n"
                  "set_brush radius=1.5 strength=0.5\n"
                  "set_brush_tool tool=draw\n"
                  "stroke origin=0,0,0 normal=0,0,1\n",
                  ".");
  test_assert(r.ok);

  float3 after = m->v.co[apex];
  return (after - before).length();
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  float off = strokeApexDisp(/*cavity=*/false, /*inverted=*/false);
  float on = strokeApexDisp(/*cavity=*/true, /*inverted=*/false);
  float onInv = strokeApexDisp(/*cavity=*/true, /*inverted=*/true);

  fprintf(stderr, "apex disp: off=%g on=%g onInv=%g\n", off, on, onInv);

  // The brush must actually move the apex in the baseline.
  test_assert(off > 1e-4f);
  // Convex apex is masked by default cavity -> smaller displacement.
  test_assert(on < off * 0.9f);
  // Inverting unmasks convexity -> apex returns near the unmasked baseline.
  test_assert(onInv > off * 0.9f);

  return test_end();
}
