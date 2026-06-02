// Boundary-preserving edge split (dyntopo integration). Splitting an edge must:
//   * propagate the parent edge's boundary source flags (e.g. seam) to BOTH
//     child edges,
//   * interpolate per-corner UVs onto the new midpoint corner *per face*, so a
//     seam (UV discontinuity) does not bleed across the split, and
//   * carry the poly-group `group` face attr onto the rebuilt triangles.
#include "test_util.h"

#include "mesh/attribute.h"
#include "mesh/attribute_enums.h"
#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "mesh/utils/edge_split.h"

#include <cstdio>
#include <span>

test_init;

using namespace sculptcore::mesh;
using namespace litestl::math;
namespace bnd = sculptcore::mesh::boundary;

static int cornerAt(Mesh &m, int f, int v)
{
  int l = m.f.l[f], c0 = m.l.c[l], c = c0;
  do {
    if (m.c.v[c] == v) return c;
    c = m.c.next[c];
  } while (c != c0);
  return ELEM_NONE;
}

int main()
{
 {
  Mesh m;
  // Two triangles sharing interior edge v0-v2.
  int v0 = m.make_vertex(float3(0, 0, 0));
  int v1 = m.make_vertex(float3(1, 0, 0));
  int v2 = m.make_vertex(float3(1, 1, 0));
  int v3 = m.make_vertex(float3(0, 1, 0));
  int a[3] = {v0, v1, v2};
  int b[3] = {v0, v2, v3};
  int fA = m.make_face(std::span<int>(a, 3));
  int fB = m.make_face(std::span<int>(b, 3));

  // Poly-group ids per face.
  AttrRef &gref = m.f.attrs.ensure(AttrType::INT, bnd::FACE_GROUP, /*materialize=*/true);
  AttrData<int> *g = gref.get_data<int>();
  (*g)[fA] = 1;
  (*g)[fB] = 2;

  // UV corner layer, with a deliberate discontinuity across the v0-v2 edge:
  // face A's corners use (0..1) coords, face B's use (10..11). The split's
  // midpoint corner must average within each face, never across.
  AttrRef &uref = m.c.attrs.ensure(AttrType::FLOAT2, "uv", /*materialize=*/true);
  uref.use = AttrUse::UV;
  AttrData<float2> *uv = uref.get_data<float2>();
  (*uv)[cornerAt(m, fA, v0)] = float2(0, 0);
  (*uv)[cornerAt(m, fA, v1)] = float2(1, 0);
  (*uv)[cornerAt(m, fA, v2)] = float2(1, 1);
  (*uv)[cornerAt(m, fB, v0)] = float2(10, 10);
  (*uv)[cornerAt(m, fB, v2)] = float2(11, 11);
  (*uv)[cornerAt(m, fB, v3)] = float2(10, 11);

  int eSplit = m.find_edge(v0, v2);
  test_assert(eSplit != ELEM_NONE);
  bnd::setEdgeFlag(&m, bnd::EDGE_SEAM, eSplit, true);

  EdgeSplitResult res;
  test_assert(splitEdge(m, eSplit, &res));
  int vm = res.new_vert;
  test_assert(vm != ELEM_NONE);

  // 1. Both child edges inherit the seam flag.
  int e0 = m.find_edge(v0, vm), e1 = m.find_edge(vm, v2);
  test_assert(e0 != ELEM_NONE && e1 != ELEM_NONE);
  test_assert(bnd::edgeFlag(&m, bnd::EDGE_SEAM, e0) == true);
  test_assert(bnd::edgeFlag(&m, bnd::EDGE_SEAM, e1) == true);

  // 2. Poly-group preserved: still exactly two group-1 faces and two group-2.
  int n1 = 0, n2 = 0;
  for (int f : m.f) {
    if ((*g)[f] == 1) n1++;
    else if ((*g)[f] == 2) n2++;
  }
  test_assert(n1 == 2 && n2 == 2);

  // 3. The midpoint corner UV averages *within* each face (no cross-seam bleed):
  // group-1 faces -> ~(0.5,0.5); group-2 faces -> ~(10.5,10.5).
  int checkedA = 0, checkedB = 0;
  for (int f : m.f) {
    int cm = cornerAt(m, f, vm);
    if (cm == ELEM_NONE) continue;
    float2 u = (*uv)[cm];
    if ((*g)[f] == 1) {
      test_assert((u - float2(0.5f, 0.5f)).length() < 1e-4f);
      checkedA++;
    } else if ((*g)[f] == 2) {
      test_assert((u - float2(10.5f, 10.5f)).length() < 1e-4f);
      checkedB++;
    }
  }
  test_assert(checkedA == 2 && checkedB == 2);

  // 4. Splitting a NON-seam edge must NOT produce seam-flagged children/spokes
  // (regression: fresh edges must read false, not garbage/leaked seam bits).
  int eNon = m.find_edge(v0, v1);
  test_assert(eNon != ELEM_NONE);
  test_assert(bnd::edgeFlag(&m, bnd::EDGE_SEAM, eNon) == false);
  EdgeSplitResult res2;
  test_assert(splitEdge(m, eNon, &res2));
  int vm2 = res2.new_vert;
  for (int e : m.e) {
    // every edge incident to the new non-seam midpoint must be non-seam
    int a2 = m.e.vs[e][0], b2 = m.e.vs[e][1];
    if (a2 == vm2 || b2 == vm2) {
      test_assert(bnd::edgeFlag(&m, bnd::EDGE_SEAM, e) == false);
    }
  }

  fprintf(stderr, "edge_split_boundary: vm=%d groups(%d,%d) uv ok\n", vm, n1, n2);
 }
  return test_end();
}
