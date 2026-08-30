// M1: cavity automasking heuristic
// (documentation/plans/2026-07-14-2007-cavity-automasking.md).
//
// Builds one grid mesh and displaces it two ways with the SAME topology:
//   * bowl  z = +A(x^2+y^2)  — concave (neighbors sit on the +normal side)
//   * dome  z = -A(x^2+y^2)  — convex  (neighbors sit on the -normal side)
// The raw estimate at the center must have opposite signs for the two, and the
// 0..1 remap must place convex below 0.5 (masked) and concave above 0.5
// (paintable) in the default (non-inverted) mode, flipping under `inverted`.
// A flat grid must land near the neutral 0.5, and stronger curvature must push
// the factor monotonically further from 0.5.
#include "test_util.h"

#include "brush/automask.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"

#include <cmath>
#include <cstdio>
#include <vector>

test_init;

using namespace sculptcore;
using namespace sculptcore::brush;
using namespace sculptcore::mesh;
using litestl::math::float3;

// Displace a flat grid into z = coeff*(x^2+y^2) in place, then recompute normals
// and the ring1 adjacency the cavity walk reads.
static void bend(Mesh *m, float coeff)
{
  for (int v = 0; v < m->v.count; v++) {
    float3 co = m->v.co[v];
    co[2] = coeff * (co[0] * co[0] + co[1] * co[1]);
    m->v.co[v] = co;
  }
  m->recalc_normals();
  m->topo_cache.invalidate();
  m->topo_cache.ensureRing1(*m);
}

// The vertex nearest the grid center (min x^2+y^2).
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

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  // Scope the scratch (and its litestl Vectors) so they free before the
  // end-of-test leak check runs.
  {
    CavityScratch scr;

    // --- Flat grid: near-neutral -------------------------------------------
    {
      Mesh *m = makeGrid(24, 24, 2.0f);
      m->recalc_normals();
      m->topo_cache.ensureRing1(*m);
      int c = centerVert(m);
      float raw = cavityRaw(m, c, 2, scr);
      fprintf(stderr, "flat raw=%g\n", raw);
      test_assert(std::fabs(raw) < 1e-3f);
      CavityParams p;
      p.enabled = true;
      p.blur_steps = 2;
      p.factor = 1.0f;
      float f = cavityRemap(p, raw);
      fprintf(stderr, "flat factor=%g\n", f);
      test_assert(std::fabs(f - 0.5f) < 0.05f);
      litestl::alloc::Delete(m);
    }

    // --- Bowl (concave) vs dome (convex): opposite signs -------------------
    float rawBowl = 0.0f, rawDome = 0.0f;
    {
      Mesh *m = makeGrid(24, 24, 2.0f);
      bend(m, 0.3f); // bowl
      int c = centerVert(m);
      float3 no = m->v.no[c];
      fprintf(stderr, "bowl center normal=(%g,%g,%g)\n", no[0], no[1], no[2]);
      test_assert(no[2] > 0.0f); // grid faces up; sanity for the sign reasoning
      rawBowl = cavityRaw(m, c, 2, scr);
      litestl::alloc::Delete(m);
    }
    {
      Mesh *m = makeGrid(24, 24, 2.0f);
      bend(m, -0.3f); // dome
      int c = centerVert(m);
      float3 no = m->v.no[c];
      fprintf(stderr, "dome center normal=(%g,%g,%g)\n", no[0], no[1], no[2]);
      test_assert(no[2] > 0.0f);
      rawDome = cavityRaw(m, c, 2, scr);
      litestl::alloc::Delete(m);
    }
    fprintf(stderr, "rawBowl=%g rawDome=%g\n", rawBowl, rawDome);
    test_assert(rawBowl > 1e-4f);
    test_assert(rawDome < -1e-4f);

    // Default mode: convex (dome) masked below 0.5, concave (bowl) above 0.5.
    CavityParams p;
    p.enabled = true;
    p.blur_steps = 2;
    p.factor = 1.0f;
    float fBowl = cavityRemap(p, rawBowl);
    float fDome = cavityRemap(p, rawDome);
    fprintf(stderr, "fBowl=%g fDome=%g\n", fBowl, fDome);
    test_assert(fBowl > 0.55f);
    test_assert(fDome < 0.45f);

    // Inverted mode flips both across 0.5.
    CavityParams pi = p;
    pi.inverted = true;
    float fBowlInv = cavityRemap(pi, rawBowl);
    float fDomeInv = cavityRemap(pi, rawDome);
    fprintf(stderr, "fBowlInv=%g fDomeInv=%g\n", fBowlInv, fDomeInv);
    test_assert(std::fabs(fBowlInv - (1.0f - fBowl)) < 1e-6f);
    test_assert(std::fabs(fDomeInv - (1.0f - fDome)) < 1e-6f);

    // --- Curve remap (M5) --------------------------------------------------
    // A constant 0.5 LUT collapses any factor to 0.5 (before inversion); an
    // inverting ramp (1 - t) flips the linear factor. Verifies cavityRemap
    // consults the curve only when use_curve is set.
    std::vector<float> flat(kCavityCurveSize, 0.5f), inv(kCavityCurveSize);
    for (int i = 0; i < kCavityCurveSize; i++) {
      inv[i] = 1.0f - float(i) / float(kCavityCurveSize - 1);
    }
    CavityParams pc = p;
    pc.use_curve = true;
    pc.curve_lut = flat.data();
    test_assert(std::fabs(cavityRemap(pc, rawBowl) - 0.5f) < 1e-6f);
    test_assert(std::fabs(cavityRemap(pc, rawDome) - 0.5f) < 1e-6f);
    pc.curve_lut = inv.data();
    fprintf(
        stderr, "curve inv bowl=%g (want ~%g)\n", cavityRemap(pc, rawBowl), 1.0f - fBowl);
    test_assert(std::fabs(cavityRemap(pc, rawBowl) - (1.0f - fBowl)) < 1e-3f);

    // --- Monotonicity: sharper curvature -> factor further from 0.5 --------
    {
      Mesh *soft = makeGrid(24, 24, 2.0f);
      bend(soft, -0.2f);
      float rawSoft = cavityRaw(soft, centerVert(soft), 2, scr);
      litestl::alloc::Delete(soft);

      Mesh *hard = makeGrid(24, 24, 2.0f);
      bend(hard, -0.5f);
      float rawHard = cavityRaw(hard, centerVert(hard), 2, scr);
      litestl::alloc::Delete(hard);

      float fSoft = cavityRemap(p, rawSoft);
      float fHard = cavityRemap(p, rawHard);
      fprintf(stderr, "fSoft=%g fHard=%g\n", fSoft, fHard);
      test_assert(fHard < fSoft); // more convex -> lower (further below 0.5)
    }
  } // scratch scope

  return test_end();
}
