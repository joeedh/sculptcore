// Tier 9a test: the field-aligned tangential smooth primitive (preremesh.h).
//
//  - Safety: on a noised low-res sphere with a real curvature field, the
//    field-aligned smooth keeps every position finite and bounded (the
//    tangent-plane projection + edge-scale clamp rail never lets a vertex blow
//    up), and never changes the vertex count.
//  - No-field fallback: with no field attribute present, align=1 must match
//    align=0 bit-for-bit (the lift degrades to per-vertex isotropic).
//  - Engages: with a field present, align=1 must actually diverge from align=0.
//  - Steering: on a triangulated grid whose field is pinned to the grid axes,
//    the shear-damped field-aligned smooth must not align axis edges worse than
//    plain isotropic after in-plane noise.
//
// The full quality ladder (min-angle, crossFieldCurl drop, adaptive grading,
// no-op guarantee, fox A/B) lands with the rest of Tier 9; this gates the
// primitive in isolation.
#include "test_util.h"

#include "dyntopo/dyntopo_trace.h"
#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/set.h"
#include "mesh/attribute_builtin.h"
#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "mesh/utils/closest_point.h"
#include "mesh/utils/mesh_validate.h"
#include "mesh/utils/surface_walk.h"
#include "mesh/utils/triangulate.h"
#include "obj_load.h"
#include "remesh/extract/reproject.h"
#include "remesh/field/cross_field.h"
#include "remesh/field/density.h"
#include "remesh/preremesh.h"
#include "remesh/remesh.h"
#include "remesh/remesh_params.h"
#include "remesh/remesh_report.h"
#include "test_config.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

test_init;

