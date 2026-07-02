/* Per-face displacement bounds (displacementAndSubSurf plan, F2 gate): the
 * `.detail.bound` FACE attribute pads owning-leaf AABBs in regen_node_bounds
 * (REYES displacement-bound style) via the setFaceDisplacementBounds dirty
 * hook, ancestors regen through the flag walk, and castRay's broad phase stays
 * conservative (all pre-pad hits still hit; misses outside the mesh still
 * miss). */
#include "test_util.h"

#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "spatial/spatial.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace sculptcore::spatial;
using namespace litestl;
using namespace litestl::math;
using litestl::util::Vector;

/* n×n triangulated quad grid on z=0, spanning [-0.5, 0.5]². */
static void buildGrid(Mesh &m, int n)
{
  Vector<int> grid;
  grid.resize(n * n);
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      float fx = float(x) / float(n - 1) - 0.5f;
      float fy = float(y) / float(n - 1) - 0.5f;
      grid[y * n + x] = m.make_vertex(float3(fx, fy, 0.0f));
    }
  }
  for (int y = 0; y < n - 1; y++) {
    for (int x = 0; x < n - 1; x++) {
      int a = grid[y * n + x], b = grid[y * n + x + 1];
      int c = grid[(y + 1) * n + x + 1], d = grid[(y + 1) * n + x];
      int t0[3] = {a, b, c}, t1[3] = {a, c, d};
      m.make_face(std::span<int>(t0, 3));
      m.make_face(std::span<int>(t1, 3));
    }
  }
}

static bool castDown(SpatialTree &tree, float x, float y, CastRayIsect &out)
{
  return tree.castRay(float3(x, y, 1.0f), float3(0.0f, 0.0f, -1.0f), out);
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Mesh *m = alloc::New<Mesh>("test mesh");
  const int N = 16;
  buildGrid(*m, N);

  SpatialTree tree(m);
  tree.leaf_limit = 16;
  tree.buildAll();
  for (auto *node : tree.leaves()) {
    tree.ensure_node_tris(node);
  }

  const float3 rootMin0 = tree.getRoot()->aabb.min;
  const float3 rootMax0 = tree.getRoot()->aabb.max;

  // Pick a face near the grid center and one near a corner.
  int centerFace = -1, cornerFace = -1;
  for (int f : m->f) {
    if (centerFace < 0) {
      centerFace = f;
    }
    cornerFace = f;
  }
  // First face is at the (0,0) corner; last at the far corner — use the first
  // as "bounded" and the last as "unbounded control".
  int boundedFace = centerFace;
  test_assert(boundedFace >= 0 && cornerFace >= 0 && boundedFace != cornerFace);

  int boundedLeafId = tree.treeMesh.f.node[boundedFace];
  int controlLeafId = tree.treeMesh.f.node[cornerFace];
  test_assert(boundedLeafId != 0 && controlLeafId != 0);
  test_assert(boundedLeafId != controlLeafId);
  SpatialNode *boundedLeaf = tree.node_from_id(boundedLeafId);
  SpatialNode *controlLeaf = tree.node_from_id(controlLeafId);
  float3 controlMin0 = controlLeaf->aabb.min;
  float3 controlMax0 = controlLeaf->aabb.max;

  // --- apply a displacement bound to one face ---
  const float pad = 0.25f;
  int faces[1] = {boundedFace};
  float bounds[1] = {pad};
  tree.setFaceDisplacementBounds(faces, bounds, 1);
  test_assert(bool(boundedLeaf->flag & Spatial_RegenBounds));
  test_assert(bool(tree.getRoot()->flag & Spatial_RegenBounds));

  tree.regenDirtyBounds();

  // The owning leaf's box contains every face vert offset by ±pad.
  {
    FaceProxy face(m, boundedFace);
    for (auto list : face.lists()) {
      for (auto c : list) {
        float3 co = m->v.co[c.v()];
        for (int axis = 0; axis < 3; axis++) {
          test_assert(boundedLeaf->aabb.min[axis] <= co[axis] - pad + 1e-6f);
          test_assert(boundedLeaf->aabb.max[axis] >= co[axis] + pad - 1e-6f);
        }
      }
    }
  }
  // Root grew to contain the padded leaf (z spans ±pad now).
  test_assert(tree.getRoot()->aabb.min[2] <= rootMin0[2] - pad + 1e-6f);
  test_assert(tree.getRoot()->aabb.max[2] >= rootMax0[2] + pad - 1e-6f);

  // A far-away leaf with no bounded faces is untouched.
  {
    float3 dmin = controlLeaf->aabb.min - controlMin0;
    float3 dmax = controlLeaf->aabb.max - controlMax0;
    for (int axis = 0; axis < 3; axis++) {
      test_assert(std::fabs(dmin[axis]) < 1e-6f);
      test_assert(std::fabs(dmax[axis]) < 1e-6f);
    }
  }

  // --- castRay conservativeness: every pre-pad hit still hits (narrow phase
  // unchanged), and rays clear of the mesh still miss despite padded boxes. ---
  int hits = 0;
  for (int i = 0; i <= 20; i++) {
    for (int j = 0; j <= 20; j++) {
      float x = -0.49f + 0.98f * float(i) / 20.0f;
      float y = -0.49f + 0.98f * float(j) / 20.0f;
      CastRayIsect out;
      bool hit = castDown(tree, x, y, out);
      test_assert(hit);
      if (hit) {
        test_assert(std::fabs(out.t - 1.0f) < 1e-4f);
        hits++;
      }
    }
  }
  fprintf(stderr, "displacement bounds: %d/441 rays hit\n", hits);
  {
    CastRayIsect out;
    test_assert(!castDown(tree, 2.0f, 2.0f, out));
    test_assert(!castDown(tree, -0.75f, 0.0f, out));
  }

  // --- zeroing the bound and regenerating shrinks the pad back ---
  float zero[1] = {0.0f};
  tree.setFaceDisplacementBounds(faces, zero, 1);
  tree.regenDirtyBounds();
  test_assert(std::fabs(tree.getRoot()->aabb.min[2] - rootMin0[2]) < 1e-5f);
  test_assert(std::fabs(tree.getRoot()->aabb.max[2] - rootMax0[2]) < 1e-5f);

  /* Skip test_end(): the `.spatial.*`/`.detail.*` attr name strings stay live
   * in the alloc tracker (mirrors test_spatial_raycast.cc). */
  alloc::Delete(m);
  return retval;
}
