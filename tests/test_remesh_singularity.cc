// M3 test: fixed-period curl reduction (singularity adjustment).
//
//  - Noisy grid: a flat grid has zero Gaussian curvature, so a globally
//    curl-free field exists (geometric floor ≈ 0). Inject sub-quarter-turn phase
//    noise into a clean M2 field, then adjust — the fixed-period Poisson re-solve
//    must drop the curl L2 by ≥1 order of magnitude (curl_after ≤ 0.1·curl_before).
//  - Determinism: two identical noisy grids adjust to identical θ / index totals.
//  - Noisy torus: a curved model has an irreducible geometric curl floor, so the
//    re-solve only has to *decrease* curl monotonically and preserve Σindex==4χ.
//  - Pin honored: a user-pinned singular vertex keeps its index across the
//    adjustment even though the field is re-solved.
//
// Tier-5 pair cancellation (cancelSingularityPairs):
//  - Planted pair: an analytic ±1 pole pair on the grid is annihilated when the
//    separation gate covers it, and untouched when it doesn't.
//  - Inert: a field with no −1 poles (sphere) yields zero attempted pairs.
//  - Pinned: pinning one endpoint blocks the pair (no flip reaches through it).
//  - Determinism: two identical planted grids cancel to identical θ / stats.
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"
#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "mesh/utils/triangulate.h"
#include "remesh/field/cross_field.h"
#include "remesh/field/singularity_adjust.h"

#include <cmath>
#include <cstdio>

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
using litestl::util::Vector;

namespace {

long chiOf(Mesh &m)
{
  return long(m.v.count) - long(m.e.count) + long(m.f.count);
}

// Deterministic LCG noise in [-1, 1] (matches cross_field.cc's generator).
struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed ? seed : 1u)
  {
  }
  double operator()()
  {
    s = s * 1664525u + 1013904223u;
    return double((s >> 8) & 0xffffffu) / double(0x1000000) * 2.0 - 1.0;
  }
};

// Perturb each face's M2 phase by amp·U(−1,1). amp < π/8 keeps the per-edge phase
// difference under π/4, so the period jumps the re-solve reads off are unchanged.
void injectPhaseNoise(Mesh &m, float amp, uint32_t seed)
{
  BuiltinAttr<float, ".remesh.f.theta"> theta;
  theta.ensure(m.f.attrs);
  Rng rng(seed);
  for (int f : m.f) {
    theta[f] = float(double(theta[f]) + amp * rng());
  }
}

Mesh *makeNoisyGrid(float amp, uint32_t seed)
{
  Mesh *g = mesh::makeGrid(16, 16, 1.0f);
  g->thawTopo();
  mesh::triangulateMesh(*g);

  // No data term: the smoothest field on a flat grid is (near) curl-free.
  remesh::CrossFieldParams p;
  p.use_sharp_features = false;
  p.use_curvature = false;
  remesh::computeCrossField(*g, p);

  injectPhaseNoise(*g, amp, seed);
  return g;
}

// Analytic ±1 quarter-index pole pair: θ_world(p) = 0.25·(arg(p−a) − arg(p−b)),
// expressed in each face's frame at its centroid. The atan2 branch jumps are
// 2π/4 = π/2 — invisible mod the cross symmetry — so the implied periods are
// smooth except for the pair they encode.
void plantPairField(Mesh &m, double ax, double ay, double bx, double by)
{
  BuiltinAttr<float, ".remesh.f.theta"> theta;
  theta.ensure(m.f.attrs);
  for (int f : m.f) {
    double cx = 0.0, cy = 0.0;
    int n = 0;
    int c0 = m.l.c[m.f.l[f]], cc = c0;
    do {
      cx += double(m.v.co[m.c.v[cc]][0]);
      cy += double(m.v.co[m.c.v[cc]][1]);
      n++;
      cc = m.c.next[cc];
    } while (cc != c0);
    cx /= double(n);
    cy /= double(n);

    float3 X, Y, N;
    remesh::faceFrame(m, f, X, Y, N);
    double alpha = std::atan2(double(X[1]), double(X[0]));
    double phi = 0.25 * (std::atan2(cy - ay, cx - ax) - std::atan2(cy - by, cx - bx));
    theta[f] = float(phi - alpha);
  }
}

