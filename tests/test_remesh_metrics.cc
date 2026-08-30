// Tier-0 quality-metric test (plans/quad-remeshing-filtering.md). Spot-checks the
// extended RemeshReport fields computed by remeshValidate / computeTier0Metrics
// on meshes whose component / boundary-loop / regularity / angle numbers are
// known by hand:
//
//  - Grid: one component, one boundary loop, fully regular interior, 90deg
//    corners, uniform cell area (ratios == 1).
//  - Cylinder (uncapped): one component, two boundary loops (the two rims),
//    regular interior.
//  - Sphere: closed -> zero boundary loops, one component (pole fans make some
//    interior verts irregular, so regular_frac < 1).
//  - Two disjoint quads: component_count == 2, two boundary loops, no interior.
//  - Holed plane (3x3 quads minus the center): one component, TWO boundary loops
//    (outer perimeter + the hole rim) — the definition that distinguishes a
//    boundary loop from a genus handle.
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/mesh_shapes.h"
#include "mesh/utils/mesh_validate.h"

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

namespace {

void reportT0(const char *name, const RemeshReport &r)
{
  fprintf(stderr,
          "[%s] comp=%d loops=%d interior=%d irr=%d reg=%.4f maxCompIrr=%d "
          "areaR=%.4f edgeR=%.4f minAng=%.4f\n",
          name,
          r.component_count,
          r.boundary_loop_count,
          r.interior_vert_count,
          r.irregular_interior_verts,
          r.regular_interior_frac,
          r.max_component_irregular,
          r.max_adjacent_area_ratio,
          r.max_adjacent_edge_ratio,
          r.min_interior_angle);
}

// Two unit quads separated along +X (disjoint => two face-components, two
// boundary loops, no interior verts).
Mesh *makeTwoQuads()
{
  Mesh *m = litestl::alloc::New<Mesh>("test twoquads");
  auto quad = [&](float ox) {
    litestl::util::Vector<int> vs;
    vs.append(m->make_vertex(float3(ox + 0, 0, 0)));
    vs.append(m->make_vertex(float3(ox + 1, 0, 0)));
    vs.append(m->make_vertex(float3(ox + 1, 1, 0)));
    vs.append(m->make_vertex(float3(ox + 0, 1, 0)));
    m->make_face(vs);
  };
  quad(0.0f);
  quad(5.0f);
  m->recalc_normals();
  return m;
}

// A 3x3 array of unit quads (4x4 verts) with the CENTER cell removed: one
// component, an outer boundary loop, and a separate inner hole-rim loop.
Mesh *makeHoledPlane()
{
  Mesh *m = litestl::alloc::New<Mesh>("test holedplane");
  int grid[4][4];
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++)
      grid[x][y] = m->make_vertex(float3(float(x), float(y), 0.0f));
  for (int cy = 0; cy < 3; cy++) {
    for (int cx = 0; cx < 3; cx++) {
      if (cx == 1 && cy == 1)
        continue; // the hole
      litestl::util::Vector<int> vs;
      vs.append(grid[cx][cy]);
      vs.append(grid[cx + 1][cy]);
      vs.append(grid[cx + 1][cy + 1]);
      vs.append(grid[cx][cy + 1]);
      m->make_face(vs);
    }
  }
  m->recalc_normals();
  return m;
}

void testGridMetrics()
{
  Mesh *g = makeGrid(5, 5, 1.0f); // 4x4 quads, 3x3 interior verts
  RemeshReport r = remeshValidate(*g);
  reportT0("grid", r);
  TASSERT(r.all_quad);
  TASSERT(r.component_count == 1);
  TASSERT(r.boundary_loop_count == 1);
  TASSERT(r.interior_vert_count == 9);
  TASSERT(r.irregular_interior_verts == 0);
  TASSERT(r.regular_interior_frac == 1.0f);
  TASSERT(r.max_component_irregular == 0);
  TASSERT(r.max_adjacent_area_ratio < 1.001f); // uniform cells
  TASSERT(r.max_adjacent_edge_ratio < 1.001f);
  TASSERT(std::fabs(r.min_interior_angle - 1.5708f) < 0.01f); // 90deg corners
  TASSERT(r.parametrization_folds == -1); // unset (no parametrization)
  litestl::alloc::Delete<Mesh>(g);
}

void testCylinderMetrics()
{
  Mesh *c = makeCylinder(24, 6, 0.5f, 2.0f, /*capped=*/false);
  RemeshReport r = remeshValidate(*c);
  reportT0("cylinder", r);
  TASSERT(r.component_count == 1);
  TASSERT(r.boundary_loop_count == 2); // top + bottom rim
  TASSERT(r.irregular_interior_verts == 0);
  TASSERT(r.regular_interior_frac == 1.0f);
  litestl::alloc::Delete<Mesh>(c);
}

