// Wave 4: boundary flag model + lazy dirty classification.
//
// Two quads sharing edge v1-v4, painted into different poly groups. After
// markAllDirty + recomputeDirty:
//   * the shared edge is a derived poly-group boundary; outer edges are not,
//   * its endpoint verts carry BC_POLYGROUP in their classification,
// and the source-flag path (mark an edge sharp -> its verts get BC_SHARP)
// flows through the same lazy recompute.
#include "test_util.h"

#include "mesh/attribute.h"
#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "mesh/mesh_path.h"

#include "litestl/util/vector.h"

#include <cstdio>
#include <span>

test_init;

using namespace sculptcore::mesh;
using namespace litestl::math;
namespace bnd = sculptcore::mesh::boundary;

int main()
{
  {
    Mesh m;
    int v0 = m.make_vertex(float3(0, 0, 0));
    int v1 = m.make_vertex(float3(1, 0, 0));
    int v2 = m.make_vertex(float3(2, 0, 0));
    int v3 = m.make_vertex(float3(0, 1, 0));
    int v4 = m.make_vertex(float3(1, 1, 0));
    int v5 = m.make_vertex(float3(2, 1, 0));

    auto edge = [&](int a, int b) {
      if (m.find_edge(a, b) == ELEM_NONE)
        m.make_edge(a, b);
    };
    edge(v0, v1);
    edge(v1, v4);
    edge(v4, v3);
    edge(v3, v0);
    edge(v1, v2);
    edge(v2, v5);
    edge(v5, v4);

    int fa[4] = {v0, v1, v4, v3};
    int fb[4] = {v1, v2, v5, v4};
    int faceA = m.make_face(std::span<int>(fa, 4));
    int faceB = m.make_face(std::span<int>(fb, 4));

    // Paint two poly groups (faceA=0, faceB=1).
    AttrRef &gref =
        m.f.attrs.ensure(AttrType::INT, bnd::FACE_GROUP, /*materialize=*/true);
    AttrData<int> *g = gref.get_data<int>();
    (*g)[faceA] = 0;
    (*g)[faceB] = 1;

    bnd::markAllDirty(&m);
    bnd::recomputeDirty(&m);

    int eShared = m.find_edge(v1, v4);
    int eOuter = m.find_edge(v0, v1);
    test_assert(eShared != ELEM_NONE && eOuter != ELEM_NONE);

    // Derived poly-group boundary: only the shared edge.
    test_assert(bnd::edgeFlag(&m, bnd::EDGE_POLYGROUP, eShared) == true);
    test_assert(bnd::edgeFlag(&m, bnd::EDGE_POLYGROUP, eOuter) == false);

    // Endpoint verts of the shared edge are classified poly-group boundary.
    test_assert((bnd::vertClass(&m, v1) & bnd::BC_POLYGROUP) != 0);
    test_assert((bnd::vertClass(&m, v4) & bnd::BC_POLYGROUP) != 0);
    // A vert away from the group boundary is not.
    test_assert((bnd::vertClass(&m, v0) & bnd::BC_POLYGROUP) == 0);

    // Source-flag path: mark an outer edge sharp; lazy recompute reflects it on
    // the edge and its endpoint vert classes.
    bnd::setEdgeFlag(&m, bnd::EDGE_SHARP, eOuter, true);
    bnd::recomputeDirty(&m);
    test_assert(bnd::edgeFlag(&m, bnd::EDGE_SHARP, eOuter) == true);
    test_assert((bnd::vertClass(&m, v0) & bnd::BC_SHARP) != 0);
    test_assert((bnd::vertClass(&m, v1) & bnd::BC_SHARP) != 0);
    // v1 now carries both poly-group and sharp bits.
    test_assert((bnd::vertClass(&m, v1) & bnd::BC_POLYGROUP) != 0);

    // Chain endpoints (exactly one dominant-type constraint edge) carry
    // BC_ENDPOINT so a smooth brush relaxes them like interior verts. v0 has one
    // sharp edge; v4 has one poly-group edge; sharp wins the dominant type at v1
    // (one sharp edge), so it too is a sharp endpoint. v3 has no boundary.
    test_assert((bnd::vertClass(&m, v0) & bnd::BC_ENDPOINT) != 0);
    test_assert((bnd::vertClass(&m, v4) & bnd::BC_ENDPOINT) != 0);
    test_assert((bnd::vertClass(&m, v1) & bnd::BC_ENDPOINT) != 0);
    test_assert((bnd::vertClass(&m, v3) & bnd::BC_ENDPOINT) == 0);

    fprintf(stderr,
            "boundary: vclass v0=%d v1=%d v4=%d\n",
            bnd::vertClass(&m, v0),
            bnd::vertClass(&m, v1),
            bnd::vertClass(&m, v4));

    // --- shortest edge path (seam-tool compute core) ---
    // v0=(0,0) to v5=(2,1): the minimal path is 3 unit edges (no diagonals),
    // e.g. v0-v1-v2-v5. Assert endpoints, edge-connectivity, and minimal weight.
    litestl::util::Vector<int> path;
    bool ok = shortestEdgePath(&m, v0, v5, path);
    test_assert(ok);
    test_assert(path.size() >= 2);
    test_assert(path[0] == v0);
    test_assert(path[path.size() - 1] == v5);
    double plen = 0.0;
    bool connected = true;
    for (int i = 1; i < (int)path.size(); i++) {
      int e = m.find_edge(path[i - 1], path[i]);
      if (e == ELEM_NONE) {
        connected = false;
      } else {
        plen += double((m.v.co[path[i]] - m.v.co[path[i - 1]]).length());
      }
    }
    test_assert(connected);
    test_assert(plen > 3.0 - 1e-4 && plen < 3.0 + 1e-4); // minimal = 3 unit edges
    fprintf(stderr, "path: verts=%d weight=%g\n", (int)path.size(), plen);

    // --- shortestEdgePath edge cases ---
    // Disconnected target: an isolated vertex is unreachable.
    int vIso = m.make_vertex(float3(9, 9, 9));
    litestl::util::Vector<int> pathU;
    test_assert(shortestEdgePath(&m, v0, vIso, pathU) == false);
    // Out-of-range indices are rejected, not crashed.
    litestl::util::Vector<int> pathBad;
    test_assert(shortestEdgePath(&m, -1, v0, pathBad) == false);
    test_assert(shortestEdgePath(&m, v0, 999999, pathBad) == false);
  }

  // Junction classification: a 2x2 quad grid whose four interior edges are all
  // seamed makes the center vert a 4-way junction (BC_JUNCTION); each arm's far
  // end has exactly one seam edge (BC_ENDPOINT, no junction).
  {
    Mesh m;
    int v[9];
    for (int j = 0; j < 3; j++) {
      for (int i = 0; i < 3; i++) {
        v[j * 3 + i] = m.make_vertex(float3(float(i), float(j), 0));
      }
    }
    auto quad = [&](int a, int b, int c, int d) {
      int vs[4] = {v[a], v[b], v[c], v[d]};
      m.make_face(std::span<int>(vs, 4));
    };
    quad(0, 1, 4, 3);
    quad(1, 2, 5, 4);
    quad(3, 4, 7, 6);
    quad(4, 5, 8, 7);

    const int center = v[4];
    const int arms[4] = {v[1], v[3], v[5], v[7]};
    for (int a : arms) {
      int e = m.find_edge(center, a);
      test_assert(e != ELEM_NONE);
      bnd::setEdgeFlag(&m, bnd::EDGE_SEAM, e, true);
    }
    bnd::recomputeDirty(&m);

    test_assert((bnd::vertClass(&m, center) & bnd::BC_JUNCTION) != 0);
    test_assert((bnd::vertClass(&m, center) & bnd::BC_ENDPOINT) == 0);
    for (int a : arms) {
      test_assert((bnd::vertClass(&m, a) & bnd::BC_ENDPOINT) != 0);
      test_assert((bnd::vertClass(&m, a) & bnd::BC_JUNCTION) == 0);
    }
  }

  // T1 (audit): the *incremental* dirty path — markFaceDirty + recomputeDirty
  // with NO markAllDirty. Guards the B3 fix: the poly-group brush marks painted
  // faces dirty so the derived boundary refreshes without a full rescan.
  {
    Mesh m;
    int v0 = m.make_vertex(float3(0, 0, 0));
    int v1 = m.make_vertex(float3(1, 0, 0));
    int v2 = m.make_vertex(float3(2, 0, 0));
    int v3 = m.make_vertex(float3(0, 1, 0));
    int v4 = m.make_vertex(float3(1, 1, 0));
    int v5 = m.make_vertex(float3(2, 1, 0));
    auto edge = [&](int a, int b) {
      if (m.find_edge(a, b) == ELEM_NONE)
        m.make_edge(a, b);
    };
    edge(v0, v1);
    edge(v1, v4);
    edge(v4, v3);
    edge(v3, v0);
    edge(v1, v2);
    edge(v2, v5);
    edge(v5, v4);
    int fa[4] = {v0, v1, v4, v3};
    int fb[4] = {v1, v2, v5, v4};
    int faceA = m.make_face(std::span<int>(fa, 4));
    int faceB = m.make_face(std::span<int>(fb, 4));

    AttrRef &gref =
        m.f.attrs.ensure(AttrType::INT, bnd::FACE_GROUP, /*materialize=*/true);
    AttrData<int> *g = gref.get_data<int>();
    (*g)[faceA] = 0;
    (*g)[faceB] = 1;

    // Only mark the painted faces dirty (what the polygroup brush now does),
    // then recompute — without markAllDirty.
    bnd::markFaceDirty(&m, faceA);
    bnd::markFaceDirty(&m, faceB);
    bnd::recomputeDirty(&m);

    int eShared = m.find_edge(v1, v4);
    test_assert(eShared != ELEM_NONE);
    test_assert(bnd::edgeFlag(&m, bnd::EDGE_POLYGROUP, eShared) == true);
    test_assert((bnd::vertClass(&m, v1) & bnd::BC_POLYGROUP) != 0);
    test_assert((bnd::vertClass(&m, v4) & bnd::BC_POLYGROUP) != 0);
  }

  return test_end();
}
