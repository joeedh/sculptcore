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

#define TASSERT(expr)                                                                     \
  do {                                                                                    \
    if (!(expr)) {                                                                        \
      retval = 1;                                                                         \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                   \
      fflush(stderr);                                                                     \
    }                                                                                     \
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
  explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
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

void testGridCurlDrop()
{
  Mesh *g = makeNoisyGrid(0.3f, 12345u);

  remesh::SingularityAdjustParams sap;
  remesh::SingularityAdjustStats st = remesh::adjustSingularities(*g, sap);

  fprintf(stderr, "[noisy_grid] faces=%d sing=%d curl_before=%.6f curl_after=%.6f\n",
          st.num_faces, st.num_singularities, st.curl_before, st.curl_after);
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
  fprintf(stderr, "[determinism] sing(a,b)=(%d,%d) index(a,b)=(%d,%d) maxdiff=%.3e\n",
          sa.num_singularities, sb.num_singularities, sa.index_sum, sb.index_sum,
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
          st.num_faces, st.num_singularities, st.index_sum, 4 * chi,
          st.curl_before, st.curl_after);
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
  fprintf(stderr, "[pin] vpin=%d before=%d after=%d index_sum=%d 4chi=%ld\n", vpin,
          before, after, st.index_sum, 4 * chi);
  TASSERT(after == before);
  TASSERT(st.index_sum == int(4 * chi)); // == 8
  litestl::alloc::Delete<Mesh>(sph);
}

} // namespace

int main()
{
  testGridCurlDrop();
  testDeterminism();
  testTorusMonotone();
  testPinHonored();
  return retval;
}