// 16×16 unit grid (spacing 1/15 ≈ 0.0667) with a planted pair ~0.2 apart.
Mesh *makePlantedGrid()
{
  Mesh *g = mesh::makeGrid(16, 16, 1.0f);
  g->thawTopo();
  mesh::triangulateMesh(*g);
  plantPairField(*g, -0.1, 0.0, 0.1, 0.0);
  return g;
}

// Count nonzero-index poles; fills the first two found.
int collectPoles(Mesh &m, int &v0, int &v1)
{
  BuiltinAttr<short, ".remesh.v.pole_index"> pole;
  pole.ensure(m.v.attrs);
  int count = 0;
  v0 = v1 = ELEM_NONE;
  for (int v : m.v) {
    if (pole[v] != 0) {
      if (count == 0) {
        v0 = v;
      } else if (count == 1) {
        v1 = v;
      }
      count++;
    }
  }
  return count;
}

void testGridCurlDrop()
{
  Mesh *g = makeNoisyGrid(0.3f, 12345u);

  remesh::SingularityAdjustParams sap;
  remesh::SingularityAdjustStats st = remesh::adjustSingularities(*g, sap);

  fprintf(stderr,
          "[noisy_grid] faces=%d sing=%d curl_before=%.6f curl_after=%.6f\n",
          st.num_faces,
          st.num_singularities,
          st.curl_before,
          st.curl_after);
  TASSERT(st.curl_before > 1e-3);
  TASSERT(st.curl_after <= 0.1 * st.curl_before);
  litestl::alloc::Delete<Mesh>(g);
}

void testDeterminism()
{
  Mesh *a = makeNoisyGrid(0.3f, 777u);
  Mesh *b = makeNoisyGrid(0.3f, 777u);

  remesh::SingularityAdjustParams sap;
  remesh::SingularityAdjustStats sa = remesh::adjustSingularities(*a, sap);
  remesh::SingularityAdjustStats sb = remesh::adjustSingularities(*b, sap);

  BuiltinAttr<float, ".remesh.f.theta"> ta, tb;
  ta.ensure(a->f.attrs);
  tb.ensure(b->f.attrs);
  double maxdiff = 0.0;
  for (int f : a->f) {
    maxdiff = std::fmax(maxdiff, std::fabs(double(ta[f]) - double(tb[f])));
  }
  fprintf(stderr,
          "[determinism] sing(a,b)=(%d,%d) index(a,b)=(%d,%d) maxdiff=%.3e\n",
          sa.num_singularities,
          sb.num_singularities,
          sa.index_sum,
          sb.index_sum,
          maxdiff);
  TASSERT(sa.num_singularities == sb.num_singularities);
  TASSERT(sa.index_sum == sb.index_sum);
  TASSERT(maxdiff < 1e-6);
  litestl::alloc::Delete<Mesh>(a);
  litestl::alloc::Delete<Mesh>(b);
}

void testTorusMonotone()
{
  Mesh *t = mesh::makeTorus(48, 32, 1.0f, 0.35f);
  t->thawTopo();
  mesh::triangulateMesh(*t);

  remesh::CrossFieldParams p;
  remesh::computeCrossField(*t, p);
  injectPhaseNoise(*t, 0.25f, 4242u);

  remesh::SingularityAdjustParams sap;
  remesh::SingularityAdjustStats st = remesh::adjustSingularities(*t, sap);

  long chi = chiOf(*t);
  fprintf(stderr,
          "[noisy_torus] faces=%d sing=%d index_sum=%d 4chi=%ld "
          "curl_before=%.6f curl_after=%.6f\n",
          st.num_faces,
          st.num_singularities,
          st.index_sum,
          4 * chi,
          st.curl_before,
          st.curl_after);
  // Curved geometry: the curl only has to drop toward the geometric floor.
  TASSERT(st.curl_after < st.curl_before);
  TASSERT(st.index_sum == int(4 * chi)); // == 0, Poincaré–Hopf preserved
  litestl::alloc::Delete<Mesh>(t);
}

