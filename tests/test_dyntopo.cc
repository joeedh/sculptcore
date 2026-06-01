#include "test_util.h"

#include "dyntopo/dyntopo.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace sculptcore::dyntopo;
using namespace litestl;
using namespace litestl::math;
using namespace litestl::util;

/* --- Mesh integrity (mirrors test_edge_collapse.cc::validateMesh) --- */
static bool validateMesh(Mesh &m, const char *tag)
{
  for (int ei : m.e) {
    int v1 = m.e.vs[ei][0], v2 = m.e.vs[ei][1];
    if (v1 < 0 || v2 < 0 || v1 >= int(m.v.capacity()) || v2 >= int(m.v.capacity())) {
      fprintf(stderr, "[%s] edge %d bad vert refs %d %d\n", tag, ei, v1, v2);
      return false;
    }
    if (m.v.freemap[v1] || m.v.freemap[v2]) {
      fprintf(stderr, "[%s] edge %d references freed vert\n", tag, ei);
      return false;
    }
    if (v1 == v2) {
      fprintf(stderr, "[%s] edge %d self-loop\n", tag, ei);
      return false;
    }
  }
  for (int vi : m.v) {
    int e0 = m.v.e[vi];
    if (e0 == ELEM_NONE) continue;
    int steps = 0, ec = e0;
    do {
      int side = m.e.vs[ec][0] == vi ? 0 : 1;
      if (m.e.vs[ec][side] != vi) {
        fprintf(stderr, "[%s] vert %d disk edge %d not incident\n", tag, vi, ec);
        return false;
      }
      int next = m.e.disk[ec][side * 2 + 1];
      int prev = m.e.disk[ec][side * 2];
      int side_n = m.e.vs[next][0] == vi ? 0 : 1;
      int side_p = m.e.vs[prev][0] == vi ? 0 : 1;
      if (m.e.disk[next][side_n * 2] != ec || m.e.disk[prev][side_p * 2 + 1] != ec) {
        fprintf(stderr, "[%s] disk prev/next mismatch v=%d e=%d\n", tag, vi, ec);
        return false;
      }
      ec = next;
      if (++steps > 1000000) {
        fprintf(stderr, "[%s] vert %d disk did not close\n", tag, vi);
        return false;
      }
    } while (ec != e0);
  }
  for (int ei : m.e) {
    int c0 = m.e.c[ei];
    if (c0 == ELEM_NONE) continue;
    int steps = 0, cc = c0;
    do {
      if (m.c.e[cc] != ei) {
        fprintf(stderr, "[%s] edge %d radial corner %d c.e mismatch\n", tag, ei, cc);
        return false;
      }
      int rn = m.c.radial_next[cc], rp = m.c.radial_prev[cc];
      if (m.c.radial_prev[rn] != cc || m.c.radial_next[rp] != cc) {
        fprintf(stderr, "[%s] radial prev/next mismatch e=%d c=%d\n", tag, ei, cc);
        return false;
      }
      cc = rn;
      if (++steps > 1000000) {
        fprintf(stderr, "[%s] edge %d radial did not close\n", tag, ei);
        return false;
      }
    } while (cc != c0);
  }
  for (int fi : m.f) {
    int li = m.f.l[fi];
    while (li != ELEM_NONE) {
      if (m.l.f[li] != fi) {
        fprintf(stderr, "[%s] list %d not owned by face %d\n", tag, li, fi);
        return false;
      }
      int c0 = m.l.c[li], cc = c0, n = 0;
      do {
        if (m.c.l[cc] != li) {
          fprintf(stderr, "[%s] corner %d c.l != list %d\n", tag, cc, li);
          return false;
        }
        int cn = m.c.next[cc];
        if (m.c.prev[cn] != cc) {
          fprintf(stderr, "[%s] corner prev/next mismatch f=%d c=%d\n", tag, fi, cc);
          return false;
        }
        int ce = m.c.e[cc], v_here = m.c.v[cc], v_next = m.c.v[cn];
        int ev0 = m.e.vs[ce][0], ev1 = m.e.vs[ce][1];
        if (!((ev0 == v_here && ev1 == v_next) || (ev1 == v_here && ev0 == v_next))) {
          fprintf(stderr, "[%s] corner edge-vert mismatch f=%d c=%d\n", tag, fi, cc);
          return false;
        }
        cc = cn;
        if (++n > 1000000) {
          fprintf(stderr, "[%s] list %d did not close\n", tag, li);
          return false;
        }
      } while (cc != c0);
      if (n != m.l.size[li]) {
        fprintf(stderr, "[%s] list %d size %d != walked %d\n", tag, li, m.l.size[li], n);
        return false;
      }
      li = m.l.next[li];
    }
  }
  /* Triangles only: dyntopo must keep every face a triangle. */
  for (int fi : m.f) {
    if (m.f.list_count[fi] != 1 || m.l.size[m.f.l[fi]] != 3) {
      fprintf(stderr, "[%s] face %d is not a triangle\n", tag, fi);
      return false;
    }
  }
  return true;
}

