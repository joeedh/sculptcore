// Tier 2a test: curvature tensor-field smoothing (computeCurvature smooth_iters).
//
//  - Noised cylinder: radial noise scrambles the per-vertex curvature
//    directions; Jacobi diffusion of the shape-operator field must drop the mean
//    kmax_dir angular error vs the analytic circumferential direction.
//  - Clean cylinder: smoothing must NOT wreck an already-coherent field (stays
//    low-error, no over-smoothing).
//  - UV sphere: smoothing preserves umbilic isotropy (|kmin-kmax| stays small).
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "remesh/field/curvature.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

test_init;

// The shared test_assert macro has a known retval=0-on-failure bug; use a local
// one that flips retval (mirrors test_remesh_curvature.cc).
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
using litestl::math::float2;
using litestl::math::float3;

namespace {

// Mean angular error (radians) of kmax_dir vs the analytic circumferential
// direction over the cylinder's interior side verts. Direction-ambiguous, so
// |dot| (a 4-RoSy axis and its negation are the same constraint).
double cylKmaxError(Mesh &m, const remesh::CurvatureParams &cp, int &nOut)
{
  remesh::computeCurvature(m, cp);

  BuiltinAttr<float3, ".remesh.v.kmax_dir"> kmax_dir;
  kmax_dir.ensure(m.v.attrs);

  double sErr = 0.0;
  int n = 0;
  for (int v : m.v) {
    float3 co = m.v.co[v];
    if (std::fabs(co[2]) > 0.7f) {
      continue; // interior rings only (away from the caps at z = +-1)
    }
    float r = std::sqrt(co[0] * co[0] + co[1] * co[1]);
    if (r < 0.3f) {
      continue; // skip cap-center verts (radial noise leaves them near r=0)
    }
    // Radial noise scales (x,y) but not the angular position, so the analytic
    // circumferential tangent (-y, x, 0) is preserved exactly.
    float3 circ(-co[1], co[0], 0.0f);
    float cl = circ.length();
    if (cl < 1e-6f) {
      continue;
    }
    circ = circ * (1.0f / cl);

    float dot = std::fabs(kmax_dir[v].dot(circ));
    dot = dot > 1.0f ? 1.0f : dot;
    sErr += std::acos(dot);
    n++;
  }
  nOut = n;
  return n > 0 ? sErr / n : 0.0;
}

// Deterministic LCG in [0,1).
struct Rng {
  uint32_t s;
  float next()
  {
    s = s * 1664525u + 1013904223u;
    return float((s >> 8) & 0xffffffu) / float(0x1000000);
  }
};

void testNoisedCylinder()
{
  const float R = 0.5f;

  // Clean reference: smoothing must keep an already-coherent field low-error.
  {
    Mesh *cyl = mesh::makeCylinder(48, 16, R, 2.0f, /*capped=*/true);
    int n0 = 0, nS = 0;
    double e0 = cylKmaxError(*cyl, {0, 0.5f}, n0);
    double eS = cylKmaxError(*cyl, {8, 0.5f}, nS);
    fprintf(stderr, "[clean cyl] n=%d err0=%.4f err8=%.4f\n", n0, e0, eS);
    TASSERT(n0 > 0);
    TASSERT(eS < 0.15);      // stays well-aligned
    TASSERT(eS < e0 + 0.05); // not made meaningfully worse
    litestl::alloc::Delete<Mesh>(cyl);
  }

  // Noised: radial perturbation scrambles directions; smoothing must recover.
  {
    Mesh *cyl = mesh::makeCylinder(48, 16, R, 2.0f, /*capped=*/true);
    Rng rng{0x1234567u};
    for (int v : cyl->v) {
      float3 co = cyl->v.co[v];
      float r = std::sqrt(co[0] * co[0] + co[1] * co[1]);
      if (r < 1e-5f) {
        continue; // leave cap centers on-axis
      }
      float3 radial(co[0] / r, co[1] / r, 0.0f);
      float amp = 0.02f * (rng.next() * 2.0f - 1.0f);
      cyl->v.co[v] = co + radial * amp;
    }

    int n0 = 0, nS = 0;
    double e0 = cylKmaxError(*cyl, {0, 0.5f}, n0);
    double eS = cylKmaxError(*cyl, {8, 0.5f}, nS);
    fprintf(stderr, "[noised cyl] n=%d err0=%.4f err8=%.4f\n", n0, e0, eS);
    TASSERT(n0 > 0);
    TASSERT(e0 > 0.05);     // noise actually perturbed the field
    TASSERT(eS < e0);       // smoothing reduced the error
    TASSERT(eS < 0.9 * e0); // by a meaningful margin
    litestl::alloc::Delete<Mesh>(cyl);
  }
}

void testSphereIsotropy()
{
  const float R = 1.0f;
  Mesh *sph = mesh::makeUVSphere(32, 48, R);
  remesh::computeCurvature(*sph, {8, 0.5f});

  BuiltinAttr<float2, ".remesh.v.k"> kval;
  kval.ensure(sph->v.attrs);

  int n = 0;
  double sAniso = 0;
  for (int v : sph->v) {
    float3 co = sph->v.co[v];
    if (std::fabs(co[2]) > 0.3f) {
      continue; // equatorial band, away from the UV poles
    }
    float kmin = kval[v][0], kmax = kval[v][1];
    float aniso = std::fabs(kmax - kmin) / (std::fabs(kmax) + std::fabs(kmin) + 1e-6f);
    sAniso += aniso;
    n++;
  }
  TASSERT(n > 0);
  if (n > 0) {
    double mAniso = sAniso / n;
    fprintf(stderr, "[sph smoothed] n=%d aniso=%.4f\n", n, mAniso);
    TASSERT(mAniso < 0.35); // Smoothing keeps the sphere's near-umbilic curvature isotropic instead of amplifying it into anisotropy.
  }
  litestl::alloc::Delete<Mesh>(sph);
}

} // namespace

int main()
{
  testNoisedCylinder();
  testSphereIsotropy();
  return retval;
}