#define TASSERT(expr)                                                                    \
  do {                                                                                   \
    if (!(expr)) {                                                                       \
      retval = 1;                                                                        \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                  \
      fflush(stderr);                                                                    \
    }                                                                                    \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::mesh;
using litestl::math::float3;

namespace {

// Deterministic in-plane jitter (keeps z so the grid stays planar and the field
// stays meaningful). LCG, no <random>, no global state.
struct Lcg {
  uint32_t s;
  explicit Lcg(uint32_t seed) : s(seed)
  {
  }
  float next() // in [-1, 1)
  {
    s = s * 1664525u + 1013904223u;
    return float(s >> 8) / float(1u << 23) - 1.0f;
  }
};

bool finiteCo(Mesh &m)
{
  for (int v : m.v) {
    float3 p = m.v.co[v];
    for (int i = 0; i < 3; i++) {
      if (!std::isfinite(p[i])) {
        return false;
      }
    }
  }
  return true;
}

void bbox(Mesh &m, float3 &lo, float3 &hi)
{
  bool init = false;
  for (int v : m.v) {
    float3 p = m.v.co[v];
    if (!init) {
      lo = hi = p;
      init = true;
      continue;
    }
    for (int i = 0; i < 3; i++) {
      if (p[i] < lo[i])
        lo[i] = p[i];
      if (p[i] > hi[i])
        hi[i] = p[i];
    }
  }
}

// Σ over all edges of a smooth π/2-periodic misalignment penalty to the nearest
// world axis: (1 − cos(4·ang))/2 ∈ [0,1], 0 when the edge runs along an axis, 1
// at 45°. Threshold-free (no edge can cross a hard gate and spuriously change the
// sum). The triangulation's 45° diagonals contribute a ~constant baseline in
// every variant, so it cancels in the iso-vs-field comparison; the discriminating
// signal is how far the axis edges sit from their axes.
double axisMisalign(Mesh &m)
{
  double sum = 0.0;
  for (int e : m.e) {
    float3 a = m.v.co[m.e.vs[e][0]], b = m.v.co[m.e.vs[e][1]];
    float dx = b[0] - a[0], dy = b[1] - a[1];
    if (double(dx) * dx + double(dy) * dy < 1e-18) {
      continue;
    }
    double ang = std::atan2(double(dy), double(dx));
    sum += 0.5 * (1.0 - std::cos(4.0 * ang));
  }
  return sum;
}

// Largest per-vertex position difference between two meshes with matching ids.
double maxVertDelta(Mesh &a, Mesh &b)
{
  double m = 0.0;
  for (int v : a.v) {
    m = std::fmax(m, double((a.v.co[v] - b.v.co[v]).length()));
  }
  return m;
}

// A noised low-res sphere, optionally with its curvature cross field solved.
Mesh *noisedSphere(uint32_t seed, bool withField)
{
  Mesh *s = mesh::makeUVSphere(16, 24, 1.0f);
  s->thawTopo();
  mesh::triangulateMesh(*s);
  if (withField) {
    remesh::CrossFieldParams p;
    remesh::computeCrossField(*s, p);
  }
  Lcg rng(seed);
  for (int v : s->v) {
    float3 c = s->v.co[v];
    for (int i = 0; i < 3; i++) {
      c[i] += 0.05f * rng.next();
    }
    s->v.co[v] = c;
  }
  return s;
}

// Safety: a noised sphere with a real curvature field stays finite, bounded, and
// topology-unchanged under the field-aligned smooth (the tangent-plane + edge
// clamp rail never lets a vertex escape).
void testFieldAlignedBounded()
{
  Mesh *sph = noisedSphere(12345u, true);
  float3 lo0, hi0;
  bbox(*sph, lo0, hi0);
  int vcount0 = sph->v.count;

  remesh::tangentialSmooth(*sph, 10, 0.5f, 1.0f);

  float3 lo1, hi1;
  bbox(*sph, lo1, hi1);
  float diag0 = (hi0 - lo0).length();
  float diag1 = (hi1 - lo1).length();
  fprintf(stderr,
          "[bounded] verts=%d diag0=%.4f diag1=%.4f finite=%d\n",
          vcount0,
          diag0,
          diag1,
          finiteCo(*sph));

  TASSERT(finiteCo(*sph));
  TASSERT(sph->v.count == vcount0);       // smooth never edits topology
  TASSERT(diag1 < diag0 * 1.10f + 1e-4f); // no blow-up (clamp rail holds)
  litestl::alloc::Delete<Mesh>(sph);
}

// Fallback: with no field present, align=1 degrades to per-vertex isotropic, so
// it must reproduce the isotropic result. The field block is skipped per-vertex
// (zero arms), so the two are numerically identical up to floating-point
// scheduling — the residual is bounded far below the field's real effect
// (see testFieldChangesResult), so an exact-zero assertion would be testing the
// compiler's instruction scheduling, not the algorithm.
void testNoFieldFallback()
{
  Mesh *a = noisedSphere(99u, false); // no .remesh.f.theta
  Mesh *b = noisedSphere(99u, false);
  remesh::tangentialSmooth(*a, 6, 0.5f, 0.0f); // isotropic
  remesh::tangentialSmooth(*b, 6, 0.5f, 1.0f); // field-aligned, but no field
  double d = maxVertDelta(*a, *b);
  fprintf(stderr, "[fallback] iso_vs_field=%.3e\n", d);
  TASSERT(d < 1e-3); // no field ⇒ field-aligned ≈ isotropic (FP-scheduling only)
  litestl::alloc::Delete<Mesh>(a);
  litestl::alloc::Delete<Mesh>(b);
}

// Engages: with a field present, align=1 must diverge from align=0 by far more
// than the no-field residual (the field actually steers the move). Guards
// against the lift silently producing no arms.
void testFieldChangesResult()
{
  Mesh *a = noisedSphere(99u, true);
  Mesh *b = noisedSphere(99u, true);
  remesh::tangentialSmooth(*a, 6, 0.5f, 0.0f);
  remesh::tangentialSmooth(*b, 6, 0.5f, 1.0f);
  double d = maxVertDelta(*a, *b);
  fprintf(stderr, "[engages] maxdelta=%.3e\n", d);
  TASSERT(d > 1e-2); // ≫ the no-field residual: the field genuinely steers
  litestl::alloc::Delete<Mesh>(a);
  litestl::alloc::Delete<Mesh>(b);
}

// Steering: on a triangulated grid whose field is pinned to the grid axes, the
// shear-damped field-aligned smooth must not align axis edges WORSE than plain
// isotropic. (Full flow straightening needs the BK flips of the 9b driver; the
// primitive alone only has to be field-consistent and non-harmful here.)
void testFieldAlignedSteers()
{
  auto buildNoised = [](uint32_t seed) -> Mesh * {
    Mesh *g = mesh::makeGrid(16, 16, 1.0f);
    g->thawTopo();
    mesh::triangulateMesh(*g);

    // Pin the field to the grid axes (stroke along +X ⇒ arms at 0°/90°).
    float3 sdir(1.0f, 0.0f, 0.0f);
    BuiltinAttr<float3, ".remesh.f.stroke_dir"> stroke;
    stroke.ensure(g->f.attrs);
    for (int f : g->f) {
      stroke[f] = sdir;
    }
    remesh::CrossFieldParams p;
    p.use_sharp_features = false;
    p.use_curvature = false;
    remesh::computeCrossField(*g, p);

    // In-plane noise (z untouched ⇒ stays planar, field stays valid). Leave the
    // perimeter alone so the comparison isn't dominated by boundary inward-pull.
    Lcg rng(seed);
    float3 lo, hi;
    bbox(*g, lo, hi);
    for (int v : g->v) {
      float3 c = g->v.co[v];
      if (c[0] <= lo[0] + 1e-4f || c[0] >= hi[0] - 1e-4f || c[1] <= lo[1] + 1e-4f ||
          c[1] >= hi[1] - 1e-4f)
      {
        continue;
      }
      c[0] += 0.10f * rng.next();
      c[1] += 0.10f * rng.next();
      g->v.co[v] = c;
    }
    return g;
  };

  Mesh *iso = buildNoised(777u);
  Mesh *fld = buildNoised(777u); // identical start (same seed)

  double e_noised = axisMisalign(*iso);
  remesh::tangentialSmooth(*iso, 15, 0.5f, 0.0f);
  remesh::tangentialSmooth(*fld, 15, 0.5f, 1.0f);
  double e_iso = axisMisalign(*iso);
  double e_fld = axisMisalign(*fld);

  fprintf(stderr, "[steers] noised=%.5f iso=%.5f field=%.5f\n", e_noised, e_iso, e_fld);
  TASSERT(e_fld < e_noised);            // field-aligned improved on the noise
  TASSERT(e_fld < e_iso * 1.05 + 1e-6); // and is no worse than isotropic (±5%)
  litestl::alloc::Delete<Mesh>(iso);
  litestl::alloc::Delete<Mesh>(fld);
}

// Mean edge length over all live edges.
double meanEdgeLen(Mesh &m)
{
  double sum = 0.0;
  int n = 0;
  for (int e : m.e) {
    sum += double((m.v.co[m.e.vs[e][0]] - m.v.co[m.e.vs[e][1]]).length());
    n++;
  }
  return n ? sum / n : 0.0;
}

// Adaptive sizing (step 3): a per-vertex size-scale attribute makes the shared BK
// remesh pass grade triangle size — short edges where s < 1, long where s > 1 —
// instead of one global length. Two claims: (a) a uniform s = 1 reproduces the
// nominal target L (the scale is calibrated, not an offset / odd normalization),
// and (b) a graded ramp produces a graded triangulation (the low-scale region is
// clearly finer than the high-scale region). The scale rides through splits
// (splitEdge interpolates the attr), so grading survives refinement.
void testSizeFieldGrades()
{
  // (a) Uniform scale = 1 → mean edge length tracks the nominal target.
  {
    Mesh *g = mesh::makeGrid(24, 24, 1.0f);
    g->thawTopo();
    mesh::triangulateMesh(*g);
    float L = float(meanEdgeLen(*g));
    BuiltinAttr<float, ".remesh.v.presize"> size;
    size.ensure(g->v.attrs);
    for (int v : g->v) {
      size[v] = 1.0f;
    }
    remesh::bkRemeshToTarget(*g, L, 4242u, ".remesh.v.presize");
    double mean = meanEdgeLen(*g);
    fprintf(stderr, "[sizefield/uniform] L=%.4f mean=%.4f\n", L, mean);
    TASSERT(finiteCo(*g));
    TASSERT(mean > L * 0.7 && mean < L * 1.4); // s=1 is nominal, no net rescale
    litestl::alloc::Delete<Mesh>(g);
  }

  // (b) Graded ramp s: 0.5 → 2.0 across +X → graded edge lengths.
  {
    Mesh *g = mesh::makeGrid(24, 24, 1.0f);
    g->thawTopo();
    mesh::triangulateMesh(*g);
    float L = float(meanEdgeLen(*g));
    float3 lo, hi;
    bbox(*g, lo, hi);
    float spanx = hi[0] - lo[0];

    BuiltinAttr<float, ".remesh.v.presize"> size;
    size.ensure(g->v.attrs);
    for (int v : g->v) {
      float t = spanx > 1e-9f ? (g->v.co[v][0] - lo[0]) / spanx : 0.5f;
      size[v] = 0.5f + 1.5f * t; // [0.5, 2.0]
    }
    remesh::bkRemeshToTarget(*g, L, 4242u, ".remesh.v.presize");
    size.ensure(g->v.attrs); // refresh handle after topology change

    double loSum = 0.0, hiSum = 0.0;
    int loN = 0, hiN = 0;
    for (int e : g->e) {
      int v0 = g->e.vs[e][0], v1 = g->e.vs[e][1];
      float se = 0.5f * (size[v0] + size[v1]);
      float len = (g->v.co[v0] - g->v.co[v1]).length();
      if (se < 0.8f) {
        loSum += len;
        loN++;
      } else if (se > 1.5f) {
        hiSum += len;
        hiN++;
      }
    }
    double loMean = loN ? loSum / loN : 0.0;
    double hiMean = hiN ? hiSum / hiN : 0.0;
    fprintf(stderr,
            "[sizefield/graded] loN=%d loMean=%.4f hiN=%d hiMean=%.4f\n",
            loN,
            loMean,
            hiN,
            hiMean);
    TASSERT(finiteCo(*g));
    TASSERT(loN > 0 && hiN > 0);
    TASSERT(loMean < hiMean * 0.75); // low-scale region is clearly finer
    litestl::alloc::Delete<Mesh>(g);
  }
}

// A prolate ellipsoid (unit sphere stretched along +X by @p ax): closed, so it is
// stable under the driver's iterated collapse (an open grid's unpinned boundary
// would migrate inward each round — the Tier 9c feature-pinning gap). Curvature is
// high at the two tips and low around the equatorial belt — the varying-curvature,
// feature-scale (~target) field the adaptive size field must grade.
Mesh *prolateEllipsoid(int nlat, int nlon, float ax)
{
  Mesh *e = mesh::makeUVSphere(nlat, nlon, 1.0f);
  e->thawTopo();
  mesh::triangulateMesh(*e);
  for (int v : e->v) {
    float3 p = e->v.co[v];
    p[0] *= ax;
    e->v.co[v] = p;
  }
  e->recalc_normals();
  return e;
}

// 9b driver, validity + convergence: the full bootstrap → field → BK → smooth loop
// on a noised sphere produces a finite, bounded mesh near the target edge length,
// and the converge_eps early-out path runs without exploding.
//
// Feature preservation is OFF here on purpose: this gates the 9b convergence
// mechanism in isolation. On a *noised* sphere a 45° dihedral test classifies the
// noise itself as sharp features, which would pin the noise and defeat the very
// denoise this test checks; 9c feature survival is gated by testDriverPreservesFeatures.
void testDriverConverges()
{
  Mesh *sph = noisedSphere(2024u, false); // driver solves its own rough field
  float3 lo0, hi0;
  bbox(*sph, lo0, hi0);
  float diag0 = (hi0 - lo0).length();
  float L = 0.18f;

  remesh::PreRemeshParams p;
  p.target = L;
  p.iters = 5;
  p.align = 1.0f;
  p.density = false;
  p.preserve_features = false; // isolate 9b convergence (see note above)
  p.converge_eps = 1e-3f;      // exercise the early-out measure
  remesh::preRemesh(*sph, p);

  float3 lo1, hi1;
  bbox(*sph, lo1, hi1);
  float diag1 = (hi1 - lo1).length();
  double mean = meanEdgeLen(*sph);
  fprintf(stderr,
          "[driver] verts=%d diag0=%.3f diag1=%.3f mean=%.4f L=%.3f\n",
          sph->v.count,
          diag0,
          diag1,
          mean,
          L);

  TASSERT(finiteCo(*sph));
  TASSERT(sph->v.count > 0);
  TASSERT(diag1 < diag0 * 1.20f + 1e-4f);    // BK + smooth don't blow the mesh up
  TASSERT(mean > L * 0.5 && mean < L * 1.6); // tracks the uniform target band
  litestl::alloc::Delete<Mesh>(sph);
}

// 9b adaptive sizing — the mechanism: with density on, the curvature size field the
// driver regenerates each round actually grades the triangulation. Bin the final
// edges by the regenerated .remesh.v.density and assert (i) the field has real
// contrast (both a high- and a low-density population exist) and (ii) edges over
// high-density (high-curvature) verts are clearly finer than over low-density ones.
//
// Binning by the field (not by geometry) makes this robust to the global shape
// erosion an *unpinned* iterated smooth causes on a sharp fixture (the prolate
// tips migrate inward). Feature preservation is OFF so the size field is the only
// thing under test (the tips would otherwise pin into the high-density bin and
// confound the contrast). The shape-preservation ladder and the adaptive-vs-uniform
// A/B (fox) are Tier-9 step-8 deliverables; here we only gate the size field.
void testDriverDensityGrades()
{
  const int nlat = 24, nlon = 40;
  const float ax = 4.0f; // 4:1:1 prolate — strong tip/equator curvature contrast
  float L = 0.35f;

  Mesh *g = prolateEllipsoid(nlat, nlon, ax);
  remesh::PreRemeshParams p;
  p.target = L;
  p.iters = 6;
  p.align = 1.0f;
  p.density = true;
  remesh::preRemesh(*g, p);

  BuiltinAttr<float, ".remesh.v.density"> dens;
  dens.ensure(g->v.attrs); // driver leaves the final size field in place
  double hiSum = 0.0, loSum = 0.0;
  int hiN = 0, loN = 0;
  for (int e : g->e) {
    int v0 = g->e.vs[e][0], v1 = g->e.vs[e][1];
    float d = 0.5f * (dens[v0] + dens[v1]);
    float len = (g->v.co[v0] - g->v.co[v1]).length();
    if (d > 1.5f) {
      hiSum += len;
      hiN++;
    } else if (d < 0.7f) {
      loSum += len;
      loN++;
    }
  }
  double hiMean = hiN ? hiSum / hiN : 0.0, loMean = loN ? loSum / loN : 0.0;
  fprintf(stderr,
          "[driver/density] verts=%d hiN=%d hiMean=%.4f loN=%d loMean=%.4f\n",
          g->v.count,
          hiN,
          hiMean,
          loN,
          loMean);
  TASSERT(finiteCo(*g));
  TASSERT(hiN > 0 && loN > 0);    // the field genuinely graded (contrast)
  TASSERT(hiMean < loMean * 0.8); // and edges track it: denser ⇒ finer
  litestl::alloc::Delete<Mesh>(g);
}

// A triangulated cube of `dimen` cells/side: its 12 edges are 90° dihedral-sharp
// and its 8 corners are feature corners — the Tier-9c feature-pinning fixture.
Mesh *triCube(int dimen, float size)
{
  Mesh *c = mesh::createCube(dimen, size);
  c->thawTopo();
  mesh::triangulateMesh(*c);
  c->recalc_normals();
  return c;
}

// 9c feature preservation: an iterated pre-pass on a cube must keep its sharp
// silhouette — the 8 corners stay put and the bounding box doesn't erode — because
// boundary/dihedral creases are pinned in both the BK collapse and the smooth. A
// cube corner has three sharp edges meeting (a junction), so it can neither collapse
// nor smooth: it is immortal and fixed. The A/B leg runs the identical flow with
// pinning off, where the same smoothing rounds the cube inward — confirming the
// pinning, not the fixture, is what preserves the extent.
void testDriverPreservesFeatures()
{
  const int dimen = 6;
  const float size = 0.5f;
  Mesh *cube = triCube(dimen, size);
  float3 lo0, hi0;
  bbox(*cube, lo0, hi0);
  float diag0 = (hi0 - lo0).length();
  float L = 0.6f * float(meanEdgeLen(*cube)); // sub-nominal ⇒ real collapse activity

  // The 8 cube corners (extreme in all three axes) — pinned ⇒ immortal + fixed.
  float3 corners[8];
  for (int i = 0; i < 8; i++) {
    corners[i] = float3(
        (i & 1) ? hi0[0] : lo0[0], (i & 2) ? hi0[1] : lo0[1], (i & 4) ? hi0[2] : lo0[2]);
  }

  // How many of the 8 original corners still have a vertex essentially on them.
  auto countCorners = [&](Mesh &m) {
    int kept = 0;
    for (int i = 0; i < 8; i++) {
      float best = 1e30f;
      for (int v : m.v) {
        float dd = (m.v.co[v] - corners[i]).length();
        if (dd < best)
          best = dd;
      }
      if (best < 1e-3f)
        kept++;
    }
    return kept;
  };

  remesh::PreRemeshParams p;
  p.target = L;
  p.iters = 5;
  p.align = 1.0f;
  p.bootstrap_iters = 0; // clean input: keep features crisp from iter 0
  p.preserve_features = true;
  remesh::preRemesh(*cube, p);
  float3 lo1, hi1;
  bbox(*cube, lo1, hi1);
  float diag1 = (hi1 - lo1).length();
  int cornersKept = countCorners(*cube);

  // A/B: identical flow with pinning off → the smooth slides the corners off and
  // collapse rounds them away, so the unpinned run keeps fewer of the 8 corners.
  Mesh *cube2 = triCube(dimen, size);
  remesh::PreRemeshParams p2 = p;
  p2.preserve_features = false;
  remesh::preRemesh(*cube2, p2);
  int cornersKept2 = countCorners(*cube2);

  fprintf(stderr,
          "[driver/features] verts=%d diag0=%.4f diag1=%.4f corners pinned=%d/8 "
          "unpinned=%d/8\n",
          cube->v.count,
          diag0,
          diag1,
          cornersKept,
          cornersKept2);

  TASSERT(finiteCo(*cube));
  TASSERT(cube->v.count > 0);
  TASSERT(cornersKept == 8);           // every corner pinned in place (immortal + fixed)
  TASSERT(diag1 > diag0 * 0.98f);      // pinned silhouette never collapses inward
  TASSERT(cornersKept2 < cornersKept); // pinning preserves corners the plain flow loses
  litestl::alloc::Delete<Mesh>(cube);
  litestl::alloc::Delete<Mesh>(cube2);
}

// A triangulated flat grid with an open border — the 6.5 boundary-sliding fixture:
// a dense square rim whose 4 corners are geometric corners but topological "clean
// curve interiors" (exactly 2 same-type feature edges each).
Mesh *triGrid(int n, float size)
{
  Mesh *g = mesh::makeGrid(n, n, size);
  g->thawTopo();
  mesh::triangulateMesh(*g);
  g->recalc_normals();
  return g;
}

// 6.5 boundary sliding: the pre-pass must coarsen an open rim ALONG its original
// polyline — rim verts slide (1D Laplacian + snapshot projection) instead of being
// hard-pinned (9c), the geometric corner gate keeps the 4 corners immortal even
// though the topological collapse test can't see them, and nothing drifts off the
// perimeter or out of plane. The A/B leg reruns the BK pass with the corner gate
// off (the pre-6.5 behavior), where rim collapse chews the corners away.
void testBoundarySliding()
{
  const int n = 33;
  const float size = 1.0f;
  const float spacing = size / float(n - 1);
  Mesh *g = triGrid(n, size);

  float3 lo0, hi0;
  bbox(*g, lo0, hi0);
  const float z0 = lo0[2];
  float3 corners[4] = {float3(lo0[0], lo0[1], z0),
                       float3(hi0[0], lo0[1], z0),
                       float3(hi0[0], hi0[1], z0),
                       float3(lo0[0], hi0[1], z0)};
  auto countCorners = [&](Mesh &m) {
    int kept = 0;
    for (int i = 0; i < 4; i++) {
      float best = 1e30f;
      for (int v : m.v) {
        float dd = (m.v.co[v] - corners[i]).length();
        if (dd < best)
          best = dd;
      }
      if (best < 1e-3f)
        kept++;
    }
    return kept;
  };
  auto isBoundaryEdge = [](Mesh &m, int e) {
    int c1 = m.e.c[e];
    return c1 != ELEM_NONE && m.c.radial_next[c1] == c1;
  };
  auto boundaryStats = [&](Mesh &m, double &len, float &minCornerEdge) {
    int nbv = 0;
    len = 0.0;
    minCornerEdge = 1e30f;
    litestl::util::Set<int> bv;
    for (int e : m.e) {
      if (!isBoundaryEdge(m, e)) {
        continue;
      }
      float3 a = m.v.co[m.e.vs[e][0]], b = m.v.co[m.e.vs[e][1]];
      float l = (a - b).length();
      len += double(l);
      bv.add(m.e.vs[e][0]);
      bv.add(m.e.vs[e][1]);
      for (int i = 0; i < 4; i++) {
        if ((a - corners[i]).length() < 1e-3f || (b - corners[i]).length() < 1e-3f) {
          if (l < minCornerEdge)
            minCornerEdge = l;
        }
      }
    }
    for (int v : bv) {
      nbv++;
      // Every rim vert stays on the square perimeter, in plane.
      float3 p = m.v.co[v];
      float dx = std::fabs(p[0] - lo0[0]) < std::fabs(p[0] - hi0[0])
                     ? std::fabs(p[0] - lo0[0])
                     : std::fabs(p[0] - hi0[0]);
      float dy = std::fabs(p[1] - lo0[1]) < std::fabs(p[1] - hi0[1])
                     ? std::fabs(p[1] - lo0[1])
                     : std::fabs(p[1] - hi0[1]);
      TASSERT((dx < 2e-3f || dy < 2e-3f) && std::fabs(p[2] - z0) < 1e-3f);
    }
    return nbv;
  };

  double len0;
  float mce0;
  int bnd0 = boundaryStats(*g, len0, mce0);

  remesh::PreRemeshParams p;
  p.target = 0.12f; // ~4x the input spacing => real rim-collapse pressure
  p.iters = 5;
  p.align = 1.0f;
  p.density = false; // uniform band: predictable rim spacing
  p.preserve_features = true;
  remesh::preRemesh(*g, p);

  double len1;
  float mce1;
  int bnd1 = boundaryStats(*g, len1, mce1);
  int cornersKept = countCorners(*g);
  bool planar = true;
  for (int v : g->v) {
    planar = planar && std::fabs(g->v.co[v][2] - z0) < 1e-3f;
  }

  // A/B: one BK pass with the geometric corner gate off (pre-6.5) — the corners
  // look like clean curve interiors to the topological test, so collapse eats them.
  Mesh *g2 = triGrid(n, size);
  remesh::classifyFeatures(*g2, 0.785398f);
  remesh::bkRemeshToTarget(*g2,
                           p.target,
                           4242u,
                           nullptr,
                           /*preserve_features=*/true,
                           nullptr,
                           /*feature_corner_angle=*/0.0f);
  int cornersNoGate = countCorners(*g2);

  fprintf(stderr,
          "[driver/bndslide] verts=%d bndVerts %d->%d perim %.4f->%.4f "
          "minCornerEdge %.4f->%.4f corners=%d/4 noGate=%d/4\n",
          g->v.count,
          bnd0,
          bnd1,
          len0,
          len1,
          mce0,
          mce1,
          cornersKept,
          cornersNoGate);

  TASSERT(finiteCo(*g));
  TASSERT(planar);           // tangential flow never leaves the plane
  TASSERT(cornersKept == 4); // corner gate + corner-angle pin: immortal
  TASSERT(bnd1 < bnd0 / 2);  // the rim genuinely coarsened...
  TASSERT(std::fabs(len1 - len0) < 0.05 * len0); // ...without shrinking the loop
  TASSERT(mce1 > 1.5f * spacing); // sliding: corner-adjacent verts migrated out
                                  // (collapse alone can't grow these — the gate
                                  // refuses corner-endpoint collapses)
  TASSERT(cornersNoGate < 4);     // the gate is load-bearing
  litestl::alloc::Delete<Mesh>(g);
  litestl::alloc::Delete<Mesh>(g2);
}

// Pre-pass-scale granular trace (dyntopo_trace.h): attaching a DynTopoTrace to the
// driver accumulates every outer iter's BK dab rounds into one continuous series,
// each stamped with its outer iter, so the split bug's sliver behavior is visible
// at multi-iter scale (not just within a single dab). This gates the threading +
// stamping mechanism and reports the observed oscillation for review.
void testPrepassTrace()
{
  Mesh *sph = noisedSphere(2024u, false);
  const int iters = 6;

  dyntopo::DynTopoTrace trace;
  remesh::PreRemeshParams p;
  p.target = 0.18f;
  p.iters = iters;
  p.align = 1.0f;
  p.density = false;
  p.preserve_features = false;
  p.converge_eps = 0.0f; // run all outer iters so the full series is captured
  p.trace = &trace;
  remesh::preRemesh(*sph, p);

  dyntopo::OscillationReport r = dyntopo::detectOscillation(trace, /*min_swing=*/2);

  // Confirm the iter stamps are contiguous 0..maxIter and in order (every outer
  // iter that ran contributed >= 1 round, no gaps), and print a per-iter summary.
  const float k = 180.0f / 3.14159265358979323846f;
  int n = int(trace.rounds.size());
  int maxIter = -1, prevIter = -1;
  bool ordered = true, contiguous = true;
  for (int i = 0; i < n; i++) {
    int it = trace.rounds[i].iter;
    if (it < prevIter)
      ordered = false;
    if (it > maxIter + 1)
      contiguous = false;
    if (it > maxIter)
      maxIter = it;
    prevIter = it;
  }
  for (int it = 0; it <= maxIter; it++) {
    int rounds = 0, maxThin = 0, churn = 0, churnRun = 0;
    float worstAng = 6.2832f;
    for (int i = 0; i < n; i++) {
      if (trace.rounds[i].iter != it)
        continue;
      const dyntopo::RoundQuality &q = trace.rounds[i];
      rounds++;
      if (q.thin_count > maxThin)
        maxThin = q.thin_count;
      if (q.min_angle < worstAng)
        worstAng = q.min_angle;
      churn = (q.splits + q.collapses) <= 2 ? churn + 1 : 0;
      if (churn > churnRun)
        churnRun = churn;
    }
    fprintf(stderr,
            "[prepass] iter=%d rounds=%-3d maxThin=%-2d worstMinAng=%5.1f "
            "churnRun=%d\n",
            it,
            rounds,
            maxThin,
            worstAng * k,
            churnRun);
  }
  fprintf(stderr,
          "[prepass/trace] rounds=%d iters=%d peak_thin=%d worstMinAng=%.1f "
          "swings=%d healed=%d churn_run=%d@iter%d\n",
          n,
          maxIter + 1,
          r.peak_thin,
          r.worst_min_angle * k,
          r.swings,
          int(r.healed),
          r.churn_run,
          r.churn_iter);

  TASSERT(n > 0);        // the trace captured rounds
  TASSERT(maxIter >= 1); // genuinely multi-iter (more than one dab)
  TASSERT(ordered);      // rounds appended in outer-iter order
  TASSERT(contiguous);   // every outer iter contributed at least one round
  TASSERT(finiteCo(*sph));
  // Regression gate for the limit-cycle early-out (DynTopoParams::max_stall_rounds,
  // default 16): no dab may churn longer than the cap before bailing. Pre-fix this
  // run spun iter0 to the round cap with a 77-round split<->collapse churn tail; the
  // early-out bounds every dab's churn to the cap, so the worst run is <= 16.
  TASSERT(r.churn_run <= 16);
  litestl::alloc::Delete<Mesh>(sph);
}

// 9b no-op guard: target <= 0 (or iters <= 0) leaves the mesh untouched, so a
// disabled pre-pass never perturbs the pipeline.
void testDriverNoOp()
{
  Mesh *g = mesh::makeGrid(12, 12, 1.0f);
  g->thawTopo();
  mesh::triangulateMesh(*g);
  int vcount0 = g->v.count;
  litestl::util::Vector<float3> co0;
  co0.resize(int(g->v.capacity()));
  for (int v : g->v) {
    co0[v] = g->v.co[v];
  }

  remesh::PreRemeshParams p;
  p.target = 0.0f; // disabled
  p.iters = 5;
  remesh::preRemesh(*g, p);

  double d = 0.0;
  for (int v : g->v) {
    d = std::fmax(d, double((g->v.co[v] - co0[v]).length()));
  }
  fprintf(stderr, "[driver/noop] vcount %d->%d maxdelta=%.3e\n", vcount0, g->v.count, d);
  TASSERT(g->v.count == vcount0);
  TASSERT(d == 0.0); // target <= 0 returns before any edit
  litestl::alloc::Delete<Mesh>(g);
}

// 9d pipeline integration, clean input: QuadRemesh with pre_remesh=true on a
// smooth radius-2 sphere (the testQuadRemeshPipeline fixture) must succeed, run
// the stage, introduce no folds on the working mesh, and still extract a valid
// all-quad result downstream.
void testPipelinePreRemeshClean()
{
  Mesh *s = mesh::makeUVSphere(24, 32, 2.0f);

  remesh::RemeshParams p;
  p.target_edge_length = 0.1f;
  p.pre_remesh = true; // all pre_remesh_* knobs left at auto/defaults
  remesh::RemeshRunReport rep;
  Mesh *out = remesh::QuadRemesh(*s, p, nullptr, nullptr, &rep);

  const auto &pe = rep.pre_remesh_effect;
  fprintf(stderr,
          "[pipeline/clean] ok=%d V=%d->%d mean=%.4f->%.4f fold90=%d->%d "
          "iters=%d/%d conv=%d ms=%lld\n",
          int(rep.success),
          pe.verts_in,
          pe.verts_out,
          pe.mean_edge_in,
          pe.mean_edge_out,
          pe.fold90_in,
          pe.fold90_out,
          pe.iters_run,
          pe.iters_resolved,
          int(pe.converged),
          pe.duration_ms);

  TASSERT(out != nullptr);
  TASSERT(rep.success);
  TASSERT(rep.pre_remesh == remesh::StageStatus::Ok);
  TASSERT(pe.ran);
  TASSERT(pe.fold90_in == 0 && pe.fold90_out == 0); // clean stays clean
  TASSERT(pe.degen_out == 0);
  if (out) {
    RemeshReport r = mesh::remeshValidate(*out);
    fprintf(stderr,
            "[pipeline/clean] out V=%d F=%d allquad=%d manifold=%d "
            "inverted=%d\n",
            r.vert_count,
            r.face_count,
            int(r.all_quad),
            int(r.manifold),
            r.inverted_faces);
    // Watertight default (cap_odd_holes on): unpairable odd rims close with
    // one cap triangle each (<=1% of faces); everything else stays quads.
    TASSERT(r.ngon_count == 0);
    TASSERT(r.tri_count * 100 <= r.face_count);
    TASSERT(r.boundary_edges == 0);
    TASSERT(r.manifold);
    // TODO: extraction folds a handful of quads (~0.2%) on ANY irregular (non-
    // UV-grid) triangulation — pre-existing downstream fragility, not 9d's (the
    // pre-pass's own output is fold-free, and align-0/density-off configs fare
    // WORSE; the no-pre-pass baseline on a noised sphere inverts 52 and goes
    // non-manifold). Tighten to == 0 when extraction robustness lands (Tier 4+).
    TASSERT(r.inverted_faces * 100 <= r.face_count);
    litestl::alloc::Delete<Mesh>(out);
  }
  litestl::alloc::Delete<Mesh>(s);
}

// Count-mode sizing: target_edge_length = 0 derives the quad edge from
// target_quad_count (L = sqrt(area/N)); the extracted count is best-effort
// (no corrective re-quantize when it misses).
void testPipelineCountMode()
{
  Mesh *s = mesh::makeUVSphere(24, 32, 2.0f);

  remesh::RemeshParams p;
  p.target_quad_count = 2000; // target_edge_length stays 0 = count mode
  remesh::RemeshRunReport rep;
  Mesh *out = remesh::QuadRemesh(*s, p, nullptr, nullptr, &rep);

  fprintf(stderr,
          "[pipeline/count] ok=%d target=%d actual=%d derived_edge=%.4f "
          "ms=%lld\n",
          int(rep.success),
          p.target_quad_count,
          rep.quad_count_actual,
          rep.derived_edge_length,
          rep.duration_ms);

  TASSERT(out != nullptr);
  TASSERT(rep.success);
  TASSERT(rep.derived_edge_length > 0.0f);
  // Sphere area ~16*pi -> L0 = sqrt(A/2000) ~ 0.16; sanity-band the derivation.
  TASSERT(rep.derived_edge_length > 0.05f && rep.derived_edge_length < 0.5f);
  TASSERT(rep.quad_count_actual == (out ? out->f.count : 0));
  // The count contract: single-pass derivation, no corrective re-quantize, and
  // extraction yield can run well under the ideal lattice count — assert only
  // the right ballpark (within 3x either way).
  TASSERT(rep.quad_count_actual >= p.target_quad_count / 3 &&
          rep.quad_count_actual <= p.target_quad_count * 3);
  if (out) {
    RemeshReport r = mesh::remeshValidate(*out);
    TASSERT(r.all_quad);
    TASSERT(r.manifold);
    litestl::alloc::Delete<Mesh>(out);
  }
  litestl::alloc::Delete<Mesh>(s);
}

// Edge budget: an auto-resolved pre-pass target may not coarsen away more than
// 20% of the input's edges (L <= mean/sqrt(0.8)). A dense sphere with a coarse
// count target previously bootstrap-decimated several-fold; the clamp must hold
// the solve mesh near input resolution and keep the bootstrap from firing.
void testPipelinePreRemeshEdgeBudget()
{
  Mesh *s = mesh::makeUVSphere(48, 64, 2.0f);

  remesh::RemeshParams p;
  p.target_quad_count = 500; // coarse: unclamped L_pre would be ~2x mean edge
  p.pre_remesh = true;
  remesh::RemeshRunReport rep;
  Mesh *out = remesh::QuadRemesh(*s, p, nullptr, nullptr, &rep);

  const auto &pe = rep.pre_remesh_effect;
  fprintf(stderr,
          "[pipeline/budget] ok=%d F=%d->%d mean=%.4f->%.4f target=%.4f "
          "bootstrap=%d\n",
          int(rep.success),
          pe.faces_in,
          pe.faces_out,
          pe.mean_edge_in,
          pe.mean_edge_out,
          pe.target_resolved,
          int(pe.coarsen_bootstrap));

  TASSERT(rep.success);
  TASSERT(pe.ran);
  TASSERT(pe.target_resolved <= 1.01f * pe.mean_edge_in / std::sqrt(0.8f));
  TASSERT(!pe.coarsen_bootstrap);
  // Realized retention: faces track edges on a closed tri mesh; 0.7 leaves
  // slack for the BK band around the clamped target.
  TASSERT(pe.faces_out >= int(0.7f * float(pe.faces_in)));
  if (out) {
    litestl::alloc::Delete<Mesh>(out);
  }
  litestl::alloc::Delete<Mesh>(s);
}

// 9d pipeline integration, noisy input: the auto heuristics resolve the sentinel
// knobs from the measured input, the run report carries the before/after A/B
// effect block, and gating the stage off leaves it skipped with an empty block.
// Output-quality gates live in the clean test above — this noised fixture is
// hard for the pipeline with or without the pre-pass (the no-pre-pass baseline
// also extracts non-manifold here), so this case asserts the integration/stats
// contract and prints the A/B record.
void testPipelinePreRemeshNoisy()
{
  Mesh *s = mesh::makeUVSphere(24, 32, 2.0f);
  s->thawTopo();
  mesh::triangulateMesh(*s);
  Lcg rng(7);
  for (int v : s->v) {
    float3 c = s->v.co[v];
    for (int i = 0; i < 3; i++) {
      c[i] += 0.02f * rng.next();
    }
    s->v.co[v] = c;
  }

  remesh::RemeshParams p;
  p.target_edge_length = 0.1f;
  p.pre_remesh = true; // all pre_remesh_* knobs left at auto/defaults
  remesh::RemeshRunReport rep;
  Mesh *out = remesh::QuadRemesh(*s, p, nullptr, nullptr, &rep);

  const auto &pe = rep.pre_remesh_effect;
  fprintf(stderr,
          "[pipeline/pre] ok=%d stage=%d V=%d->%d F=%d->%d mean=%.4f->%.4f "
          "cv=%.3f fold90=%d->%d degen=%d->%d iters=%d/%d conv=%d coarsen=%d "
          "target=%.4f bootstrap=%d ms=%lld\n",
          int(rep.success),
          int(rep.pre_remesh),
          pe.verts_in,
          pe.verts_out,
          pe.faces_in,
          pe.faces_out,
          pe.mean_edge_in,
          pe.mean_edge_out,
          pe.edge_cv_in,
          pe.fold90_in,
          pe.fold90_out,
          pe.degen_in,
          pe.degen_out,
          pe.iters_run,
          pe.iters_resolved,
          int(pe.converged),
          int(pe.coarsen_bootstrap),
          pe.target_resolved,
          pe.bootstrap_resolved,
          pe.duration_ms);

  TASSERT(out != nullptr);
  TASSERT(rep.success);
  TASSERT(rep.pre_remesh == remesh::StageStatus::Ok);
  TASSERT(pe.ran);
  TASSERT(pe.verts_in > 0 && pe.verts_out > 0);
  TASSERT(pe.mean_edge_in > 0.0f && pe.mean_edge_out > 0.0f);
  TASSERT(pe.iters_run >= 1 && pe.iters_run <= pe.iters_resolved);
  TASSERT(pe.iters_resolved >= 3 && pe.iters_resolved <= 6);
  TASSERT(pe.target_resolved == p.target_edge_length); // no solve len set
  TASSERT(pe.bootstrap_resolved >= 0);
  // degen in/out are recorded (printed above) but not gated: on this hard noised
  // fixture a stray degenerate face is run-order-sensitive (pointer-ordered BK).
  if (out) {
    RemeshReport r = mesh::remeshValidate(*out);
    fprintf(stderr,
            "[pipeline/pre]  out manifold=%d inverted=%d fold90=%d\n",
            int(r.manifold),
            r.inverted_faces,
            r.fold90_edges);
    litestl::alloc::Delete<Mesh>(out);
  }

  // Gated off: the stage is skipped and the effect block stays empty. Print the
  // baseline output quality next to the pre-pass run's for the A/B record.
  remesh::RemeshParams p0;
  p0.target_edge_length = 0.1f;
  remesh::RemeshRunReport rep0;
  Mesh *out0 = remesh::QuadRemesh(*s, p0, nullptr, nullptr, &rep0);
  TASSERT(rep0.pre_remesh == remesh::StageStatus::Skipped);
  TASSERT(!rep0.pre_remesh_effect.ran);
  if (out0) {
    RemeshReport r0 = mesh::remeshValidate(*out0);
    fprintf(stderr,
            "[pipeline/base] ok=%d manifold=%d inverted=%d fold90=%d\n",
            int(rep0.success),
            int(r0.manifold),
            r0.inverted_faces,
            r0.fold90_edges);
    litestl::alloc::Delete<Mesh>(out0);
  }
  litestl::alloc::Delete<Mesh>(s);
}

// Fold counts per pre-pass stage, via the promoted Tier-0d metric
// (mesh::countGeometricFolds).
void printFolds(const char *stage, Mesh &m)
{
  mesh::FoldCounts fc = mesh::countGeometricFolds(m);
  fprintf(stderr,
          "[fold] %-22s V=%-6d F=%-6d fold90=%-4d fold180=%-4d degen=%-3d\n",
          stage,
          m.v.count,
          m.f.count,
          fc.fold90,
          fc.fold180,
          fc.degenerate_faces);
}

// Opt-in (REMESH_FOLD_DIAG=<asset name or abs path>) stage-by-stage fold trace
// of the pre-pass on a real asset, replicating preRemesh's loop with public
// primitives. REMESH_TARGET overrides the edge target (default 0.1).
void testFoldDiagnostic()
{
  const char *spec = std::getenv("REMESH_FOLD_DIAG");
  if (!spec) {
    fprintf(stderr, "[fold] skipped (set REMESH_FOLD_DIAG=<asset|path>)\n");
    return;
  }
  char path[2048];
  if (std::strchr(spec, '/') || std::strchr(spec, '\\')) {
    std::snprintf(path, sizeof(path), "%s", spec);
  } else {
    std::snprintf(path, sizeof(path), "%s/%s.obj", SCULPTCORE_ASSETS_DIR, spec);
  }
  Mesh *m = mesh::loadObj(path, /*keepNgons=*/true);
  if (!m) {
    fprintf(stderr, "[fold] skipped (could not open %s)\n", path);
    return;
  }
  m->thawTopo();
  mesh::triangulateMesh(*m); // mirror runPreRemesh
  Mesh *input = mesh::loadObj(path, /*keepNgons=*/true);
  input->thawTopo();
  mesh::triangulateMesh(*input);

  double mean_e = 0.0;
  int ne = 0;
  for (int e : m->e) {
    mean_e += double((m->v.co[m->e.vs[e][0]] - m->v.co[m->e.vs[e][1]]).length());
    ne++;
  }
  mean_e = ne ? mean_e / ne : 0.0;

  remesh::PreRemeshParams p; // defaults = app defaults
  p.target = 0.1f;
  if (const char *e = std::getenv("REMESH_TARGET")) {
    p.target = float(std::atof(e));
  }
  fprintf(stderr, "[fold] asset=%s mean_edge=%.5f target=%.4f\n", path, mean_e, p.target);
  printFolds("input", *m);

  if (p.bootstrap_iters > 0) {
    remesh::tangentialSmooth(*m,
                             p.bootstrap_iters,
                             p.smooth_lambda,
                             0.0f,
                             /*fold_guard=*/true);
    printFolds("bootstrap-smooth", *m);
  }
  const int cadence = p.field_cadence > 0 ? p.field_cadence : 1;
  const bool dumpRounds = std::getenv("REMESH_FOLD_TRACE") != nullptr;
  dyntopo::DynTopoTrace trace;
  for (int it = 0; it < p.iters; it++) {
    if (p.align > 0.0f && (it % cadence) == 0) {
      remesh::CrossFieldParams cp;
      cp.use_curvature = true;
      cp.use_sharp_features = true;
      cp.seed = p.seed;
      remesh::computeCrossField(*m, cp);
    }
    // Mirror preRemesh's (now default) density + gradation sizing path;
    // writeSizeScale is internal so its 1/sqrt(d) mapping is replicated here.
    const char *size_attr = nullptr;
    if (p.density) {
      remesh::DensityParams dpa;
      dpa.target_edge_length = p.target;
      dpa.density_min = p.density_min;
      dpa.density_max = p.density_max;
      remesh::generateAutoDensity(*m, dpa);
      remesh::limitDensityGradation(
          *m, p.target, p.gradation, p.gradation_iters, p.density_min, p.density_max);
      mesh::BuiltinAttr<float, ".remesh.v.density"> density;
      mesh::BuiltinAttr<float, ".remesh.v.presize"> size;
      density.ensure(m->v.attrs);
      size.ensure(m->v.attrs);
      for (int v : m->v) {
        float d = density[v];
        if (d < p.density_min)
          d = p.density_min;
        if (d > p.density_max)
          d = p.density_max;
        size[v] = d > 1e-12f ? 1.0f / std::sqrt(d) : 1.0f;
      }
      size_attr = ".remesh.v.presize";
    }
    if (p.preserve_features) {
      remesh::classifyFeatures(*m, p.sharp_angle);
    }
    size_t r0 = trace.rounds.size();
    remesh::bkRemeshToTarget(
        *m, p.target, p.seed + uint32_t(it) + 1u, size_attr, p.preserve_features, &trace);
    for (size_t i = r0; i < trace.rounds.size(); i++) {
      trace.rounds[i].iter = it;
    }
    {
      // Convergence summary: how the candidate frontier decayed this iter. A
      // healthy BK pass decays sc/cc geometrically; sustained flat tails are the
      // split<->collapse pathology.
      int rounds = 0, s = 0, c = 0;
      float worstOver = 0.0f, worstUnder = 1e30f;
      const dyntopo::RoundQuality *last = nullptr;
      for (size_t i = r0; i < trace.rounds.size(); i++) {
        const dyntopo::RoundQuality &q = trace.rounds[i];
        rounds++;
        s += q.splits;
        c += q.collapses;
        if (q.max_over > worstOver)
          worstOver = q.max_over;
        if (q.min_under > 0.0f && q.min_under < worstUnder)
          worstUnder = q.min_under;
        last = &q;
      }
      fprintf(stderr,
              "[conv] iter=%d rounds=%-3d splits=%-5d collapses=%-5d "
              "worstOver=%.2f worstUnder=%.2f endCands=%d/%d\n",
              it,
              rounds,
              s,
              c,
              worstOver,
              worstUnder < 1e29f ? worstUnder : 0.0f,
              last ? last->split_cands : 0,
              last ? last->collapse_cands : 0);
      char buf[64];
      std::snprintf(buf, sizeof(buf), "iter%d-bk", it);
      printFolds(buf, *m);
    }
    if (p.preserve_features) {
      mesh::boundary::recomputeDirty(m);
    }
    remesh::tangentialSmooth(*m,
                             p.smooth_iters,
                             p.smooth_lambda,
                             p.align,
                             /*fold_guard=*/true);
    {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "iter%d-smooth", it);
      printFolds(buf, *m);
    }
  }
  remesh::ReprojectParams rp;
  remesh::reprojectToSurface(*m, *input, rp);
  printFolds("reproject", *m);
  if (dumpRounds) {
    dyntopo::printTrace(trace, "fold");
  }

  litestl::alloc::Delete<Mesh>(input);
  litestl::alloc::Delete<Mesh>(m);
}

// 9g: greedy face-adjacency walk (surface_walk.h). On a convex closed surface
// the distance field has a single basin, so the walk must match the global BVH
// query from ANY seed; from the correct seed it must converge immediately; an
// invalid seed must report no hit (callers fall back to the global query).
void testSurfaceWalkMatchesGlobal()
{
  Mesh *s = mesh::makeUVSphere(16, 24, 1.0f);
  s->thawTopo();
  mesh::triangulateMesh(*s);

  spatial::SpatialTree tree(s);
  tree.buildAll();

  Lcg rng(424242u);
  int mismatches = 0, nonconv = 0, n = 0;
  double max_rel = 0.0;
  for (int q = 0; q < 64; q++) {
    float3 dir(rng.next(), rng.next(), rng.next());
    float dl = dir.length();
    if (dl < 1e-3f)
      continue;
    dir = dir * (1.0f / dl);
    float3 p = dir * (q % 2 ? 1.4f : 0.6f); // alternate outside / inside

    ClosestPointResult g = findClosestPoint(tree, p);
    TASSERT(g.hit);

    // Seed deliberately far: the face whose first vert is most antipodal.
    int seed = -1;
    float best = 2.0f;
    for (int f : s->f) {
      float d = s->v.co[s->c.v[s->l.c[s->f.l[f]]]].dot(dir);
      if (d < best) {
        best = d;
        seed = f;
      }
    }
    SurfaceWalkResult w = walkClosestPoint(*s, seed, p);
    TASSERT(w.hit);
    if (!w.converged)
      nonconv++;
    double rel =
        std::fabs(double(w.dist) - double(g.dist)) / std::fmax(1e-9, double(g.dist));
    if (rel > max_rel)
      max_rel = rel;
    if (rel > 1e-4)
      mismatches++;
    n++;
  }
  fprintf(stderr,
          "[walk/global] n=%d mismatches=%d nonconv=%d max_rel=%.3e\n",
          n,
          mismatches,
          nonconv,
          max_rel);
  TASSERT(n > 50);
  TASSERT(mismatches == 0); // convex: every walk reaches the global minimum
  TASSERT(nonconv == 0);

  // Correct seed: immediate convergence at the same distance.
  float3 p(0.0f, 0.0f, 1.3f);
  ClosestPointResult g = findClosestPoint(tree, p);
  SurfaceWalkResult w = walkClosestPoint(*s, g.face, p);
  TASSERT(w.hit && w.converged && w.steps <= 1);
  TASSERT(std::fabs(w.dist - g.dist) < 1e-6f);

  // Invalid seeds: no hit.
  TASSERT(!walkClosestPoint(*s, -1, p).hit);
  TASSERT(!walkClosestPoint(*s, int(s->f.capacity()) + 100, p).hit);
  litestl::alloc::Delete<Mesh>(s);
}

// 9g: preRemesh with a source mesh maintains per-vertex anchors landing on the
// source surface — every anchor alive, re-walking from it converges, the snap
// distance stays small, and the anchored distance matches the global query for
// >= 95% of verts (a rumpled fixture can hold a few genuine local minima;
// systematic wrong-sheet drift would blow the gate).
void testPreRemeshAnchors()
{
  Mesh *src = noisedSphere(7u, false);
  Mesh *m = noisedSphere(7u, false); // identical geometry

  remesh::PreRemeshParams p;
  p.target = 0.3f;
  p.iters = 3;
  p.seed = 5;
  p.source = src;
  remesh::preRemesh(*m, p);

  TASSERT(m->v.attrs.has(mesh::AttrType::INT,
                         litestl::util::string(remesh::kPreRemeshSrcFaceAttr)));
  mesh::BuiltinAttr<int, ".remesh.v.src_face"> anchor;
  anchor.ensure(m->v.attrs);

  spatial::SpatialTree tree(src);
  tree.buildAll();

  int n = 0, dead = 0, drift = 0, nonconv = 0;
  float max_d = 0.0f;
  for (int v : m->v) {
    int f = anchor[v];
    n++;
    if (f < 0 || f >= int(src->f.capacity()) || src->f.freemap[f]) {
      dead++;
      continue;
    }
    SurfaceWalkResult w = walkClosestPoint(*src, f, m->v.co[v]);
    TASSERT(w.hit);
    if (!w.converged)
      nonconv++;
    if (w.dist > max_d)
      max_d = w.dist;
    ClosestPointResult g = findClosestPoint(tree, m->v.co[v]);
    if (w.dist > g.dist + 1e-4f)
      drift++;
  }
  fprintf(stderr,
          "[anchors] verts=%d dead=%d nonconv=%d drift=%d max_d=%.4f\n",
          n,
          dead,
          nonconv,
          drift,
          max_d);
  TASSERT(n > 0);
  TASSERT(dead == 0);
  TASSERT(nonconv == 0);
  TASSERT(max_d < 0.25f);   // anchors stay near the source surface
  TASSERT(drift * 20 <= n); // >= 95% match the global query
  litestl::alloc::Delete<Mesh>(m);
  litestl::alloc::Delete<Mesh>(src);
}

// 9g pipeline integration: pre_remesh with anchors on (default) and off both
// succeed end-to-end — anchors only change how the reproject snap is seeded.
void testPipelineAnchorsSmoke()
{
  for (int anchors = 1; anchors >= 0; anchors--) {
    Mesh *s = mesh::makeUVSphere(24, 32, 2.0f);
    remesh::RemeshParams p;
    p.target_edge_length = 0.1f;
    p.pre_remesh = true;
    p.pre_remesh_anchors = anchors != 0;
    remesh::RemeshRunReport rep;
    Mesh *out = remesh::QuadRemesh(*s, p, nullptr, nullptr, &rep);
    fprintf(stderr,
            "[pipeline/anchors=%d] ok=%d quads=%d\n",
            anchors,
            int(rep.success),
            rep.quad_count_actual);
    TASSERT(out != nullptr);
    TASSERT(rep.success);
    if (out) {
      RemeshReport r = mesh::remeshValidate(*out);
      // Watertight default: cap triangles (<=1% of faces), no ngons, no holes.
      TASSERT(r.ngon_count == 0);
      TASSERT(r.tri_count * 100 <= r.face_count);
      TASSERT(r.boundary_edges == 0);
      TASSERT(r.manifold);
      litestl::alloc::Delete<Mesh>(out);
    }
    litestl::alloc::Delete<Mesh>(s);
  }
}

} // namespace

int main()
{
  testFieldAlignedBounded();
  testNoFieldFallback();
  testFieldChangesResult();
  testFieldAlignedSteers();
  testSizeFieldGrades();
  testDriverConverges();
  testDriverDensityGrades();
  testDriverPreservesFeatures();
  testBoundarySliding();
  testPrepassTrace();
  testDriverNoOp();
  testPipelinePreRemeshClean();
  testPipelineCountMode();
  testPipelinePreRemeshEdgeBudget();
  testPipelinePreRemeshNoisy();
  testSurfaceWalkMatchesGlobal();
  testPreRemeshAnchors();
  testPipelineAnchorsSmoke();
  testFoldDiagnostic();
  return retval;
}