void testPinHonored()
{
  Mesh *sph = mesh::makeUVSphere(24, 36, 1.0f);
  sph->thawTopo();
  mesh::triangulateMesh(*sph);

  remesh::CrossFieldParams p;
  remesh::computeCrossField(*sph, p);

  BuiltinAttr<short, ".remesh.v.pole_index"> pole;
  pole.ensure(sph->v.attrs);

  // Find a singular vertex and pin it.
  int vpin = ELEM_NONE;
  short before = 0;
  for (int v : sph->v) {
    if (pole[v] != 0) {
      vpin = v;
      before = pole[v];
      break;
    }
  }
  TASSERT(vpin != ELEM_NONE);

  BuiltinAttr<bool, ".remesh.v.pole_pinned"> pinned;
  pinned.ensure(sph->v.attrs);
  for (int v : sph->v) {
    pinned.set(v, false);
  }
  if (vpin != ELEM_NONE) {
    pinned.set(vpin, true);
  }

  remesh::SingularityAdjustParams sap;
  remesh::SingularityAdjustStats st = remesh::adjustSingularities(*sph, sap);

  long chi = chiOf(*sph);
  short after = (vpin != ELEM_NONE) ? pole[vpin] : 0;
  fprintf(stderr,
          "[pin] vpin=%d before=%d after=%d index_sum=%d 4chi=%ld\n",
          vpin,
          before,
          after,
          st.index_sum,
          4 * chi);
  TASSERT(after == before);
  TASSERT(st.index_sum == int(4 * chi)); // == 8
  litestl::alloc::Delete<Mesh>(sph);
}

void testPlantedPairCancel()
{
  Mesh *g = makePlantedGrid();

  remesh::SingularityAdjustParams sap;
  remesh::SingularityAdjustStats ast = remesh::adjustSingularities(*g, sap);

  BuiltinAttr<short, ".remesh.v.pole_index"> pole;
  pole.ensure(g->v.attrs);
  int v0, v1;
  int npoles = collectPoles(*g, v0, v1);
  fprintf(stderr,
          "[planted] sing=%d index_sum=%d poles=(%d:%d, %d:%d)\n",
          ast.num_singularities,
          ast.index_sum,
          v0,
          v0 != ELEM_NONE ? int(pole[v0]) : 0,
          v1,
          v1 != ELEM_NONE ? int(pole[v1]) : 0);
  // Sign-agnostic — frame handedness may flip both indices together.
  TASSERT(npoles == 2);
  TASSERT(ast.index_sum == 0);
  TASSERT(v0 != ELEM_NONE && v1 != ELEM_NONE && int(pole[v0]) * int(pole[v1]) == -1);

  // Gate below the pair separation (~0.2): the pass must not touch it.
  remesh::SingularityCancelParams tight;
  tight.target_edge_length = 0.02f; // max_dist = 0.03
  remesh::SingularityCancelStats ts = remesh::cancelSingularityPairs(*g, tight);
  fprintf(stderr,
          "[cancel_tight] attempted=%d cancelled=%d sing=%d\n",
          ts.attempted_pairs,
          ts.cancelled_pairs,
          ts.num_singularities);
  TASSERT(ts.attempted_pairs == 0);
  TASSERT(ts.num_singularities == 2);

  // Gate above the pair separation: the pair must annihilate.
  remesh::SingularityCancelParams wide;
  wide.target_edge_length = 0.2f; // max_dist = 0.3
  remesh::SingularityCancelStats ws = remesh::cancelSingularityPairs(*g, wide);
  fprintf(stderr,
          "[cancel_wide] rounds=%d attempted=%d cancelled=%d reverted=%d "
          "sing=%d index_sum=%d curl_after=%.6f\n",
          ws.rounds,
          ws.attempted_pairs,
          ws.cancelled_pairs,
          ws.reverted_rounds,
          ws.num_singularities,
          ws.index_sum,
          ws.curl_after);
  TASSERT(ws.cancelled_pairs >= 1);
  TASSERT(ws.reverted_rounds == 0);
  TASSERT(ws.num_singularities == 0);
  TASSERT(ws.index_sum == 0);
  litestl::alloc::Delete<Mesh>(g);
}

