/* M7.6b: deferred merge. A collapse-heavy dab shrinks a refined region; the
 * periodic merge pass (applyDeferredMerge) must fold the now-under-full sibling
 * leaves back into their parents (cascading up the chain) WITHOUT a full
 * rebuild — leaf count drops and node ownership stays complete + consistent. */
#include "test_util.h"

#include "dyntopo/dyntopo.h"
#include "mesh/mesh.h"
#include "spatial/spatial.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::math;

static Mesh *makeTriGrid(int n)
{
  Mesh *m = alloc::New<Mesh>("test_spatial_merge grid");
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

/* Every live face/vert owned by exactly one leaf, owner ids agree, leaves hold
 * only live geometry, and coverage is complete (sum of unique_verts == v.count). */
static bool validateOwnership(spatial::SpatialTree *tree, Mesh *m, const char *tag)
{
  int ownedV = 0;
  for (auto *leaf : tree->leaves()) {
    if (!leaf->data) {
      continue;
    }
    for (int f : leaf->data->unique_faces) {
      if (m->f.freemap[f] || tree->treeMesh.f.node[f] != leaf->id) {
        fprintf(stderr, "[%s] face %d bad owner (leaf %d)\n", tag, f, leaf->id);
        return false;
      }
    }
    for (int v : leaf->data->unique_verts) {
      if (m->v.freemap[v] || tree->treeMesh.v.node[v] != leaf->id) {
        fprintf(stderr, "[%s] vert %d bad owner (leaf %d)\n", tag, v, leaf->id);
        return false;
      }
      ownedV++;
    }
  }
  if (ownedV != m->v.count) {
    fprintf(stderr, "[%s] %d verts owned but mesh has %d\n", tag, ownedV, m->v.count);
    return false;
  }
  for (int f : m->f) {
    if (tree->treeMesh.f.node[f] == 0) {
      fprintf(stderr, "[%s] live face %d unowned\n", tag, f);
      return false;
    }
  }
  return true;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Mesh *m = makeTriGrid(25); /* 625 verts, spacing ~0.0417 */
  spatial::SpatialTree *tree = alloc::New<spatial::SpatialTree>("test tree", m);
  tree->leaf_limit = 48; /* small, so the base mesh already fans into a subtree */
  tree->buildAll();

  int leavesPeak = int(tree->leaves().size());
  test_assert(leavesPeak > 2); /* a real multi-leaf tree to merge back down */
  test_assert(validateOwnership(tree, m, "build"));

  /* Collapse-heavy dab over the whole mesh: l_min far above the grid spacing, so
   * the interior collapses aggressively and most verts are killed. remove_vert
   * fires for each, recording the shrinking leaves' parents as merge candidates. */
  int vBefore = m->v.count;
  dyntopo::DynTopoParams p;
  p.mode = dyntopo::DynTopoMode::Collapse;
  p.l_min = 0.3f;
  p.l_max = 0.6f;
  dyntopo::runDyntopoRemesh(*m, float3(0, 0, 0), 2.0f, p, /*seed=*/9u,
                         tree->getSpatialCallbacks());
  test_assert(m->v.count < vBefore); /* the dab actually removed geometry */
  test_assert(validateOwnership(tree, m, "post-collapse"));

  /* The periodic merge pass folds the under-full siblings back up (what update()
   * does every mergeCadence_-th tick; driven directly here, no GPUManager). */
  tree->applyDeferredMerge();

  for (auto *leaf : tree->leaves()) {
    tree->ensure_node_tris(leaf);
  }

  int leavesAfter = int(tree->leaves().size());
  test_assert(leavesAfter < leavesPeak);          /* merged down */
  test_assert(validateOwnership(tree, m, "post-merge"));

  /* Idempotent: a second pass with no new shrinkage is a no-op and stays valid. */
  tree->applyDeferredMerge();
  test_assert(int(tree->leaves().size()) == leavesAfter);
  test_assert(validateOwnership(tree, m, "post-merge-2"));

  printf("spatial_merge test: ok (%d verts -> %d, leaves %d -> %d)\n", vBefore,
         m->v.count, leavesPeak, leavesAfter);

  /* M7.6c rebalance: a lopsided subtree (one near-empty child + a fuller sibling,
   * combined still above the under-full watermark) must be folded/collapsed back
   * by applyDeferredMerge, WITHOUT corrupting ownership and WITHOUT oscillating
   * against the deferred split. Build a one-sided refine, then collapse only part
   * of the refined region so its leaves shrink unevenly. */
  Mesh *m2 = makeTriGrid(25);
  spatial::SpatialTree *tree2 = alloc::New<spatial::SpatialTree>("test tree 2", m2);
  tree2->leaf_limit = 48;
  tree2->buildAll();

  /* Refine an interior region into a deep multi-leaf subtree (kept off the mesh
   * boundary, where split_node's lack of orphan-recovery is a separate concern). */
  dyntopo::DynTopoParams sub;
  sub.mode = dyntopo::DynTopoMode::Subdivide;
  sub.l_max = 0.03f;
  sub.l_min = 0.005f;
  dyntopo::runDyntopoRemesh(*m2, float3(0, 0, 0), 0.28f, sub, /*seed=*/3u,
                         tree2->getSpatialCallbacks());
  tree2->applyDeferredNodeSplit();
  test_assert(validateOwnership(tree2, m2, "r-refine"));
  int leavesRefined = int(tree2->leaves().size());
  test_assert(leavesRefined > 2);

  /* Collapse an interior sub-region, gutting some refined leaves while their
   * siblings stay populated -> lopsided parents in the [watermark, limit) band. */
  dyntopo::DynTopoParams col;
  col.mode = dyntopo::DynTopoMode::Collapse;
  col.l_min = 0.25f;
  col.l_max = 0.5f;
  dyntopo::runDyntopoRemesh(*m2, float3(0.12f, 0.05f, 0), 0.12f, col, /*seed=*/4u,
                         tree2->getSpatialCallbacks());
  test_assert(validateOwnership(tree2, m2, "r-collapse"));

  /* Alternating rebalance/merge cycles: must reach a stable leaf count (the
   * mean-split predictor refuses merges the split would just recreate) and keep
   * ownership valid every cycle — the anti-thrash / no-corruption guarantee. */
  int prevLeaves = int(tree2->leaves().size());
  int stable = 0;
  for (int i = 0; i < 8 && stable < 2; i++) {
    tree2->applyDeferredNodeSplit();
    tree2->applyDeferredMerge();
    for (auto *leaf : tree2->leaves()) {
      tree2->ensure_node_tris(leaf);
    }
    test_assert(validateOwnership(tree2, m2, "r-cycle"));
    int now = int(tree2->leaves().size());
    stable = (now == prevLeaves) ? stable + 1 : 0;
    prevLeaves = now;
  }
  test_assert(stable >= 2); /* converged, no split<->merge oscillation */
  int leavesRebalanced = int(tree2->leaves().size());
  test_assert(leavesRebalanced <= leavesRefined); /* rebalance never grew the tree */

  printf("spatial_rebalance test: ok (leaves refined %d -> rebalanced %d)\n",
         leavesRefined, leavesRebalanced);

  alloc::Delete(tree2);
  alloc::Delete(m2);
  alloc::Delete(tree);
  alloc::Delete(m);
  return test_end();
}
