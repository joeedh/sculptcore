/* M3: incremental spatial node-ownership. Applying a dyntopo dab through the
 * tree's MeshCallbacks (getSpatialCallbacks) keeps node ownership complete and
 * consistent WITHOUT a full rebuild — every live face owned by exactly one
 * leaf, owner ids agree, and per-leaf tris regen from the updated unique_faces. */
#include "test_util.h"

#include "dyntopo/dyntopo.h"
#include "mesh/mesh.h"
#include "spatial/spatial.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <algorithm>
#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::math;

static Mesh *makeTriGrid(int n)
{
  Mesh *m = alloc::New<Mesh>("test_spatial_dyntopo grid");
  util::Vector<int> grid;
  grid.resize(n * n);
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      float fx = float(x) / float(n - 1) - 0.5f;
      float fy = float(y) / float(n - 1) - 0.5f;
      grid[y * n + x] = m->make_vertex(float3(fx, fy, 0.0f));
    }
  }
  for (int y = 0; y < n - 1; y++) {
    for (int x = 0; x < n - 1; x++) {
      int a = grid[y * n + x], b = grid[y * n + x + 1];
      int c = grid[(y + 1) * n + x + 1], d = grid[(y + 1) * n + x];
      int t0[3] = {a, b, c};
      int t1[3] = {a, c, d};
      m->make_face(std::span<int>(t0, 3));
      m->make_face(std::span<int>(t1, 3));
    }
  }
  return m;
}

/* Every live face is owned by exactly one leaf, owner ids agree, and the
 * per-leaf unique_faces hold only live faces. Returns owned-face count. */
