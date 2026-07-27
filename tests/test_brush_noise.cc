// Brush-noise metric (plans/2026-07-26-0909-brush-displacement-base-attribute.md
// §9.1): the instrument the displacement-base A/B is judged on, so the
// instrument itself is tested before any number measured with it is believed.
// Sub-tests (a)-(d) validate `computeRoughness` against surfaces whose answer is
// known analytically; (e) runs the plan's fixture (flat grid + dyntopo + one
// non-accumulate draw stroke) and prints the per-dab live/base trace. (e)'s
// numeric gate lands with M3 — at M0 it only asserts the fixture reaches the
// metric with a meaningful region.
#include "test_util.h"

#include "debug/roughness.h"
#include "debug/scene.h"
#include "debug/script.h"

#include "mesh/mesh.h"

#include <cmath>
#include <cstdio>
#include <string>

test_init;

using namespace sculptcore::debug_app;
using namespace sculptcore::mesh;
using namespace litestl::math;
using litestl::util::Vector;

// A flat n x n grid of extent `size`, triangulated, with a spatial tree.
static bool makeGridScene(Scene &scene, int n, float size)
{
  char src[512];
  std::snprintf(src, sizeof(src),
                "make_shape kind=grid n=%d m=%d size=%g\n"
                "triangulate\n"
                "build_spatial\n"
                "set_backend backend=cpp\n",
                n, n, double(size));
  auto r = script::run(scene, src, ".");
  if (!r.ok) {
    std::fprintf(stderr, "  line %d: %s\n", r.line_no, r.error.c_str());
  }
  return r.ok;
}

static void allVerts(Mesh *m, Vector<int> &out)
{
  out.clear();
  for (int v : m->v) {
    out.append(v);
  }
}

static RoughnessResult scoreLive(Mesh *m)
{
  Vector<int> region;
  allVerts(m, region);
  return computeRoughness(m, region, RoughnessPoints::Live, 0);
}

