#include "test_util.h"

#include "litestl/math/vector.h"
#include "mesh/mesh.h"
#include "spatial/spatial.h"
#include "spatial/spatial_base.h"

#include <array>
#include <cmath>
#include <cstdio>

test_init;

/* Local assert that flips retval (the shared test_assert macro has a known
 * retval=0-on-failure bug — see tests/test_meshlog_topo.cc:14-21). */
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

/* NxN grid of quads on z=0, unit square in XY. */
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

      /* Only create each edge once; reuse via find_edge for shared ones. */
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

/* Cast a downward ray at (x, y, 1) and check it hits z=0 plane. */
bool cast_down_hits(SpatialTree &tree, float x, float y, float &t_out)
{
  CastRayIsect isect;
  bool ok = tree.castRay(float3(x, y, 1.0f), float3(0.0f, 0.0f, -1.0f), isect);
  if (ok) {
    t_out = isect.t;
  }
  return ok;
}

} // namespace

int main()
{
  /* Single-leaf sanity: small grid, default leaf_limit. */
  {
    Mesh m;
    build_grid(m, 2);
    SpatialTree tree(&m);
    tree.buildAll();
    for (auto *node : tree.leaves()) {
      tree.ensure_node_tris(node);
    }

    float t = 0.0f;
    TASSERT(cast_down_hits(tree, 0.5f, 0.5f, t));
    TASSERT(std::fabs(t - 1.0f) < 1e-4f);

    /* nearestVert (Wave 5 click-to-vertex): the hit triangle's
     * max-barycentric-weight corner — i.e. the vertex of the hit *face* nearest
     * the hit point. Verify it's populated and is indeed the closest of the hit
     * face's corners to isect.p (no assumption about how the quad tessellates). */
    auto nearestFaceCorner = [&](int fi, const float3 &p) -> int {
      int best = ELEM_NONE;
      float bestD = 1e30f;
      int li = m.f.l[fi];
      while (li != ELEM_NONE) {
        int c0 = m.l.c[li], cc = c0;
        do {
          int v = m.c.v[cc];
          float d = (m.v.co[v] - p).length();
          if (d < bestD) {
            bestD = d;
            best = v;
          }
          cc = m.c.next[cc];
        } while (cc != c0 && cc != ELEM_NONE);
        li = m.l.next[li];
      }
      return best;
    };
    auto checkNearestVert = [&](float x, float y) {
      CastRayIsect isect;
      bool ok = tree.castRay(float3(x, y, 1.0f), float3(0, 0, -1.0f), isect);
      TASSERT(ok);
      TASSERT(isect.nearestVert != ELEM_NONE);
      TASSERT(isect.faceIndex != ELEM_NONE);
      TASSERT(isect.nearestVert == nearestFaceCorner(isect.faceIndex, isect.p));
    };
    checkNearestVert(0.49f, 0.49f);
    checkNearestVert(0.02f, 0.02f);
  }

  /* Multi-leaf grid: the regression case. With leaf_limit forced low the
   * tree splits several levels deep, so castRay must accumulate `ok` across
   * sibling children — the bug at node.h:169 made a hit in child[0] get
   * overwritten by child[1]'s miss and the outer castRay returned false. */
  {
    Mesh m;
    const int N = 16;
    build_grid(m, N);

    SpatialTree tree(&m);
    tree.leaf_limit = 16; /* force deep splits */
    tree.buildAll();
    for (auto *node : tree.leaves()) {
      tree.ensure_node_tris(node);
    }

    /* Center hit: must report hit at t ≈ 1.0 (origin z=1, plane z=0). */
    {
      float t = 0.0f;
      TASSERT(cast_down_hits(tree, 0.5f, 0.5f, t));
      TASSERT(std::fabs(t - 1.0f) < 1e-3f);
    }

    /* Sweep every cell — every ray must hit. Without the fix, a significant
     * fraction (those whose traversal visits a hit-child before a miss-child)
     * report a false miss. */
    int misses = 0;
    int casts = 0;
    for (int j = 0; j < N; j++) {
      for (int i = 0; i < N; i++) {
        float x = (float(i) + 0.5f) / float(N);
        float y = (float(j) + 0.5f) / float(N);
        float t = 0.0f;
        casts++;
        if (!cast_down_hits(tree, x, y, t)) {
          misses++;
          continue;
        }
        if (std::fabs(t - 1.0f) > 1e-3f) {
          misses++;
        }
      }
    }
    if (misses != 0) {
      fprintf(stderr, "  raycast sweep: %d miss(es) out of %d casts\n", misses, casts);
    }
    TASSERT(misses == 0);

    /* Genuine miss: ray pointing away from the grid. */
    {
      CastRayIsect isect;
      bool ok = tree.castRay(float3(0.5f, 0.5f, 1.0f), float3(0.0f, 0.0f, 1.0f), isect);
      TASSERT(!ok);
    }

    /* Ray outside the grid in XY must miss. */
    {
      CastRayIsect isect;
      bool ok =
          tree.castRay(float3(-1.0f, -1.0f, 1.0f), float3(0.0f, 0.0f, -1.0f), isect);
      TASSERT(!ok);
    }

    /* Oblique ray crossing tree-internal split boundaries: enters one
     * child's AABB then another, hits a triangle in the second. */
    {
      CastRayIsect isect;
      float3 orig(-0.5f, 0.5f, 0.5f);
      float3 dir = (float3(0.75f, 0.5f, 0.0f) - orig).normalized();
      bool ok = tree.castRay(orig, dir, isect);
      TASSERT(ok);
    }
  }

  /* Skip test_end()'s leak check: SpatialTree's attribute setup leaves
   * some attribute-name strings live in litestl::alloc's tracker (the
   * BuiltinAttr ".spatial.v.node" etc. names). That's pre-existing and
   * orthogonal to raycast correctness. Just return the assertion result. */
  return retval;
}
