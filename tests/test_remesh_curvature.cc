// M1 foundation test: principal-curvature estimation + BVH closest-point query.
//
//  - Cylinder: kmax ≈ 1/R aligned circumferentially, kmin ≈ 0 aligned axially
//    (the SWAP between the shape operator's eigenvalues and eigenvectors).
//  - UV sphere: kmin ≈ kmax (isotropic) on the equatorial band.
//  - Torus: findClosestPoint matches a brute-force scan over all triangles.
#include "test_util.h"

#include "litestl/math/geom.h"
#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "mesh/utils/closest_point.h"
#include "mesh/utils/triangulate.h"
#include "remesh/field/curvature.h"
#include "remesh/field/feature_tag.h"
#include "spatial/spatial.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

test_init;

// The shared test_assert macro has a known retval=0-on-failure bug; use a local
// one that flips retval (mirrors test_spatial_raycast.cc).
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
using namespace sculptcore::spatial;
using litestl::math::float2;
using litestl::math::float3;

namespace {

void testCylinderCurvature()
{
  const float R = 0.5f;
  Mesh *cyl = mesh::makeCylinder(48, 16, R, 2.0f, /*capped=*/true);
  remesh::computeCurvature(*cyl);

  BuiltinAttr<float3, ".remesh.v.kmin_dir"> kmin_dir;
  BuiltinAttr<float3, ".remesh.v.kmax_dir"> kmax_dir;
  BuiltinAttr<float2, ".remesh.v.k"> kval;
  kmin_dir.ensure(cyl->v.attrs);
  kmax_dir.ensure(cyl->v.attrs);
  kval.ensure(cyl->v.attrs);

  int n = 0;
  double sKmaxDot = 0, sKminDot = 0, sKmax = 0, sKminAbs = 0;
  for (int v : cyl->v) {
    float3 co = cyl->v.co[v];
    float r = std::sqrt(co[0] * co[0] + co[1] * co[1]);
    if (std::fabs(co[2]) > 0.5f) {
      continue; // interior rings only (away from caps)
    }
    if (std::fabs(r - R) > 1e-3f) {
      continue; // side verts only (skip cap centers)
    }
    float3 circ(-co[1], co[0], 0.0f);
    float cl = circ.length();
    if (cl < 1e-6f) {
      continue;
    }
    circ = circ * (1.0f / cl);
    float3 axial(0.0f, 0.0f, 1.0f);

    float dotMax = std::fabs(kmax_dir[v].dot(circ));
    float dotMin = std::fabs(kmin_dir[v].dot(axial));
    float kmin = kval[v][0], kmax = kval[v][1];

    // Loose per-vertex bounds; the means below are checked tightly.
    TASSERT(dotMax > 0.80f);
    TASSERT(dotMin > 0.80f);
    TASSERT(kmax > 1.2f && kmax < 2.8f);
    TASSERT(std::fabs(kmin) < 0.5f);

    sKmaxDot += dotMax;
    sKminDot += dotMin;
    sKmax += kmax;
    sKminAbs += std::fabs(kmin);
    n++;
  }
  TASSERT(n > 0);
  if (n > 0) {
    double mMaxDot = sKmaxDot / n, mMinDot = sKminDot / n;
    double mKmax = sKmax / n, mKminAbs = sKminAbs / n;
    fprintf(stderr,
            "[cyl] n=%d kmaxDot=%.3f kminDot=%.3f kmax=%.3f |kmin|=%.3f (1/R=%.1f)\n",
            n, mMaxDot, mMinDot, mKmax, mKminAbs, 1.0f / R);
    TASSERT(mMaxDot > 0.95);
    TASSERT(mMinDot > 0.95);
    TASSERT(mKmax > 1.7 && mKmax < 2.3);
    TASSERT(mKminAbs < 0.25);
  }
  litestl::alloc::Delete<Mesh>(cyl);
}

void testSphereCurvature()
{
  const float R = 1.0f;
  Mesh *sph = mesh::makeUVSphere(32, 48, R);
  remesh::computeCurvature(*sph);

  BuiltinAttr<float2, ".remesh.v.k"> kval;
  kval.ensure(sph->v.attrs);

  int n = 0;
  double sAniso = 0, sKmax = 0, sKmin = 0;
  for (int v : sph->v) {
    float3 co = sph->v.co[v];
    if (std::fabs(co[2]) > 0.3f) {
      continue; // equatorial band, away from the UV poles
    }
    float kmin = kval[v][0], kmax = kval[v][1];
    float aniso =
        std::fabs(kmax - kmin) / (std::fabs(kmax) + std::fabs(kmin) + 1e-6f);
    sAniso += aniso;
    sKmax += kmax;
    sKmin += kmin;
    n++;
  }
  TASSERT(n > 0);
  if (n > 0) {
    double mAniso = sAniso / n, mKmax = sKmax / n, mKmin = sKmin / n;
    fprintf(stderr, "[sph] n=%d aniso=%.3f kmax=%.3f kmin=%.3f (1/R=%.1f)\n", n,
            mAniso, mKmax, mKmin, 1.0f / R);
    TASSERT(mAniso < 0.35);
    TASSERT(mKmax > 0.6 && mKmax < 1.5);
    TASSERT(mKmin > 0.6 && mKmin < 1.5);
  }
  litestl::alloc::Delete<Mesh>(sph);
}

void testClosestPoint()
{
  Mesh *torus = mesh::makeTorus(96, 64, 1.0f, 0.3f);
  torus->thawTopo();
  mesh::triangulateMesh(*torus); // BVH tris == brute-force tris exactly

  SpatialTree tree(torus);
  tree.buildAll();
  for (auto *node : tree.leaves()) {
    tree.ensure_node_tris(node);
  }

  uint32_t s = 0x9e3779b9u;
  auto rnd = [&]() -> float {
    s = s * 1664525u + 1013904223u;
    return float((s >> 8) & 0xffffffu) / float(0x1000000); // [0,1)
  };

  const int N = 200;
  int fails = 0;
  for (int q = 0; q < N; q++) {
    float3 p((rnd() * 2.0f - 1.0f) * 1.6f, (rnd() * 2.0f - 1.0f) * 1.6f,
             (rnd() * 2.0f - 1.0f) * 0.6f);

    float bestd = 1e30f;
    for (int f : torus->f) {
      int c0 = torus->l.c[torus->f.l[f]];
      int c1 = torus->c.next[c0], c2 = torus->c.next[c1];
      float3 cp = math::closestPointOnTri(p, torus->v.co[torus->c.v[c0]],
                                          torus->v.co[torus->c.v[c1]],
                                          torus->v.co[torus->c.v[c2]]);
      float d = (cp - p).length();
      if (d < bestd) {
        bestd = d;
      }
    }

    ClosestPointResult res = findClosestPoint(tree, p);
    TASSERT(res.hit);
    if (std::fabs(res.dist - bestd) > 1e-3f) {
      fails++;
      fprintf(stderr, "[closest] q=%d brute=%.5f tree=%.5f\n", q, bestd, res.dist);
    }
  }
  TASSERT(fails == 0);
  litestl::alloc::Delete<Mesh>(torus);
}

} // namespace

int main()
{
  testCylinderCurvature();
  testSphereCurvature();
  testClosestPoint();

  // Skip test_end()'s leak check: SpatialTree leaves attribute-name strings
  // live in the alloc tracker (pre-existing; see test_spatial_raycast.cc).
  return retval;
}