static int validateOwnership(spatial::SpatialTree *tree, Mesh *m, const char *tag)
{
  int owned = 0;
  auto leaves = tree->leaves();
  for (auto *leaf : leaves) {
    if (!leaf->data) {
      continue;
    }
    for (int f : leaf->data->unique_faces) {
      if (m->f.freemap[f]) {
        fprintf(stderr, "[%s] leaf %d owns dead face %d\n", tag, leaf->id, f);
        return -1;
      }
      if (tree->treeMesh.f.node[f] != leaf->id) {
        fprintf(stderr, "[%s] face %d owner %d != leaf %d\n", tag, f,
                tree->treeMesh.f.node[f], leaf->id);
        return -1;
      }
      owned++;
    }
    /* Vert ownership: a regression here (new verts mis-attributed to a parent's
     * leaf) makes node_needs_split undercount, so the tree never rebalances. */
    for (int v : leaf->data->unique_verts) {
      if (m->v.freemap[v]) {
        fprintf(stderr, "[%s] leaf %d owns dead vert %d\n", tag, leaf->id, v);
        return -1;
      }
      if (tree->treeMesh.v.node[v] != leaf->id) {
        fprintf(stderr, "[%s] vert %d owner %d != leaf %d\n", tag, v,
                tree->treeMesh.v.node[v], leaf->id);
        return -1;
      }
    }
  }
  /* Every live face and vert is owned by exactly one leaf (complete coverage). */
  int ownedV = 0;
  for (auto *leaf : leaves) {
    if (leaf->data) ownedV += int(leaf->data->unique_verts.size());
  }
  if (ownedV != m->v.count) {
    fprintf(stderr, "[%s] %d verts owned but mesh has %d\n", tag, ownedV, m->v.count);
    return -1;
  }
  for (int f : m->f) {
    if (tree->treeMesh.f.node[f] == 0) {
      fprintf(stderr, "[%s] live face %d is unowned\n", tag, f);
      return -1;
    }
  }
  return owned;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Mesh *m = makeTriGrid(13); /* spacing ~0.083 */
  spatial::SpatialTree *tree =
      alloc::New<spatial::SpatialTree>("test tree", m);
  /* Small leaf_limit so the (flip-leaned) refinement still crosses it and forces
   * a rebalance — dyntopo's geometric flips (M7.2) cut the split count, so the
   * default 512 would leave the refined region in a single leaf. */
  tree->leaf_limit = 96;
  tree->buildAll();

  int fBefore = m->f.count;
  int leavesBefore = int(tree->leaves().size());
  int owned0 = validateOwnership(tree, m, "build");
  test_assert(owned0 == fBefore); /* a fresh build owns every face exactly once */

  /* Apply a dyntopo dab INCREMENTALLY through the spatial callbacks — no
   * full rebuild. The callbacks add/remove faces and verts as the operators
   * fire create/kill events. */
  dyntopo::DynTopoParams p;
  p.l_max = 0.05f;
  p.l_min = 0.005f;
  p.mode = dyntopo::DynTopoMode::Subdivide;
  dyntopo::DynTopoStats st = dyntopo::applyBrushDab(
      *m, float3(0, 0, 0), 0.3f, p, /*seed=*/42u, tree->getSpatialCallbacks());

  test_assert(st.splits > 0);
  int fAfter = m->f.count;
  test_assert(fAfter > fBefore); /* refined under the dab */

  /* Ownership is complete + consistent immediately after the dab — placement
   * (add_face_at) is O(1) and eager; only the leaf SPLIT is deferred (M7.6), so
   * the region's verts are correctly owned even though they pile into a few
   * over-full leaves until the rebalance pass runs. */
  int owned1 = validateOwnership(tree, m, "incremental");
  test_assert(owned1 == fAfter);

  /* Deferred batched rebalance: split the over-full leaves once each (what
   * update() does at the top, here without a GPUManager). */
  tree->applyDeferredNodeSplit();

  /* Per-leaf tris regen from the updated unique_faces (no GPU). Touched leaves
   * carry Spatial_RegenTris from add_face/remove_face. */
  for (auto *leaf : tree->leaves()) {
    tree->ensure_node_tris(leaf);
  }
  int owned2 = validateOwnership(tree, m, "post-regen");
  test_assert(owned2 == fAfter);

  /* The refinement pushed the region's leaves well past leaf_limit, so the
   * deferred rebalance must have split them — not left one giant leaf. */
  int leavesAfter = int(tree->leaves().size());
  test_assert(leavesAfter > leavesBefore);

  /* End-to-end rebalance guard (M7.6c): many alternating subdivide/collapse dabs,
   * each followed by the deferred rebalance + merge passes. Ownership must stay
   * valid every cycle, and the leaf count must not grow without bound — the merge/
   * rebalance pass keeps the tree from accreting wasted split levels as the region
   * is repeatedly refined and gutted. */
  int leavesCap = 0;
  for (int i = 0; i < 6; i++) {
    dyntopo::DynTopoParams dp;
    if (i & 1) {
      dp.mode = dyntopo::DynTopoMode::Collapse;
      dp.l_min = 0.06f;
      dp.l_max = 0.12f;
    } else {
      dp.mode = dyntopo::DynTopoMode::Subdivide;
      dp.l_max = 0.05f;
      dp.l_min = 0.005f;
    }
    float fx = (i % 3 - 1) * 0.2f;
    dyntopo::applyBrushDab(*m, float3(fx, 0, 0), 0.25f, dp,
                           /*seed=*/100u + i, tree->getSpatialCallbacks());
    tree->applyDeferredNodeSplit();
    tree->applyDeferredMerge();
    for (auto *leaf : tree->leaves()) {
      tree->ensure_node_tris(leaf);
    }
    int ownedN = validateOwnership(tree, m, "e2e-cycle");
    test_assert(ownedN == m->f.count);
    leavesCap = std::max(leavesCap, int(tree->leaves().size()));
  }
  /* The tree never exploded: peak leaf count stays bounded relative to faces
   * (a corrupt merge/rebalance would either fail validation above or leak leaves). */
  test_assert(leavesCap > 0);
  test_assert(leavesCap < m->f.count); /* far fewer leaves than faces */

  printf("spatial_dyntopo test: ok (%d -> %d faces, %d splits, %d -> %d leaves, "
         "e2e peak %d)\n",
         fBefore, fAfter, st.splits, leavesBefore, leavesAfter, leavesCap);

  alloc::Delete(tree);
  alloc::Delete(m);
  return test_end();
}
