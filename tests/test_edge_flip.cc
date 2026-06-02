#include "test_util.h"

#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/utils/edge_flip.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/rand.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::math;
using namespace litestl::util;

/* Disk + radial + loop integrity, triangles only (mirrors the other op tests). */
static bool validateMesh(Mesh &m, const char *tag)
{
  for (int ei : m.e) {
    int v1 = m.e.vs[ei][0], v2 = m.e.vs[ei][1];
    if (v1 < 0 || v2 < 0 || m.v.freemap[v1] || m.v.freemap[v2] || v1 == v2) {
      fprintf(stderr, "[%s] edge %d bad verts %d %d\n", tag, ei, v1, v2);
      return false;
    }
  }
  for (int vi : m.v) {
    int e0 = m.v.e[vi];
    if (e0 == ELEM_NONE) continue;
    int steps = 0, ec = e0;
    do {
      int side = m.e.vs[ec][0] == vi ? 0 : 1;
      int next = m.e.disk[ec][side * 2 + 1], prev = m.e.disk[ec][side * 2];
      int sn = m.e.vs[next][0] == vi ? 0 : 1, sp = m.e.vs[prev][0] == vi ? 0 : 1;
      if (m.e.disk[next][sn * 2] != ec || m.e.disk[prev][sp * 2 + 1] != ec) {
        fprintf(stderr, "[%s] disk mismatch v=%d e=%d\n", tag, vi, ec);
        return false;
      }
      ec = next;
      if (++steps > 1000000) return false;
    } while (ec != e0);
  }
  for (int ei : m.e) {
    int c0 = m.e.c[ei];
    if (c0 == ELEM_NONE) continue;
    int steps = 0, cc = c0;
    do {
      if (m.c.e[cc] != ei) { fprintf(stderr, "[%s] radial c.e\n", tag); return false; }
      int rn = m.c.radial_next[cc], rp = m.c.radial_prev[cc];
      if (m.c.radial_prev[rn] != cc || m.c.radial_next[rp] != cc) {
        fprintf(stderr, "[%s] radial mismatch e=%d c=%d\n", tag, ei, cc);
        return false;
      }
      cc = rn;
      if (++steps > 1000000) return false;
    } while (cc != c0);
  }
  for (int fi : m.f) {
    if (m.f.list_count[fi] != 1 || m.l.size[m.f.l[fi]] != 3) {
      fprintf(stderr, "[%s] face %d not a triangle\n", tag, fi);
      return false;
    }
    int li = m.f.l[fi], c0 = m.l.c[li], cc = c0, n = 0;
    do {
      if (m.c.l[cc] != li) return false;
      int cn = m.c.next[cc];
      if (m.c.prev[cn] != cc) { fprintf(stderr, "[%s] loop prev/next\n", tag); return false; }
      int ce = m.c.e[cc], vh = m.c.v[cc], vn = m.c.v[cn];
      int ev0 = m.e.vs[ce][0], ev1 = m.e.vs[ce][1];
      if (!((ev0 == vh && ev1 == vn) || (ev1 == vh && ev0 == vn))) {
        fprintf(stderr, "[%s] corner edge-vert mismatch f=%d c=%d\n", tag, fi, cc);
        return false;
      }
      cc = cn;
      if (++n > 1000000) return false;
    } while (cc != c0);
  }
  return true;
}

static int64_t edgeKey(int a, int b)
{
  int lo = a < b ? a : b, hi = a < b ? b : a;
  return (int64_t(lo) << 32) | uint32_t(hi);
}
static bool noDupEdges(Mesh &m, const char *tag)
{
  Set<int64_t> seen;
  for (int ei : m.e) {
    if (!seen.add(edgeKey(m.e.vs[ei][0], m.e.vs[ei][1]))) {
      fprintf(stderr, "[%s] dup edge %d-%d\n", tag, m.e.vs[ei][0], m.e.vs[ei][1]);
      return false;
    }
  }
  return true;
}