int main()
{
  // (a) A flat, undisplaced grid is the metric's zero: the umbrella centroid is
  // in-plane, so the normal component vanishes exactly (up to fp).
  {
    Scene scene(64, 64, /*headless=*/true);
    test_assert(makeGridScene(scene, 32, 2.0f));
    RoughnessResult r = scoreLive(scene.mesh);
    std::fprintf(stderr, "(a) flat verts=%d rms=%.3g max=%.3g dih=%.3g\n", r.verts,
                 r.rms, r.maxr, r.dihedral);
    test_assert(r.verts > 500);
    test_assert(r.rms < 1e-5f);
    test_assert(r.maxr < 1e-5f);
    test_assert(r.dihedral < 1e-5f);
  }

  // (b) Vertex noise of amplitude `a` reads r ~ a/h, so the metric is linear in
  // the amplitude. Checkerboard sign by (i+j) parity, which the grid's row-major
  // vertex order reproduces as alternating indices along each row.
  {
    float rms[2] = {0.0f, 0.0f};
    for (int k = 0; k < 2; k++) {
      Scene scene(64, 64, /*headless=*/true);
      test_assert(makeGridScene(scene, 32, 2.0f));
      Mesh *m = scene.mesh;
      // Small vs h: n(v) tilts with amplitude, so linearity is the small-signal
      // property (h ~ 0.065 here).
      float a = 0.001f * float(k + 1);
      for (int v : m->v) {
        int i = int(std::lround((m->v.co[v][0] + 1.0f) * 31.0f / 2.0f));
        int j = int(std::lround((m->v.co[v][1] + 1.0f) * 31.0f / 2.0f));
        m->v.co[v][2] = ((i + j) & 1) ? a : -a;
      }
      rms[k] = scoreLive(m).rms;
    }
    std::fprintf(stderr, "(b) noise rms(a)=%.4g rms(2a)=%.4g ratio=%.3f\n", rms[0],
                 rms[1], double(rms[1] / rms[0]));
    test_assert(rms[0] > 1e-4f);
    test_assert(rms[1] / rms[0] > 1.95f && rms[1] / rms[0] < 2.05f);
  }

  // (c) On a smooth surface r ~ kappa*h/4, so refining the same analytic dome
  // must drive the metric down roughly in step with h -- this is what makes an
  // absolute threshold meaningful (the plan's `/h`, not `/h^2`, choice).
  {
    float rms[2] = {0.0f, 0.0f};
    for (int k = 0; k < 2; k++) {
      Scene scene(64, 64, /*headless=*/true);
      test_assert(makeGridScene(scene, k == 0 ? 32 : 64, 2.0f));
      Mesh *m = scene.mesh;
      for (int v : m->v) {
        float3 co = m->v.co[v];
        float r2 = co[0] * co[0] + co[1] * co[1];
        m->v.co[v][2] = 0.3f * std::exp(-r2 / 0.25f);
      }
      rms[k] = scoreLive(m).rms;
    }
    std::fprintf(stderr, "(c) dome rms(h)=%.4g rms(h/2)=%.4g ratio=%.3f\n", rms[0],
                 rms[1], double(rms[1] / rms[0]));
    test_assert(rms[0] > 0.0f);
    test_assert(rms[1] < 0.7f * rms[0]);
  }

  // (d) With no brush snapshot on the mesh the base point set is the live one,
  // so both scores must agree exactly -- the fallback the A/B relies on for
  // verts a stroke never touched.
  {
    Scene scene(64, 64, /*headless=*/true);
    test_assert(makeGridScene(scene, 32, 2.0f));
    Mesh *m = scene.mesh;
    for (int v : m->v) {
      m->v.co[v][2] = 0.05f * std::sin(6.0f * m->v.co[v][0]);
    }
    Vector<int> region;
    allVerts(m, region);
    RoughnessResult live = computeRoughness(m, region, RoughnessPoints::Live, 0);
    RoughnessResult base = computeRoughness(m, region, RoughnessPoints::Base, 0);
    std::fprintf(stderr, "(d) unstamped live rms=%.6g base rms=%.6g\n", live.rms,
                 base.rms);
    test_assert(live.rms == base.rms);
    test_assert(live.dihedral == base.dihedral);
  }

  // (e) The plan's fixture. Flat grid so the stroke-start surface is exactly
  // z = 0 and displacement *is* z; dyntopo with tangential smoothing on, which
  // is what the app ships. `rough=1` prints the per-dab live/base trace.
  {
    Scene scene(64, 64, /*headless=*/true);
    auto r = script::run(scene,
                         "make_shape kind=grid n=32 m=32 size=2\n"
                         "triangulate\n"
                         "build_spatial\n"
                         "set_backend backend=cpp\n"
                         "set_brush_tool tool=draw\n"
                         "set_brush radius=0.25 strength=0.5 nonaccum=1\n"
                         "dyntopo enabled=1 detail=0.02 seed=1 smooth=1\n"
                         "stroke_path p1=-0.6,0,0 p2=0.6,0,0 steps=24 "
                         "normal=0,0,1 rough=1\n",
                         ".");
    test_assert(r.ok);
    if (!r.ok) {
      std::fprintf(stderr, "  line %d: %s\n", r.line_no, r.error.c_str());
      return 1;
    }
    Vector<int> region;
    collectRegion(scene.mesh, scene.lastStroke.centers, scene.lastStroke.radius,
                  region);
    RoughnessResult live = computeRoughness(scene.mesh, region,
                                            RoughnessPoints::Live, scene.strokeGen);
    RoughnessResult base = computeRoughness(scene.mesh, region,
                                            RoughnessPoints::Base, scene.strokeGen);
    std::fprintf(stderr,
                 "(e) fixture verts=%d live rms=%.6g p95=%.6g max=%.6g dih=%.6g | "
                 "base rms=%.6g p95=%.6g max=%.6g dih=%.6g | maxz=%.6g volume=%.6g\n",
                 live.verts, live.rms, live.p95, live.maxr, live.dihedral, base.rms,
                 base.p95, base.maxr, base.dihedral, live.maxDisp, live.volume);
    test_assert(live.verts > 500);
    // Fidelity guard: the stroke really deposited material, so a later A/B
    // cannot win on noise by depositing less.
    test_assert(live.maxDisp > 0.1f);
    test_assert(live.volume > 0.0f);
  }

  // (f) M3 gate, pinned. Same fixture on the displacement base: the derived
  // base must stay on the stroke-start plane (z = 0) even though the tangential
  // smooth slides verts across a displaced surface. Guards the disp resampling
  // in smoothTangent — without it this reads ~0.088, the same as the legacy
  // `.brush.orig.co` path (see research/2026-07-26-brush-base-noise-baseline.md).
  {
    Scene scene(64, 64, /*headless=*/true);
    auto r = script::run(scene,
                         "make_shape kind=grid n=32 m=32 size=2\n"
                         "triangulate\n"
                         "build_spatial\n"
                         "set_backend backend=cpp\n"
                         "set_brush_tool tool=draw\n"
                         "set_brush radius=0.25 strength=0.5 nonaccum=1 dispbase=1\n"
                         "dyntopo enabled=1 detail=0.02 seed=1 smooth=1\n"
                         "stroke_path p1=-0.6,0,0 p2=0.6,0,0 steps=24 "
                         "normal=0,0,1\n",
                         ".");
    test_assert(r.ok);
    if (!r.ok) {
      std::fprintf(stderr, "  line %d: %s\n", r.line_no, r.error.c_str());
      return 1;
    }
    Vector<int> region;
    collectRegion(scene.mesh, scene.lastStroke.centers, scene.lastStroke.radius,
                  region);
    RoughnessResult live = computeRoughness(scene.mesh, region,
                                            RoughnessPoints::Live, scene.strokeGen);
    RoughnessResult base = computeRoughness(scene.mesh, region,
                                            RoughnessPoints::Base, scene.strokeGen);
    std::fprintf(stderr, "(f) dispbase live rms=%.6g | base rms=%.6g | maxz=%.6g\n",
                 live.rms, base.rms, live.maxDisp);
    test_assert(live.verts > 500);
    test_assert(live.maxDisp > 0.1f); // fidelity guard, same as (e)
    test_assert(live.volume > 0.0f);
    // Measured 0.0054; legacy and un-resampled disp both measure ~0.088.
    test_assert(base.rms < 0.02f);
  }

  return test_end();
}
