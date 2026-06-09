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

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "mesh/utils/triangulate.h"
#include "remesh/field/cross_field.h"
#include "remesh/preremesh.h"

#include <cmath>
#include <cstdio>

test_init;

#define TASSERT(expr)                                                                     \
  do {                                                                                    \
    if (!(expr)) {                                                                         \
      retval = 1;                                                                          \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                   \
      fflush(stderr);                                                                      \
    }                                                                                      \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::mesh;
using litestl::math::float3;

namespace {

// Deterministic in-plane jitter (keeps z so the grid stays planar and the field
// stays meaningful). LCG, no <random>, no global state.
struct Lcg {
  uint32_t s;
  explicit Lcg(uint32_t seed) : s(seed) {}
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
      if (p[i] < lo[i]) lo[i] = p[i];
      if (p[i] > hi[i]) hi[i] = p[i];
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
  fprintf(stderr, "[bounded] verts=%d diag0=%.4f diag1=%.4f finite=%d\n", vcount0,
          diag0, diag1, finiteCo(*sph));

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
          c[1] >= hi[1] - 1e-4f) {
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

  fprintf(stderr, "[steers] noised=%.5f iso=%.5f field=%.5f\n", e_noised, e_iso,
          e_fld);
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
    fprintf(stderr, "[sizefield/graded] loN=%d loMean=%.4f hiN=%d hiMean=%.4f\n",
            loN, loMean, hiN, hiMean);
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
  p.converge_eps = 1e-3f; // exercise the early-out measure
  remesh::preRemesh(*sph, p);

  float3 lo1, hi1;
  bbox(*sph, lo1, hi1);
  float diag1 = (hi1 - lo1).length();
  double mean = meanEdgeLen(*sph);
  fprintf(stderr, "[driver] verts=%d diag0=%.3f diag1=%.3f mean=%.4f L=%.3f\n",
          sph->v.count, diag0, diag1, mean, L);

  TASSERT(finiteCo(*sph));
  TASSERT(sph->v.count > 0);
  TASSERT(diag1 < diag0 * 1.20f + 1e-4f); // BK + smooth don't blow the mesh up
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
          g->v.count, hiN, hiMean, loN, loMean);
  TASSERT(finiteCo(*g));
  TASSERT(hiN > 0 && loN > 0);            // the field genuinely graded (contrast)
  TASSERT(hiMean < loMean * 0.8);         // and edges track it: denser ⇒ finer
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
    corners[i] = float3((i & 1) ? hi0[0] : lo0[0], (i & 2) ? hi0[1] : lo0[1],
                        (i & 4) ? hi0[2] : lo0[2]);
  }

  // How many of the 8 original corners still have a vertex essentially on them.
  auto countCorners = [&](Mesh &m) {
    int kept = 0;
    for (int i = 0; i < 8; i++) {
      float best = 1e30f;
      for (int v : m.v) {
        float dd = (m.v.co[v] - corners[i]).length();
        if (dd < best) best = dd;
      }
      if (best < 1e-3f) kept++;
    }
    return kept;
  };

  remesh::PreRemeshParams p;
  p.target = L;
  p.iters = 5;
  p.align = 1.0f;
  p.bootstrap_iters = 0;       // clean input: keep features crisp from iter 0
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
          "[driver/features] verts=%d diag0=%.4f diag1=%.4f corners pinned=%d/8 unpinned=%d/8\n",
          cube->v.count, diag0, diag1, cornersKept, cornersKept2);

  TASSERT(finiteCo(*cube));
  TASSERT(cube->v.count > 0);
  TASSERT(cornersKept == 8);          // every corner pinned in place (immortal + fixed)
  TASSERT(diag1 > diag0 * 0.98f);     // pinned silhouette never collapses inward
  TASSERT(cornersKept2 < cornersKept); // pinning preserves corners the plain flow loses
  litestl::alloc::Delete<Mesh>(cube);
  litestl::alloc::Delete<Mesh>(cube2);
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
  fprintf(stderr, "[driver/noop] vcount %d->%d maxdelta=%.3e\n", vcount0,
          g->v.count, d);
  TASSERT(g->v.count == vcount0);
  TASSERT(d == 0.0); // target <= 0 returns before any edit
  litestl::alloc::Delete<Mesh>(g);
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
  testDriverNoOp();
  return retval;
}