/* Triangulated NxN grid in z=0, spanning [-0.5, 0.5]^2. One boundary loop. */
static Mesh *makeTriGrid(int n)
{
  Mesh *m = alloc::New<Mesh>("test_dyntopo grid");
  Vector<int> grid;
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

static float dist(float3 a, float3 b)
{
  return (a - b).length();
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  const float3 center(0, 0, 0);
  const float radius = 0.30f;

  /* --- Subdivide: a coarse grid refines under the dab, manifold throughout,
   *     refinement stays local, and the outside is untouched. --- */
  {
    Mesh *m = makeTriGrid(9); /* spacing 0.125 */
    test_assert(validateMesh(*m, "sub-pre"));

    int V0 = m->v.count, F0 = m->f.count;

    /* Snapshot original verts outside the region. */
    Vector<float3> origCo;
    origCo.resize(m->v.capacity());
    Vector<int> wasOutside;
    wasOutside.resize(m->v.capacity());
    for (int vi : m->v) {
      origCo[vi] = m->v.co[vi];
      wasOutside[vi] = dist(m->v.co[vi], center) > radius ? 1 : 0;
    }

    DynTopoParams p;
    p.l_max = 0.08f;
    p.l_min = 0.01f; /* below any starting length: collapse never triggers */
    p.mode = DynTopoMode::Subdivide;
    DynTopoStats st = applyBrushDab(*m, center, radius, p, /*seed=*/1234u);

    test_assert(validateMesh(*m, "sub-post"));
    test_assert(!st.capped); /* default budget converges this dab */
    test_assert(st.splits > 0);
    test_assert(st.collapses == 0);
    test_assert(m->v.count > V0 && m->f.count > F0);

    /* New verts (index >= V0 region of the storage) all lie inside the dab;
     * original outside verts are unmoved. */
    for (int vi : m->v) {
      if (vi < int(origCo.size()) && wasOutside.size() > vi && wasOutside[vi] &&
          !m->v.freemap[vi]) {
        /* still the same original vert (subdivide never kills verts) */
        test_assert(dist(m->v.co[vi], origCo[vi]) < 1e-6f);
      }
    }
    for (int vi : m->v) {
      bool isNew = vi >= V0; /* freelist was empty, new verts append */
      if (isNew) {
        test_assert(dist(m->v.co[vi], center) <= radius + 1e-4f);
      }
    }

    /* Converged (not capped): every edge fully inside the region is within
     * the target length. */
    if (!st.capped) {
      for (int e : m->e) {
        float3 a = m->v.co[m->e.vs[e][0]], b = m->v.co[m->e.vs[e][1]];
        if (dist(a, center) < radius && dist(b, center) < radius) {
          test_assert(dist(a, b) <= p.l_max * 1.001f);
        }
      }
    }

    printf("subdivide: V %d->%d, F %d->%d, %d splits, %d rounds%s\n", V0,
           m->v.count, F0, m->f.count, st.splits, st.rounds,
           st.capped ? " (capped)" : "");
    alloc::Delete<Mesh>(m);
  }

  /* --- Collapse: a fine grid coarsens under the dab, staying manifold. --- */
  {
    Mesh *m = makeTriGrid(17); /* spacing 0.0625 */
    test_assert(validateMesh(*m, "col-pre"));
    int V0 = m->v.count, F0 = m->f.count;

    DynTopoParams p;
    p.l_max = 1.0f; /* never split */
    p.l_min = 0.12f;
    p.mode = DynTopoMode::Collapse;
    DynTopoStats st = applyBrushDab(*m, center, radius, p, /*seed=*/77u);

    test_assert(validateMesh(*m, "col-post"));
    test_assert(st.collapses > 0);
    test_assert(st.splits == 0);
    test_assert(m->v.count < V0 && m->f.count < F0);

    printf("collapse: V %d->%d, F %d->%d, %d collapses, %d rounds%s\n", V0,
           m->v.count, F0, m->f.count, st.collapses, st.rounds,
           st.capped ? " (capped)" : "");
    alloc::Delete<Mesh>(m);
  }

  /* --- Determinism: same seed -> identical result counts. --- */
  {
    DynTopoParams p;
    p.l_max = 0.08f;
    p.l_min = 0.01f;
    p.mode = DynTopoMode::Subdivide;

    Mesh *a = makeTriGrid(9);
    Mesh *b = makeTriGrid(9);
    DynTopoStats sa = applyBrushDab(*a, center, radius, p, 999u);
    DynTopoStats sb = applyBrushDab(*b, center, radius, p, 999u);
    test_assert(sa.splits == sb.splits);
    test_assert(sa.rounds == sb.rounds);
    test_assert(a->v.count == b->v.count);
    test_assert(a->e.count == b->e.count);
    test_assert(a->f.count == b->f.count);
    alloc::Delete<Mesh>(a);
    alloc::Delete<Mesh>(b);
  }

  /* --- Idempotence: re-running on an already-in-band region does nothing. */
  {
    DynTopoParams p;
    p.l_max = 0.08f;
    p.l_min = 0.01f;
    p.mode = DynTopoMode::Subdivide;

    Mesh *m = makeTriGrid(9);
    applyBrushDab(*m, center, radius, p, 5u);
    int V1 = m->v.count;
    DynTopoStats st2 = applyBrushDab(*m, center, radius, p, 5u);
    test_assert(st2.splits == 0);
    test_assert(m->v.count == V1);
    alloc::Delete<Mesh>(m);
  }

  printf("dyntopo test: ok\n");
  return test_end();
}
