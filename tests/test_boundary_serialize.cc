// Poly-group boundary flags must survive a save/load round trip: the derived
// overlay (EDGE_POLYGROUP / VERT_CLASS) is TEMP and never written, so a loaded
// mesh has to come back boundary-dirty and reclassify to the same answer the
// pre-save mesh had. Regression for "edge boundary flags are not being updated
// after file load, so poly-group boundaries don't smooth properly".
#include "test_util.h"

#include "mesh/attribute.h"
#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "mesh/mesh_serialize.h"

#include <cstdio>
#include <span>
#include <sstream>

test_init;

using namespace sculptcore::mesh;
using namespace litestl::math;
namespace bnd = sculptcore::mesh::boundary;

namespace {

// Two quads sharing edge v1-v4, painted into groups 0 and 1 (same fixture as
// test_boundary.cc). Reports the shared-edge verts and one outer vert.
void buildTwoGroupQuads(Mesh &m, int &vShared0, int &vShared1, int &vOuter)
{
  int v0 = m.make_vertex(float3(0, 0, 0));
  int v1 = m.make_vertex(float3(1, 0, 0));
  int v2 = m.make_vertex(float3(2, 0, 0));
  int v3 = m.make_vertex(float3(0, 1, 0));
  int v4 = m.make_vertex(float3(1, 1, 0));
  int v5 = m.make_vertex(float3(2, 1, 0));

  auto edge = [&](int a, int b) {
    if (m.find_edge(a, b) == ELEM_NONE) m.make_edge(a, b);
  };
  edge(v0, v1); edge(v1, v4); edge(v4, v3); edge(v3, v0);
  edge(v1, v2); edge(v2, v5); edge(v5, v4);

  int fa[4] = {v0, v1, v4, v3};
  int fb[4] = {v1, v2, v5, v4};
  int faceA = m.make_face(std::span<int>(fa, 4));
  int faceB = m.make_face(std::span<int>(fb, 4));

  AttrRef &gref = m.f.attrs.ensure(AttrType::INT, bnd::FACE_GROUP, /*materialize=*/true);
  AttrData<int> *g = gref.get_data<int>();
  (*g)[faceA] = 0;
  (*g)[faceB] = 1;

  vShared0 = v1;
  vShared1 = v4;
  vOuter = v0;
}

} // namespace

int main()
{
  {
    Mesh src;
    int v1, v4, v0;
    buildTwoGroupQuads(src, v1, v4, v0);
    bnd::markAllDirty(&src);
    bnd::recomputeDirty(&src);

    const int clsV1 = bnd::vertClass(&src, v1);
    const int clsV4 = bnd::vertClass(&src, v4);
    test_assert((clsV1 & bnd::BC_POLYGROUP) != 0);
    test_assert((clsV4 & bnd::BC_POLYGROUP) != 0);
    test_assert((bnd::vertClass(&src, v0) & bnd::BC_POLYGROUP) == 0);

    std::stringstream blob(std::ios::in | std::ios::out | std::ios::binary);
    test_assert(serial::writeMesh(src, blob));

    Mesh dst;
    blob.seekg(0);
    test_assert(serial::readMesh(dst, blob));

    // The source flag (the face `group` layer) is persistent and must come back.
    AttrRef gref = dst.f.attrs.find_attribute(AttrType::INT, bnd::FACE_GROUP);
    test_assert(gref.exists());

    // The derived overlay is not persisted, so the loaded mesh must be dirty —
    // otherwise nothing ever reclassifies and every vert reads BC_NONE.
    test_assert(dst.boundaryDirty);

    // What the app does right after deserialize. The dirty markers are TEMP too,
    // so this used to silently empty the dirty set while leaving boundaryDirty
    // set — the recompute below then classified nothing.
    dst.dropTempAttrs();
    test_assert(dst.boundaryDirty);

    bnd::recomputeDirty(&dst);

    // Vert order is preserved by the compaction (no free slots here), so the
    // same indices carry the same classification.
    test_assert(bnd::vertClass(&dst, v1) == clsV1);
    test_assert(bnd::vertClass(&dst, v4) == clsV4);
    test_assert((bnd::vertClass(&dst, v0) & bnd::BC_POLYGROUP) == 0);

    int shared = dst.find_edge(v1, v4);
    test_assert(shared != ELEM_NONE);
    test_assert(bnd::edgeFlag(&dst, bnd::EDGE_POLYGROUP, shared) == true);

    fprintf(stderr, "boundary_serialize: v1 cls %d -> %d, v4 cls %d -> %d\n", clsV1,
            bnd::vertClass(&dst, v1), clsV4, bnd::vertClass(&dst, v4));
  }

  return test_end();
}