void testCancelInertNoTargets()
{
  Mesh *sph = mesh::makeUVSphere(24, 36, 1.0f);
  sph->thawTopo();
  mesh::triangulateMesh(*sph);

  remesh::CrossFieldParams p;
  p.use_curvature = false;
  p.use_sharp_features = false;
  remesh::computeCrossField(*sph, p);

  remesh::SingularityAdjustParams sap;
  remesh::SingularityAdjustStats ast = remesh::adjustSingularities(*sph, sap);

  // Smoothest field on a sphere: all-positive poles (Σ == 8) — no pair targets.
  BuiltinAttr<short, ".remesh.v.pole_index"> pole;
  pole.ensure(sph->v.attrs);
  int minus = 0;
  for (int v : sph->v) {
    if (pole[v] < 0) {
      minus++;
    }
  }

  remesh::SingularityCancelParams scp;
  scp.target_edge_length = 1.0f; // generous gate: max_dist = 1.5
  remesh::SingularityCancelStats cs = remesh::cancelSingularityPairs(*sph, scp);
  fprintf(stderr,
          "[cancel_inert] minus=%d sing_before=%d sing_after=%d attempted=%d "
          "index_sum=%d\n",
          minus,
          ast.num_singularities,
          cs.num_singularities,
          cs.attempted_pairs,
          cs.index_sum);
  TASSERT(minus == 0);
  TASSERT(cs.attempted_pairs == 0);
  TASSERT(cs.num_singularities == ast.num_singularities);
  TASSERT(cs.index_sum == 8);
  litestl::alloc::Delete<Mesh>(sph);
}

void testCancelPinnedSkip()
{
  Mesh *g = makePlantedGrid();

  remesh::SingularityAdjustParams sap;
  remesh::adjustSingularities(*g, sap);

  BuiltinAttr<short, ".remesh.v.pole_index"> pole;
  pole.ensure(g->v.attrs);
  int v0, v1;
  int npoles = collectPoles(*g, v0, v1);
  TASSERT(npoles == 2);
  if (npoles != 2) {
    litestl::alloc::Delete<Mesh>(g);
    return;
  }

  // Pin the −1 pole: it can be neither traversed through nor consumed.
  int vneg = pole[v0] < 0 ? v0 : v1;
  BuiltinAttr<bool, ".remesh.v.pole_pinned"> pinned;
  pinned.ensure(g->v.attrs);
  for (int v : g->v) {
    pinned.set(v, false);
  }
  pinned.set(vneg, true);
  short before0 = pole[v0], before1 = pole[v1];

  remesh::SingularityCancelParams scp;
  scp.target_edge_length = 0.2f;
  remesh::SingularityCancelStats cs = remesh::cancelSingularityPairs(*g, scp);
  fprintf(stderr,
          "[cancel_pin] vneg=%d attempted=%d sing=%d poles=(%d,%d)\n",
          vneg,
          cs.attempted_pairs,
          cs.num_singularities,
          int(pole[v0]),
          int(pole[v1]));
  TASSERT(cs.attempted_pairs == 0);
  TASSERT(cs.num_singularities == 2);
  TASSERT(pole[v0] == before0 && pole[v1] == before1);
  litestl::alloc::Delete<Mesh>(g);
}

void testCancelDeterminism()
{
  Mesh *a = makePlantedGrid();
  Mesh *b = makePlantedGrid();

  remesh::SingularityAdjustParams sap;
  remesh::adjustSingularities(*a, sap);
  remesh::adjustSingularities(*b, sap);

  remesh::SingularityCancelParams scp;
  scp.target_edge_length = 0.2f;
  remesh::SingularityCancelStats ca = remesh::cancelSingularityPairs(*a, scp);
  remesh::SingularityCancelStats cb = remesh::cancelSingularityPairs(*b, scp);

  BuiltinAttr<float, ".remesh.f.theta"> ta, tb;
  ta.ensure(a->f.attrs);
  tb.ensure(b->f.attrs);
  double maxdiff = 0.0;
  for (int f : a->f) {
    maxdiff = std::fmax(maxdiff, std::fabs(double(ta[f]) - double(tb[f])));
  }
  fprintf(stderr,
          "[cancel_determinism] attempted=(%d,%d) cancelled=(%d,%d) sing=(%d,%d) "
          "maxdiff=%.3e\n",
          ca.attempted_pairs,
          cb.attempted_pairs,
          ca.cancelled_pairs,
          cb.cancelled_pairs,
          ca.num_singularities,
          cb.num_singularities,
          maxdiff);
  TASSERT(ca.attempted_pairs == cb.attempted_pairs);
  TASSERT(ca.cancelled_pairs == cb.cancelled_pairs);
  TASSERT(ca.num_singularities == cb.num_singularities);
  TASSERT(maxdiff < 1e-6);
  litestl::alloc::Delete<Mesh>(a);
  litestl::alloc::Delete<Mesh>(b);
}

} // namespace

int main()
{
  testGridCurlDrop();
  testDeterminism();
  testTorusMonotone();
  testPinHonored();
  testPlantedPairCancel();
  testCancelInertNoTargets();
  testCancelPinnedSkip();
  testCancelDeterminism();
  return retval;
}