static Mesh *makeTriGrid(int n)
{
  Mesh *m = alloc::New<Mesh>("flip grid");
  Vector<int> g;
  g.resize(n * n);
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++)
      g[y * n + x] = m->make_vertex(float3(float(x), float(y), 0.0f));
  for (int y = 0; y < n - 1; y++)
    for (int x = 0; x < n - 1; x++) {
      int a = g[y * n + x], b = g[y * n + x + 1];
      int c = g[(y + 1) * n + x + 1], d = g[(y + 1) * n + x];
      int t0[3] = {a, b, c}, t1[3] = {a, c, d};
      m->make_face(std::span<int>(t0, 3));
      m->make_face(std::span<int>(t1, 3));
    }
  return m;
}

static int faceCountOfEdge(Mesh &m, int e)
{
  int c0 = m.e.c[e];
  if (c0 == ELEM_NONE) return 0;
  int n = 0, cc = c0;
  do { n++; cc = m.c.radial_next[cc]; } while (cc != c0);
  return n;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  /* --- Randomized flip storm: counts invariant, manifold preserved. --- */
  int flips = 0;
  for (int N : {3, 5, 9, 14}) {
    Mesh *m = makeTriGrid(N);
    test_assert(validateMesh(*m, "pre"));
    Random rnd(uint32_t(N * 71 + 3));
    int V = m->v.count, E = m->e.count, F = m->f.count;

    for (int iter = 0; iter < N * N * 2; iter++) {
      Vector<int> cands;
      for (int e : m->e) {
        if (faceCountOfEdge(*m, e) == 2) cands.append(e);
      }
      if (cands.isEmpty()) break;
      int e = cands[int(rnd.get_int() % uint32_t(cands.size()))];

      EdgeFlipResult res;
      auto ok = flipEdge(*m, e, &res);
      if (!ok) continue; /* refused (e.g. pre-existing c-d edge) — fine */
      flips++;

      if (!validateMesh(*m, "flip")) {
        fprintf(stderr, "integrity fail iter=%d e=%d\n", iter, e);
        return test_end();
      }
      test_assert(noDupEdges(*m, "flip"));
      /* Flip changes only connectivity: counts are invariant. */
      test_assert(m->v.count == V && m->e.count == E && m->f.count == F);
      test_assert(res.created_edge != ELEM_NONE && res.killed_edge == e);
    }
    alloc::Delete<Mesh>(m);
  }

  /* --- Reversibility: flipping the new diagonal restores the original edge. */
  {
    Mesh *m = makeTriGrid(4);
    /* an interior edge */
    int e = ELEM_NONE;
    for (int ei : m->e) {
      if (faceCountOfEdge(*m, ei) == 2) { e = ei; break; }
    }
    test_assert(e != ELEM_NONE);
    int a = m->e.vs[e][0], b = m->e.vs[e][1];

    EdgeFlipResult r1;
    test_assert(bool(flipEdge(*m, e, &r1)));
    test_assert(validateMesh(*m, "rev1"));
    /* a-b is gone, c-d exists */
    test_assert(m->find_edge(a, b) == ELEM_NONE);
    int cd = r1.created_edge;
    test_assert(cd != ELEM_NONE);

    EdgeFlipResult r2;
    test_assert(bool(flipEdge(*m, cd, &r2)));
    test_assert(validateMesh(*m, "rev2"));
    /* flipping the new diagonal brings a-b back */
    test_assert(m->find_edge(a, b) != ELEM_NONE);
    alloc::Delete<Mesh>(m);
  }

  /* --- Boundary edge is refused. --- */
  {
    Mesh *m = makeTriGrid(3);
    int be = ELEM_NONE;
    for (int ei : m->e) {
      if (faceCountOfEdge(*m, ei) == 1) { be = ei; break; }
    }
    test_assert(be != ELEM_NONE);
    EdgeFlipResult res;
    test_assert(!bool(flipEdge(*m, be, &res)));
    alloc::Delete<Mesh>(m);
  }

  printf("edge_flip test: %d flips\n", flips);
  return test_end();
}
