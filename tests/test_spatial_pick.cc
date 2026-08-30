#include "test_util.h"

#include "litestl/math/vector.h"
#include "mesh/mesh.h"
#include "spatial/spatial.h"
#include "spatial/spatial_base.h"

#include <cmath>
#include <cstdio>

test_init;

/* Local assert that flips retval (the shared test_assert macro has a known
 * retval=0-on-failure bug — see test_spatial_raycast.cc). */
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
using namespace litestl;

namespace {

/* NxN grid of quads on z=0, unit square in XY. */
void build_grid(Mesh &m, int N)
{
  int *v = new int[(N + 1) * (N + 1)];
  auto vat = [&](int i, int j) -> int & { return v[j * (N + 1) + i]; };

  for (int j = 0; j <= N; j++) {
    for (int i = 0; i <= N; i++) {
      vat(i, j) = m.make_vertex(float3(float(i) / float(N), float(j) / float(N), 0.0f));
    }
  }
  for (int j = 0; j < N; j++) {
    for (int i = 0; i < N; i++) {
      int v0 = vat(i, j), v1 = vat(i + 1, j), v2 = vat(i + 1, j + 1), v3 = vat(i, j + 1);

      if (m.find_edge(v0, v1) == ELEM_NONE)
        m.make_edge(v0, v1);
      if (m.find_edge(v1, v2) == ELEM_NONE)
        m.make_edge(v1, v2);
      if (m.find_edge(v2, v3) == ELEM_NONE)
        m.make_edge(v2, v3);
      if (m.find_edge(v3, v0) == ELEM_NONE)
        m.make_edge(v3, v0);

      int verts[4] = {v0, v1, v2, v3};
      m.make_face(std::span<int>(verts, 4));
    }
  }
  delete[] v;
}

void build_tree(Mesh &m, SpatialTree &tree)
{
  tree.leaf_limit = 16; /* force deep splits so traversal is exercised */
  tree.buildAll();
  for (auto *node : tree.leaves()) {
    tree.ensure_node_tris(node);
  }
}

/* Cast a box frustum over the XY rect [x0,y0]-[x1,y1], z from +1 to -1. */
bool castRect(SpatialTree &tree,
              float x0,
              float y0,
              float x1,
              float y1,
              util::Vector<int> &faces,
              util::Vector<int> &verts)
{
  return tree.castScreenRect(float3(x0, y0, 1.0f),
                             float3(x1, y0, 1.0f),
                             float3(x1, y1, 1.0f),
                             float3(x0, y1, 1.0f),
                             float3(x0, y0, -1.0f),
                             float3(x1, y0, -1.0f),
                             float3(x1, y1, -1.0f),
                             float3(x0, y1, -1.0f),
                             faces,
                             verts);
}

} // namespace

int main()
{
  const int N = 8;
  const int TOTAL_FACES = N * N;

  /* ---- castScreenCircle (cone) ---- */
  {
    Mesh m;
    build_grid(m, N);
    SpatialTree tree(&m);
    build_tree(m, tree);

    /* Narrow cone straight down through the grid center. */
    {
      util::Vector<int> faces, verts;
      bool hit = tree.castScreenCircle(
          float3(0.5f, 0.5f, 1.0f), float3(0.0f, 0.0f, -2.0f), 0.1f, 0.1f, faces, verts);
      TASSERT(hit);
      TASSERT(faces.size() > 0);
      TASSERT(verts.size() > 0);
      /* Narrow radius: must not grab the whole grid. */
      TASSERT(faces.size() < TOTAL_FACES);
    }

    /* Cone pointing away from the grid (upward): nothing hit. */
    {
      util::Vector<int> faces, verts;
      bool hit = tree.castScreenCircle(
          float3(0.5f, 0.5f, 1.0f), float3(0.0f, 0.0f, 2.0f), 0.1f, 0.1f, faces, verts);
      TASSERT(!hit);
      TASSERT(faces.size() == 0);
      TASSERT(verts.size() == 0);
    }

    /* Wide cone enclosing everything. */
    {
      util::Vector<int> faces, verts;
      tree.castScreenCircle(
          float3(0.5f, 0.5f, 1.0f), float3(0.0f, 0.0f, -2.0f), 5.0f, 5.0f, faces, verts);
      TASSERT(faces.size() == TOTAL_FACES);
    }
  }

  /* ---- castScreenRect (frustum) ---- */
  {
    Mesh m;
    build_grid(m, N);
    SpatialTree tree(&m);
    build_tree(m, tree);

    /* Sub-rect over the lower-left quadrant. */
    {
      util::Vector<int> faces, verts;
      bool hit = castRect(tree, 0.0f, 0.0f, 0.5f, 0.5f, faces, verts);
      TASSERT(hit);
      TASSERT(faces.size() > 0);
      TASSERT(faces.size() < TOTAL_FACES);
      TASSERT(verts.size() > 0);
    }

    /* Whole-grid rect selects every face (would fail if planes were
     * mis-oriented — inward orientation is exercised here). */
    {
      util::Vector<int> faces, verts;
      castRect(tree, -0.1f, -0.1f, 1.1f, 1.1f, faces, verts);
      TASSERT(faces.size() == TOTAL_FACES);
    }

    /* Rect entirely outside the grid: nothing. */
    {
      util::Vector<int> faces, verts;
      bool hit = castRect(tree, 5.0f, 5.0f, 6.0f, 6.0f, faces, verts);
      TASSERT(!hit);
      TASSERT(faces.size() == 0);
    }

    /* Degenerate (zero-area) rect: no crash. */
    {
      util::Vector<int> faces, verts;
      castRect(tree, 0.5f, 0.5f, 0.5f, 0.5f, faces, verts);
      TASSERT(faces.size() >= 0); /* just must not crash */
    }
  }

  /* Skip test_end()'s leak check: SpatialTree's attribute setup leaves builtin
   * attribute-name strings live in the tracker (pre-existing, see
   * test_spatial_raycast.cc). */
  return retval;
}
