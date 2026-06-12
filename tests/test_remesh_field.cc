// M2 test: the 4-RoSy cross-field solve.
//
//  - Torus  (χ=0): a curvature-aligned field is singularity-free → Σindex==0.
//  - Sphere (χ=2): you can't comb a sphere → Σindex == 4χ == 8 (Poincaré–Hopf);
//    the umbilic surface exercises the smoothest-eigenvector fallback.
//  - Flat grid + uniform stroke: every face is hard-pinned, so the recovered θ
//    aligns to the stroke (validates the constraint plumbing + θ recovery).
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "mesh/utils/triangulate.h"
#include "remesh/field/cross_field.h"

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

namespace {

long chiOf(Mesh &m)
{
  return long(m.v.count) - long(m.e.count) + long(m.f.count);
}

void testTorusField()
{
  Mesh *torus = mesh::makeTorus(48, 32, 1.0f, 0.35f);
  torus->thawTopo();
  mesh::triangulateMesh(*torus);

  remesh::CrossFieldParams p;
  remesh::CrossFieldStats st = remesh::computeCrossField(*torus, p);

  long chi = chiOf(*torus);
  fprintf(stderr, "[torus] faces=%d sing=%d index_sum=%d 4chi=%ld eigen=%d\n",
          st.num_faces, st.num_singularities, st.index_sum, 4 * chi,
          st.solved_eigen);
  TASSERT(st.index_sum == int(4 * chi));
  TASSERT(st.index_sum == 0);
  TASSERT(st.num_singularities == 0);
  litestl::alloc::Delete<Mesh>(torus);
}

void testSphereField()
{
  Mesh *sph = mesh::makeUVSphere(24, 36, 1.0f);
  sph->thawTopo();
  mesh::triangulateMesh(*sph);

  remesh::CrossFieldParams p;
  remesh::CrossFieldStats st = remesh::computeCrossField(*sph, p);

  long chi = chiOf(*sph);
  fprintf(stderr, "[sphere] faces=%d sing=%d index_sum=%d 4chi=%ld eigen=%d\n",
          st.num_faces, st.num_singularities, st.index_sum, 4 * chi,
          st.solved_eigen);
  TASSERT(chi == 2);
  TASSERT(st.index_sum == int(4 * chi)); // == 8
  TASSERT(st.num_singularities > 0);
  litestl::alloc::Delete<Mesh>(sph);
}

// Tier 4: field_smoothness / curvature_weight sweep on a noised sphere.
// Hard invariant: Gauss-Bonnet (Σindex == 4χ) holds at every setting. The
// singularity counts are logged as a diagnostic only — the expected trend is
// non-increasing with smoothness, but it is not a reliable invariant.
void testSmoothnessSweep()
{
  // Fresh mesh per setting: radial position noise creates curvature noise the
  // smoothness term has to fight (deterministic LCG, same seed → same geometry).
  auto makeNoisySphere = []() {
    Mesh *sph = mesh::makeUVSphere(24, 36, 1.0f);
    sph->thawTopo();
    mesh::triangulateMesh(*sph);
    uint32_t s = 9001u;
    auto noise = [&s]() {
      s = s * 1664525u + 1013904223u;
      return double((s >> 8) & 0xffffffu) / double(0x1000000) * 2.0 - 1.0;
    };
    for (int v : sph->v) {
      float3 p = sph->v.co[v];
      if (p.length() > 1e-6f) {
        p *= 1.0f + 0.03f * float(noise());
        sph->v.co[v] = p;
      }
    }
    return sph;
  };

  struct Setting {
    float smoothness, weight;
  };
  const Setting settings[] = {
      {0.25f, 1.0f}, {0.5f, 1.0f}, {1.0f, 1.0f}, {2.0f, 1.0f}, {4.0f, 1.0f},
      {8.0f, 1.0f},  {1.0f, 0.0f}, {1.0f, 0.25f}, {1.0f, 4.0f}, {4.0f, 0.25f},
  };

  for (const Setting &set : settings) {
    Mesh *sph = makeNoisySphere();
    remesh::CrossFieldParams p;
    p.field_smoothness = set.smoothness;
    p.curvature_weight = set.weight;
    remesh::CrossFieldStats st = remesh::computeCrossField(*sph, p);

    long chi = chiOf(*sph);
    fprintf(stderr,
            "[sweep] smoothness=%.2f weight=%.2f sing=%d index_sum=%d 4chi=%ld\n",
            set.smoothness, set.weight, st.num_singularities, st.index_sum, 4 * chi);
    TASSERT(chi == 2);
    TASSERT(st.index_sum == int(4 * chi)); // Gauss-Bonnet at every setting
    TASSERT(st.num_singularities > 0);     // sphere can't be combed
    litestl::alloc::Delete<Mesh>(sph);
  }
}

void testStrokeAlignment()
{
  Mesh *grid = mesh::makeGrid(12, 12, 1.0f);
  grid->thawTopo();

  // Pin a uniform stroke direction on every face.
  const float ang = 0.6f;
  float3 sdir(std::cos(ang), std::sin(ang), 0.0f);
  BuiltinAttr<float3, ".remesh.f.stroke_dir"> stroke;
  stroke.ensure(grid->f.attrs);
  for (int f : grid->f) {
    stroke[f] = sdir;
  }

  // Isolate the stroke: boundary-edge / curvature constraints would otherwise
  // fight the uniform pin on the grid's perimeter faces.
  remesh::CrossFieldParams p;
  p.use_sharp_features = false;
  p.use_curvature = false;
  remesh::CrossFieldStats st = remesh::computeCrossField(*grid, p);

  BuiltinAttr<float, ".remesh.f.theta"> theta;
  theta.ensure(grid->f.attrs);

  int n = 0;
  double maxerr = 0.0;
  for (int f : grid->f) {
    float3 X, Y, Nn;
    remesh::faceFrame(*grid, f, X, Y, Nn);
    float phi = std::atan2(sdir.dot(Y), sdir.dot(X)); // stroke angle in frame
    const double half_pi = 1.57079632679489661923;
    double d = double(theta[f]) - double(phi);
    // Reduce to the nearest π/2 representative ∈ (−π/4, π/4].
    d -= half_pi * std::round(d / half_pi);
    maxerr = std::fmax(maxerr, std::fabs(d));
    n++;
  }
  fprintf(stderr, "[grid] faces=%d maxerr=%.5f\n", n, maxerr);
  TASSERT(n > 0);
  TASSERT(maxerr < 1e-2);
  litestl::alloc::Delete<Mesh>(grid);
}

} // namespace

int main()
{
  testTorusField();
  testSphereField();
  testSmoothnessSweep();
  testStrokeAlignment();
  return retval;
}
