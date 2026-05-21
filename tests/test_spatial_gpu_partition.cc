#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "spatial/node.h"
#include "spatial/spatial.h"
#include "spatial/spatial_base.h"

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
using namespace sculptcore::spatial;
using litestl::math::float3;

namespace {

void build_grid(Mesh &m, int N)
{
  int *v = new int[(N + 1) * (N + 1)];
  auto vat = [&](int i, int j) -> int & { return v[j * (N + 1) + i]; };

  for (int j = 0; j <= N; j++) {
    for (int i = 0; i <= N; i++) {
      float x = float(i) / float(N);
      float y = float(j) / float(N);
      vat(i, j) = m.make_vertex(float3(x, y, 0.0f));
    }
  }
  for (int j = 0; j < N; j++) {
    for (int i = 0; i < N; i++) {
      int v0 = vat(i, j);
      int v1 = vat(i + 1, j);
      int v2 = vat(i + 1, j + 1);
      int v3 = vat(i, j + 1);

      if (m.find_edge(v0, v1) == ELEM_NONE) m.make_edge(v0, v1);
      if (m.find_edge(v1, v2) == ELEM_NONE) m.make_edge(v1, v2);
      if (m.find_edge(v2, v3) == ELEM_NONE) m.make_edge(v2, v3);
      if (m.find_edge(v3, v0) == ELEM_NONE) m.make_edge(v3, v0);

      int verts[4] = {v0, v1, v2, v3};
      m.make_face(std::span<int>(verts, 4));
    }
  }
  delete[] v;
}

void check_partition(SpatialTree &tree, int total_expected_tris)
{
  /* Drive partition without a GPUManager. */
  for (SpatialNode *node : tree.leaves()) {
    tree.ensure_node_tris(node);
  }
  tree.recompute_subtree_tri_counts();
  tree.assign_gpu_nodes();

  auto gpus = tree.gpu_nodes();
  auto lvs = tree.leaves();

  /* 1. Sum of GPU node tri counts equals mesh tri total. */
  int sum = 0;
  for (SpatialNode *g : gpus) {
    sum += g->subtree_tri_count;
  }
  TASSERT(sum == total_expected_tris);

  /* 2. No GPU node is an ancestor of another. */
  for (SpatialNode *g : gpus) {
    for (SpatialNode *p = g->parent; p; p = p->parent) {
      TASSERT(!p->is_gpu_node);
    }
  }

  /* 3. Every leaf has exactly one GPU node ancestor. */
  for (SpatialNode *leaf : lvs) {
    int ancestors_with_gpu = 0;
    for (SpatialNode *n = leaf; n; n = n->parent) {
      if (n->is_gpu_node) {
        ancestors_with_gpu++;
      }
    }
    TASSERT(ancestors_with_gpu == 1);
  }

  /* 4. find_gpu_owner returns the unique ancestor and matches a node in
   *    the gpu_nodes() list. */
  litestl::util::Set<SpatialNode *> gpu_set;
  for (SpatialNode *g : gpus) {
    gpu_set.add(g);
  }
  for (SpatialNode *leaf : lvs) {
    SpatialNode *owner = tree.find_gpu_owner(leaf);
    TASSERT(owner != nullptr);
    TASSERT(gpu_set.contains(owner));
  }
}

int count_face_tris(Mesh &m)
{
  /* Same triangulation rule as SpatialTree::regen_node_tris: tri=1,
   * quad=2, n-gon: not handled by current logic so we only build grids. */
  int total = 0;
  for (int f = 0; f < m.f.count; f++) {
    int l = m.f.l[f];
    int sz = m.l.size[l];
    total += (sz >= 3) ? (sz - 2) : 0;
    if (sz > 4) {
      /* Current code only handles tri/quad — assert we never test n-gons. */
      total = -1;
      break;
    }
  }
  return total;
}

} // namespace

int main()
{
  /* Small grid: one leaf, one GPU node. */
  {
    Mesh m;
    build_grid(m, 4);
    SpatialTree tree(&m);
    tree.buildAll();
    int expected = count_face_tris(m); /* 16 quads -> 32 tris */
    TASSERT(expected == 32);
    check_partition(tree, expected);
    TASSERT(tree.gpu_nodes().size() == 1);
  }

  /* Larger grid with low leaf_limit so the tree actually splits; sweep
   * gpu_tri_target. Use a fresh Mesh per iteration because SpatialTree
   * writes per-face ownership into mesh attributes that don't get reset
   * when the tree is destroyed. */
  {
    const int N = 32;
    int expected_static = 0;
    for (int target : {64, 256, 2048, 100000}) {
      Mesh m;
      build_grid(m, N);
      int expected = count_face_tris(m);
      if (!expected_static) {
        expected_static = expected;
        TASSERT(expected == N * N * 2);
      }
      SpatialTree tree(&m);
      tree.leaf_limit = 16;
      tree.gpu_tri_target = target;
      tree.buildAll();
      check_partition(tree, expected);

      auto gpus = tree.gpu_nodes();
      if (target >= expected) {
        /* Entire tree should fit under root. */
        TASSERT(gpus.size() == 1);
      }
      /* Every GPU node either fits the target, or is a leaf whose own
       * tri count exceeds the target (cannot subdivide further). */
      for (SpatialNode *g : gpus) {
        bool is_leaf = bool(g->flag & Spatial_Leaf);
        TASSERT(g->subtree_tri_count <= target || is_leaf);
      }
    }
  }

  return retval;
}
