// View-normal automasking
// (documentation/plans/2026-07-25-1138-view-normal-automasking.md).
//
// Part 1 checks viewNormalFactor() directly: head-on is full strength, the ramp
// into the limit is linear, and back faces either mirror the front (cull off) or
// read 0 outright (cull on).
// Part 2 drives a real draw stroke over a flat grid through the debug scene, so
// the executor pre-fill and the strength() seam in the generated kernel are
// exercised too — not just the standalone factor.
// Part 3 pins the normal source: the mask reads the stroke-start `.brush.orig.no`
// snapshot (keyed on `.brush.disp.gen`, stamped alongside the displacement
// field), not the live, possibly mid-stroke-refreshed v.no.
#include "test_util.h"

#include "brush/automask.h"
#include "brush/brush.h"
#include "brush/brush_executor.h"
#include "debug/scene.h"
#include "debug/script.h"
#include "mesh/mesh.h"

#include <cmath>
#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::brush;
using namespace sculptcore::debug_app;
using namespace sculptcore::mesh;
using litestl::math::float3;

static constexpr float kDeg = 3.14159265f / 180.0f;

// Unit normal `deg` degrees off the +Z head-on direction, in the XZ plane.
static float3 normalAt(float deg)
{
  return float3{std::sin(deg * kDeg), 0.0f, std::cos(deg * kDeg)};
}

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