void testSphereMetrics()
{
  Mesh *s = makeUVSphere(16, 24, 1.0f);
  RemeshReport r = remeshValidate(*s);
  reportT0("sphere", r);
  TASSERT(r.component_count == 1);
  TASSERT(r.boundary_loop_count == 0); // closed
  // Pole fans + the band adjacent to them make some interior verts irregular.
  TASSERT(r.irregular_interior_verts > 0);
  TASSERT(r.regular_interior_frac < 1.0f);
  TASSERT(r.max_component_irregular == r.irregular_interior_verts); // one comp
  litestl::alloc::Delete<Mesh>(s);
}

void testTwoComponentMetrics()
{
  Mesh *m = makeTwoQuads();
  RemeshReport r = remeshValidate(*m);
  reportT0("two-quads", r);
  TASSERT(r.all_quad);
  TASSERT(r.component_count == 2);
  TASSERT(r.boundary_loop_count == 2); // each quad's perimeter
  TASSERT(r.interior_vert_count == 0);
  TASSERT(r.regular_interior_frac == 1.0f); // vacuous (no interior)
  TASSERT(r.max_component_irregular == 0);
  litestl::alloc::Delete<Mesh>(m);
}

void testHoledPlaneMetrics()
{
  Mesh *m = makeHoledPlane();
  RemeshReport r = remeshValidate(*m);
  reportT0("holed-plane", r);
  TASSERT(r.all_quad);
  TASSERT(r.face_count == 8);          // 9 - center
  TASSERT(r.component_count == 1);     // the ring is edge-connected
  TASSERT(r.boundary_loop_count == 2); // outer perimeter + hole rim
  litestl::alloc::Delete<Mesh>(m);
}

void testBoundaryDeviation()
{
  Mesh *ref = makeGrid(5, 5, 1.0f); // rim: 16 edges / 16 verts on the +-0.5 square
  Mesh *test = makeGrid(5, 5, 1.0f);

  BoundaryDeviation bd = boundaryDeviation(*ref, *test);
  fprintf(stderr,
          "[bnd-dev/identical] refE=%d testV=%d mean=%g max=%g\n",
          bd.ref_boundary_edges,
          bd.test_boundary_verts,
          bd.mean_dist,
          bd.max_dist);
  TASSERT(bd.ref_boundary_edges == 16);
  TASSERT(bd.test_boundary_verts == 16);
  TASSERT(bd.max_dist < 1e-6f);

  // Push the mid-edge rim vert at (0, -0.5) outward (-y) by 0.25; an interior
  // vert is located too for the no-participation check below.
  int rim = -1, inner = -1;
  for (int vi : test->v) {
    float3 co = test->v.co[vi];
    if (std::fabs(co[0]) < 1e-4f && std::fabs(co[1] + 0.5f) < 1e-4f)
      rim = vi;
    if (std::fabs(co[0]) < 1e-4f && std::fabs(co[1]) < 1e-4f)
      inner = vi;
  }
  TASSERT(rim >= 0 && inner >= 0);
  {
    float3 co = test->v.co[rim];
    co[1] -= 0.25f;
    test->v.co[rim] = co;
  }
  bd = boundaryDeviation(*ref, *test);
  fprintf(stderr, "[bnd-dev/perturbed] mean=%g max=%g\n", bd.mean_dist, bd.max_dist);
  TASSERT(std::fabs(bd.max_dist - 0.25f) < 1e-5f);
  TASSERT(std::fabs(bd.mean_dist - 0.25f / 16.0f) < 1e-5f);

  // Interior verts don't participate in the metric.
  {
    float3 co = test->v.co[inner];
    co[2] += 100.0f;
    test->v.co[inner] = co;
  }
  BoundaryDeviation bd2 = boundaryDeviation(*ref, *test);
  TASSERT(bd2.max_dist == bd.max_dist);
  TASSERT(bd2.mean_dist == bd.mean_dist);

  // Closed mesh on either side -> the corresponding count stays 0.
  Mesh *sphere = makeUVSphere(16, 24, 1.0f);
  BoundaryDeviation bd3 = boundaryDeviation(*sphere, *test);
  TASSERT(bd3.ref_boundary_edges == 0);
  TASSERT(bd3.test_boundary_verts == 0);
  BoundaryDeviation bd4 = boundaryDeviation(*ref, *sphere);
  TASSERT(bd4.ref_boundary_edges == 16);
  TASSERT(bd4.test_boundary_verts == 0);

  litestl::alloc::Delete<Mesh>(sphere);
  litestl::alloc::Delete<Mesh>(test);
  litestl::alloc::Delete<Mesh>(ref);
}

} // namespace

int main()
{
  testGridMetrics();
  testCylinderMetrics();
  testSphereMetrics();
  testTwoComponentMetrics();
  testHoledPlaneMetrics();
  testBoundaryDeviation();
  return retval;
}