// Stroke a flat grid once with the draw brush and return how far the center
// vertex moved. `viewDir` is the object-space eye->surface ray fed to the mask.
static float strokeCenterDisp(bool viewNormal, bool cullBackfaces, float3 viewDir)
{
  Scene s(128, 128, /*headless=*/true);
  auto r = script::run(s, "make_shape kind=grid n=24 m=24 size=2\n", ".");
  test_assert(r.ok);
  Mesh *m = s.mesh;
  m->recalc_normals();

  int c = centerVert(m);
  test_assert(m->v.no[c][2] > 0.0f); // grid faces +Z; the angles below assume it
  float3 before = m->v.co[c];

  s.brush.automask_view_normal = viewNormal;
  s.brush.cull_backfaces = cullBackfaces;
  s.brush.viewDir = viewDir;

  r = script::run(s,
                  "build_spatial leaf_limit=256 depth_limit=8\n"
                  "set_brush radius=1.5 strength=0.5\n"
                  "set_brush_tool tool=draw\n"
                  "stroke origin=0,0,0 normal=0,0,1\n",
                  ".");
  test_assert(r.ok);

  return (m->v.co[c] - before).length();
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  // --- Part 1: the factor itself -----------------------------------------
  ViewNormalParams p;
  p.enabled = true;
  p.view_dir = float3{0, 0, -1}; // looking down -Z at a +Z-facing surface

  // Full strength head-on and anywhere before the ramp starts (90 - 25 = 65).
  test_assert(std::fabs(viewNormalFactor(normalAt(0.0f), p) - 1.0f) < 1e-5f);
  test_assert(std::fabs(viewNormalFactor(normalAt(60.0f), p) - 1.0f) < 1e-5f);

  // Linear ramp: the midpoint of the 65..90 band reads 0.5, and it is zero at
  // and past the limit.
  float mid = viewNormalFactor(normalAt(77.5f), p);
  fprintf(stderr, "ramp midpoint=%g\n", mid);
  test_assert(std::fabs(mid - 0.5f) < 1e-3f);
  // Exactly at the limit, modulo the float error in a 90-degree normal.
  test_assert(viewNormalFactor(normalAt(90.0f), p) < 1e-5f);

  // Non-unit normals and view rays normalize internally.
  float3 longNo = normalAt(77.5f) * 7.0f;
  test_assert(std::fabs(viewNormalFactor(longNo, p) - mid) < 1e-3f);
  ViewNormalParams pl = p;
  pl.view_dir = float3{0, 0, -4};
  test_assert(std::fabs(viewNormalFactor(normalAt(77.5f), pl) - mid) < 1e-3f);

  // A degenerate ray (a caller that never set one) must disable the mask, not
  // read as 90 degrees off-axis and zero the whole stroke.
  ViewNormalParams pz = p;
  pz.view_dir = float3{0, 0, 0};
  test_assert(std::fabs(viewNormalFactor(normalAt(0.0f), pz) - 1.0f) < 1e-5f);
  test_assert(std::fabs(viewNormalFactor(normalAt(90.0f), pz) - 1.0f) < 1e-5f);
  pz.cull_backfaces = true;
  test_assert(std::fabs(viewNormalFactor(normalAt(180.0f), pz) - 1.0f) < 1e-5f);

  // Cull off: a back face fades exactly like the front face at the same angle
  // off edge-on, so directly-away is full strength.
  test_assert(std::fabs(viewNormalFactor(normalAt(180.0f), p) - 1.0f) < 1e-5f);
  test_assert(std::fabs(viewNormalFactor(normalAt(120.0f), p) -
                        viewNormalFactor(normalAt(60.0f), p)) < 1e-5f);

  // Cull on: everything past 90 is beyond the limit and reads 0, while the
  // front-facing side is untouched.
  ViewNormalParams pc = p;
  pc.cull_backfaces = true;
  test_assert(viewNormalFactor(normalAt(120.0f), pc) == 0.0f);
  test_assert(viewNormalFactor(normalAt(180.0f), pc) == 0.0f);
  test_assert(std::fabs(viewNormalFactor(normalAt(60.0f), pc) - 1.0f) < 1e-5f);
  test_assert(std::fabs(viewNormalFactor(normalAt(77.5f), pc) - mid) < 1e-3f);

  // Zero falloff degrades to a hard cutoff at the limit. Tested below 90 so the
  // cull-off fold (which maps 91 back onto 89) can't stand in for the cutoff.
  ViewNormalParams ph = p;
  ph.falloff = 0.0f;
  ph.limit = 60.0f * kDeg;
  test_assert(std::fabs(viewNormalFactor(normalAt(55.0f), ph) - 1.0f) < 1e-5f);
  test_assert(viewNormalFactor(normalAt(65.0f), ph) == 0.0f);

  // --- Part 2: end-to-end through a draw stroke ---------------------------
  const float3 headOn{0, 0, -1}; // eye in front of the +Z-facing grid
  const float3 edgeOn{1, 0, 0};  // view ray parallel to the surface
  const float3 fromBehind{0, 0, 1};

  float off = strokeCenterDisp(/*viewNormal=*/false, /*cullBackfaces=*/false, headOn);
  float on = strokeCenterDisp(true, false, headOn);
  float edge = strokeCenterDisp(true, false, edgeOn);
  float behind = strokeCenterDisp(true, false, fromBehind);
  float behindCulled = strokeCenterDisp(true, true, fromBehind);

  fprintf(stderr,
          "center disp: off=%g on=%g edge=%g behind=%g behindCulled=%g\n",
          off,
          on,
          edge,
          behind,
          behindCulled);

  // The brush must actually move the center in the baseline.
  test_assert(off > 1e-4f);
  // Head-on is full strength: masking on changes nothing.
  test_assert(std::fabs(on - off) < 1e-6f);
  // Edge-on is exactly at the limit, so the dab is fully masked out.
  test_assert(edge < 1e-9f);
  // Cull off: viewing the same surface from behind is symmetric with head-on.
  test_assert(std::fabs(behind - off) < 1e-6f);
  // Cull on: that same away-facing geometry is removed entirely.
  test_assert(behindCulled < 1e-9f);

  // --- Part 3: the mask is dynamic (live normals, every dab) ---------------
  // The view factor is evaluated in strength() against each vertex's LIVE
  // normal — no per-stroke cache. Dab 1 moves the head-on grid at full
  // strength; every live normal is then rotated edge-on mid-stroke, and dab 2
  // of the SAME stroke must come out fully masked (a cached per-stroke factor
  // would have kept full strength). Rotating back re-enables it.
  {
    Scene s(128, 128, /*headless=*/true);
    auto r = script::run(s,
                         "make_shape kind=grid n=24 m=24 size=2\n"
                         "build_spatial leaf_limit=256 depth_limit=8\n"
                         "set_brush radius=1.5 strength=0.5\n"
                         "set_brush_tool tool=draw\n",
                         ".");
    test_assert(r.ok);
    Mesh *m = s.mesh;
    m->recalc_normals();
    int c = centerVert(m);
    test_assert(m->v.no[c][2] > 0.0f);

    s.brush.automask_view_normal = true;
    s.brush.cull_backfaces = false;
    s.brush.viewDir = float3{0, 0, -1};
    s.brush.writeProps();

    brush::CommandExecutor exec(s.tree, &s.brush);
    exec.meshLog = &s.meshLog;
    exec.setNonAccum(true);
    exec.setStrokeGen(1);
    exec.beginStep(false);

    float3 before = m->v.co[c];
    exec.applyDab(
        s.currentTool, float3{0, 0, 0}, float3{0, 0, 1}, s.brush.radius, nullptr, 1);
    float disp1 = (m->v.co[c] - before).length();

    for (int v = 0; v < m->v.count; v++) {
      m->v.no[v] = float3{1, 0, 0};
    }
    before = m->v.co[c];
    exec.applyDab(
        s.currentTool, float3{0, 0, 0}, float3{0, 0, 1}, s.brush.radius, nullptr, 2);
    float disp2 = (m->v.co[c] - before).length();

    for (int v = 0; v < m->v.count; v++) {
      m->v.no[v] = float3{0, 0, 1};
    }
    before = m->v.co[c];
    exec.applyDab(
        s.currentTool, float3{0, 0, 0}, float3{0, 0, 1}, s.brush.radius, nullptr, 3);
    float disp3 = (m->v.co[c] - before).length();
    exec.endStep();

    fprintf(stderr,
            "dynamic mask dabs: head-on=%g edge-on=%g restored=%g\n",
            disp1,
            disp2,
            disp3);
    test_assert(disp1 > 1e-4f); // head-on: full strength
    test_assert(disp2 < 1e-9f); // edge-on live normals: masked out, same stroke
    test_assert(disp3 > 1e-4f); // restored normals: strength returns
  }

  return test_end();
}
