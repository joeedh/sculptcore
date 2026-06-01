#include "test_util.h"
#include "mesh_dump.h"

#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/utils/delaunay.h"
#include "mesh/utils/edge_collapse.h"

#include "litestl/math/vector.h"
#include "litestl/util/map.h"
#include "litestl/util/rand.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <memory>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::math;
using namespace litestl::util;

/* --- Mesh integrity (mirrors test_delaunay.cc::validateMesh) --- */
static bool validateMesh(Mesh &m, const char *tag)
{
  for (int ei : m.e) {
    int v1 = m.e.vs[ei][0];
    int v2 = m.e.vs[ei][1];
    if (v1 < 0 || v2 < 0 || v1 >= int(m.v.capacity()) || v2 >= int(m.v.capacity())) {
      fprintf(stderr, "[%s] edge %d bad vert refs %d %d\n", tag, ei, v1, v2);
      return false;
    }
    if (m.v.freemap[v1] || m.v.freemap[v2]) {
      fprintf(stderr, "[%s] edge %d references freed vert\n", tag, ei);
      return false;
    }
    if (v1 == v2) {
      fprintf(stderr, "[%s] edge %d is a self-loop\n", tag, ei);
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
      if (m.e.disk[next][side_n * 2] != ec ||
          m.e.disk[prev][side_p * 2 + 1] != ec) {
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
      int rn = m.c.radial_next[cc];
      int rp = m.c.radial_prev[cc];
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
      int c0 = m.l.c[li];
      int cc = c0;
      int n = 0;
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
        int ce = m.c.e[cc];
        int v_here = m.c.v[cc];
        int v_next = m.c.v[cn];
        int ev0 = m.e.vs[ce][0], ev1 = m.e.vs[ce][1];
        if (!((ev0 == v_here && ev1 == v_next) ||
              (ev1 == v_here && ev0 == v_next))) {
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

  return true;
}

static int64_t edgeKey(int a, int b)
{
  int lo = a < b ? a : b;
  int hi = a < b ? b : a;
  return (int64_t(lo) << 32) | uint32_t(hi);
}

static int64_t faceKeyFromMesh(Mesh &m, int fi)
{
  Vector<int, 8> verts;
  int li = m.f.l[fi];
  int c0 = m.l.c[li];
  int cc = c0;
  do {
    verts.append(m.c.v[cc]);
    cc = m.c.next[cc];
  } while (cc != c0);
  int n = int(verts.size());
  int min_i = 0;
  for (int i = 1; i < n; i++) {
    if (verts[i] < verts[min_i]) min_i = i;
  }
  Vector<int, 8> fwd, rev;
  for (int i = 0; i < n; i++) fwd.append(verts[(min_i + i) % n]);
  rev.append(fwd[0]);
  for (int i = n - 1; i >= 1; i--) rev.append(fwd[i]);
  bool useFwd = true;
  for (int i = 1; i < n; i++) {
    if (fwd[i] != rev[i]) {
      useFwd = fwd[i] < rev[i];
      break;
    }
  }
  uint64_t h = 1469598103934665603ull;
  const auto &use = useFwd ? fwd : rev;
  for (int v : use) {
    h ^= uint64_t(uint32_t(v));
    h *= 1099511628211ull;
  }
  return int64_t(h);
}

static bool noDuplicateEdges(Mesh &m, const char *tag)
{
  Set<int64_t> seen;
  for (int ei : m.e) {
    int64_t k = edgeKey(m.e.vs[ei][0], m.e.vs[ei][1]);
    if (!seen.add(k)) {
      fprintf(stderr, "[%s] duplicate edge between %d-%d\n",
              tag, m.e.vs[ei][0], m.e.vs[ei][1]);
      return false;
    }
  }
  return true;
}

static bool noDuplicateFaces(Mesh &m, const char *tag)
{
  Set<int64_t> seen;
  for (int fi : m.f) {
    int64_t k = faceKeyFromMesh(m, fi);
    if (!seen.add(k)) {
      fprintf(stderr, "[%s] duplicate face fi=%d\n", tag, fi);
      return false;
    }
  }
  return true;
}

/* Count boundary loops: walk edges with radial cycle of length 1 (only
 * one corner), grouping them into closed chains via shared vertices. */
static int countBoundaryLoops(Mesh &m)
{
  Vector<int> boundaryEdges;
  for (int ei : m.e) {
    int c0 = m.e.c[ei];
    if (c0 == ELEM_NONE) continue; /* wire */
    if (m.c.radial_next[c0] == c0) {
      boundaryEdges.append(ei);
    }
  }
  if (boundaryEdges.isEmpty()) return 0;

  Set<int> isBoundary;
  for (int ei : boundaryEdges) isBoundary.add(ei);

  Set<int> visited;
  int loops = 0;
  for (int ei : boundaryEdges) {
    if (visited.contains(ei)) continue;
    /* Walk along boundary: at each end-vertex find the next boundary edge. */
    int start = ei;
    int cur = ei;
    int prevV = m.e.vs[ei][0];
    int curV = m.e.vs[ei][1];
    int safety = 0;
    while (true) {
      visited.add(cur);
      /* Find next boundary edge incident to curV other than cur. */
      int nextE = ELEM_NONE;
      for (int ne : EdgeOfVertIter(&m, curV, m.v.e[curV])) {
        if (ne == cur) continue;
        if (!isBoundary.contains(ne)) continue;
        nextE = ne;
        break;
      }
      if (nextE == ELEM_NONE) break; /* dangling -- shouldn't happen */
      if (nextE == start) {
        loops++;
        break;
      }
      int a = m.e.vs[nextE][0], b = m.e.vs[nextE][1];
      prevV = curV;
      curV = (a == curV) ? b : a;
      cur = nextE;
      if (++safety > 1000000) break;
    }
  }
  return loops;
}

static int eulerChar(Mesh &m)
{
  return m.v.count - m.e.count + m.f.count;
}

/* --- Build a closed triangulated disc-shape by Delaunay-triangulating
 *     a random point cloud, then closing the boundary. To make it closed
 *     (no boundary loops) we sandwich two triangulations: project up and
 *     down sharing the boundary ring. Simplest: take a sphere-ish point
 *     cloud and Delaunay-triangulate two hemispheres separately, sharing
 *     the equator vertices. Even simpler for our test purposes: use a
 *     planar Delaunay triangulation as the input — it has 1 boundary
 *     loop, which is *fine* for this test as long as we only collapse
 *     interior edges and the loop count stays at 1. */
static Mesh *makeDelaunayDisc(int n, uint32_t seed)
{
  Random rnd(seed);
  Mesh *m = alloc::New<Mesh>("test_edge_collapse mesh");
  Vector<int> verts;
  /* Force a fixed boundary square so all interior collapses leave the
   * outer ring intact. Then sprinkle interior points. */
  int corners[4];
  corners[0] = m->make_vertex(float3(-1.0f, -1.0f, 0.0f));
  corners[1] = m->make_vertex(float3(1.0f, -1.0f, 0.0f));
  corners[2] = m->make_vertex(float3(1.0f, 1.0f, 0.0f));
  corners[3] = m->make_vertex(float3(-1.0f, 1.0f, 0.0f));
  for (int c : corners) verts.append(c);
  /* Interior. */
  Vector<float2> placed;
  int attempts = 0;
  while (int(verts.size()) < n + 4 && attempts < n * 50) {
    attempts++;
    float x = rnd.get_float() * 1.8f - 0.9f;
    float y = rnd.get_float() * 1.8f - 0.9f;
    bool dup = false;
    for (auto &q : placed) {
      if (std::fabs(q[0] - x) < 1e-2f && std::fabs(q[1] - y) < 1e-2f) {
        dup = true;
        break;
      }
    }
    if (dup) continue;
    placed.append(float2(x, y));
    verts.append(m->make_vertex(float3(x, y, 0.0f)));
  }

  Vector<int> outFaces;
  delaunayTriangulate(*m,
                      std::span<int>(verts.data(), verts.size()),
                      std::optional<float3>(float3(0, 0, 1)),
                      &outFaces);
  return m;
}

static int randInRange(Random &r, int lo, int hi)
{
  if (hi <= lo) return lo;
  return lo + int(r.get_int() % uint32_t(hi - lo));
}

/* Pick a random live edge whose endpoints are *not* on the outer
 * boundary loop, so collapses do not affect the boundary. */
static int pickInteriorEdge(Mesh &m, Random &rnd, const Set<int> &boundaryVerts)
{
  Vector<int> candidates;
  for (int ei : m.e) {
    int v0 = m.e.vs[ei][0];
    int v1 = m.e.vs[ei][1];
    if (boundaryVerts.contains(v0) || boundaryVerts.contains(v1)) continue;
    if (m.e.c[ei] == ELEM_NONE) continue; /* wire */
    candidates.append(ei);
  }
  if (candidates.isEmpty()) return ELEM_NONE;
  return candidates[randInRange(rnd, 0, int(candidates.size()))];
}

static Set<int> collectBoundaryVerts(Mesh &m)
{
  Set<int> s;
  for (int ei : m.e) {
    int c0 = m.e.c[ei];
    if (c0 == ELEM_NONE) continue;
    if (m.c.radial_next[c0] == c0) {
      s.add(m.e.vs[ei][0]);
      s.add(m.e.vs[ei][1]);
    }
  }
  return s;
}

struct Stats {
  int collapses = 0;
  int rejected = 0;
  int meshes = 0;
};

static bool runCollapseSession(const char *tag, Mesh &m, Random &rnd,
                               int maxCollapses, Stats &stats)
{
  if (!validateMesh(m, tag)) return false;
  if (!noDuplicateEdges(m, tag)) return false;
  if (!noDuplicateFaces(m, tag)) return false;

  std::unique_ptr<sculptcore::mesh::dump::MeshLog> meshlog;
  if (const char *logdir = sculptcore::mesh::dump::MeshLog::envDir()) {
    meshlog = std::make_unique<sculptcore::mesh::dump::MeshLog>(logdir, tag);
    meshlog->initial(m);
  }

  Set<int> boundaryVerts = collectBoundaryVerts(m);
  int boundaryLoopsBefore = countBoundaryLoops(m);

  for (int iter = 0; iter < maxCollapses; iter++) {
    int ei = pickInteriorEdge(m, rnd, boundaryVerts);
    if (ei == ELEM_NONE) break;

    int v_keep = m.e.vs[ei][0];
    int v_kill = m.e.vs[ei][1];

    /* Compute expected delta: for each face on the radial of `ei` that is
     * a triangle, dF -= 1, dE -= 1 (one of its non-collapsed edges merges
     * with the other). Non-triangle faces only lose a corner so dF
     * unchanged but they don't contribute extra edge loss. dV always -=1.
     * The collapsed edge itself contributes dE -= 1 separately. */
    int dV_expected = -1;
    int dE_expected = -1; /* the edge itself */
    int dF_expected = 0;
    {
      int c0 = m.e.c[ei];
      int cc = c0;
      do {
        int li = m.c.l[cc];
        int sz = m.l.size[li];
        if (sz == 3) {
          dF_expected -= 1;
          dE_expected -= 1; /* two edges merge into one */
        }
        cc = m.c.radial_next[cc];
      } while (cc != c0);
    }
    int chi_before = eulerChar(m);
    int V_before = m.v.count;
    int E_before = m.e.count;
    int F_before = m.f.count;

    /* Sanity: refuse pathological collapses that would create a non-manifold
     * mesh by joining two triangles that share two edges (a "double edge"
     * config). Detect: if v_keep and v_kill share more than 2 common
     * neighbors, skip — collapsing would create non-manifold edges. */
    {
      Set<int> nbrKeep;
      for (int e2 : EdgeOfVertIter(&m, v_keep, m.v.e[v_keep])) {
        int o = (m.e.vs[e2][0] == v_keep) ? m.e.vs[e2][1] : m.e.vs[e2][0];
        nbrKeep.add(o);
      }
      int common = 0;
      for (int e2 : EdgeOfVertIter(&m, v_kill, m.v.e[v_kill])) {
        int o = (m.e.vs[e2][0] == v_kill) ? m.e.vs[e2][1] : m.e.vs[e2][0];
        if (o == v_keep) continue;
        if (nbrKeep.contains(o)) common++;
      }
      /* For a triangle mesh, exactly 2 common neighbors = the two triangle
       * apex verts. More than that = a fold. */
      if (common > 2) {
        stats.rejected++;
        continue;
      }
    }

    sculptcore::mesh::dump::Highlight hl;
    if (meshlog) {
      hl.kind = 'e';
      hl.ids.append(ei);
    }

    auto ok = collapseEdge(m, ei);
    test_assert(bool(ok));
    if (meshlog) {
      meshlog->step("collapseEdge", hl, m);
    }
    if (!validateMesh(m, tag)) {
      fprintf(stderr, "[%s] integrity failed after collapse iter=%d ei=%d\n",
              tag, iter, ei);
      return false;
    }
    if (!noDuplicateEdges(m, tag)) return false;
    if (!noDuplicateFaces(m, tag)) return false;

    int dV = m.v.count - V_before;
    int dE = m.e.count - E_before;
    int dF = m.f.count - F_before;
    int chi_after = eulerChar(m);

    if (dV != dV_expected) {
      fprintf(stderr, "[%s] dV %d != %d\n", tag, dV, dV_expected);
      return false;
    }
    if (dF != dF_expected) {
      fprintf(stderr, "[%s] dF %d != %d (iter %d)\n", tag, dF, dF_expected, iter);
      return false;
    }
    if (dE != dE_expected) {
      fprintf(stderr, "[%s] dE %d != %d (iter %d)\n", tag, dE, dE_expected, iter);
      return false;
    }
    /* Euler char preserved iff dV - dE + dF == 0. For a triangle interior
     * collapse: -1 -(-3) + (-2) = 0. */
    if (chi_after - chi_before != (dV - dE + dF)) {
      fprintf(stderr, "[%s] chi delta arithmetic mismatch\n", tag);
      return false;
    }
    if ((dV - dE + dF) != 0) {
      fprintf(stderr,
              "[%s] chi changed unexpectedly: dV=%d dE=%d dF=%d (iter %d)\n",
              tag, dV, dE, dF, iter);
      return false;
    }

    /* No new boundary loops. */
    int loopsAfter = countBoundaryLoops(m);
    if (loopsAfter != boundaryLoopsBefore) {
      fprintf(stderr,
              "[%s] boundary loop count changed: %d -> %d (iter %d)\n",
              tag, boundaryLoopsBefore, loopsAfter, iter);
      return false;
    }

    stats.collapses++;
  }

  return true;
}

static int findEdge(Mesh &m, int a, int b)
{
  for (int ei : m.e) {
    int v0 = m.e.vs[ei][0], v1 = m.e.vs[ei][1];
    if ((v0 == a && v1 == b) || (v0 == b && v1 == a)) return ei;
  }
  return ELEM_NONE;
}

/* Seed a float4 "color" vertex attr from position so blends are verifiable. */
static AttrData<float4> *addColorAttr(Mesh &m)
{
  AttrRef &ref = m.v.attrs.ensure(AttrType::FLOAT4, "color", /*materialize=*/true);
  auto *data = static_cast<AttrData<float4> *>(ref.data);
  data->materialize_all();
  for (int vi : m.v) {
    float3 co = m.v.co[vi];
    (*data)[vi] = float4(co[0], co[1], co[2], 1.0f);
  }
  return data;
}

/* Build a regular octahedron (closed, every vertex interior, valence 4). */
static Mesh *makeOctahedron()
{
  Mesh *m = alloc::New<Mesh>("octahedron");
  int t = m->make_vertex(float3(0, 0, 1));
  int b = m->make_vertex(float3(0, 0, -1));
  int e0 = m->make_vertex(float3(1, 0, 0));
  int e1 = m->make_vertex(float3(0, 1, 0));
  int e2 = m->make_vertex(float3(-1, 0, 0));
  int e3 = m->make_vertex(float3(0, -1, 0));
  int eq[4] = {e0, e1, e2, e3};
  for (int i = 0; i < 4; i++) {
    int a = eq[i], c = eq[(i + 1) % 4];
    int top[3] = {t, a, c};
    int bot[3] = {b, c, a};
    m->make_face(std::span<int>(top, 3));
    m->make_face(std::span<int>(bot, 3));
  }
  return m;
}

int main()
{
  Stats stats;

  /* A handful of randomized Delaunay-triangulated discs of varying size,
   * with random interior-edge collapses. */
  int sizes[] = {16, 32, 64, 128};
  int seedBase = 17;
  for (int N : sizes) {
    for (int trial = 0; trial < 4; trial++) {
      uint32_t seed = uint32_t(seedBase + trial * 101 + N * 7);
      Mesh *m = makeDelaunayDisc(N, seed);
      Random rnd(seed ^ 0xdeadbeef);

      char tag[64];
      snprintf(tag, sizeof(tag), "delaunay-N%d-t%d", N, trial);
      bool ok = runCollapseSession(tag, *m, rnd, N, stats);
      test_assert(ok);

      alloc::Delete<Mesh>(m);
      stats.meshes++;
    }
  }

  /* Self-loop / wire-edge edge cases. */
  {
    Mesh m;
    int v0 = m.make_vertex(float3(0, 0, 0));
    int v1 = m.make_vertex(float3(1, 0, 0));
    int e = m.make_edge(v0, v1);
    auto ok = collapseEdge(m, e);
    test_assert(bool(ok));
    test_assert(m.v.count == 1);
    test_assert(m.e.count == 0);
    test_assert(validateMesh(m, "wire"));
  }

  /* Result-struct id reporting + survivor attribute blend, on a clean
   * interior collapse (octahedron edge, both endpoints interior). */
  {
    Mesh *m = makeOctahedron();
    AttrData<float4> *cd = addColorAttr(*m);
    int ei = findEdge(*m, 2, 3); /* equator e0-e1 */
    test_assert(ei != ELEM_NONE);

    int v_keep = m->e.vs[ei][0];
    int v_kill = m->e.vs[ei][1];
    float4 ckeep = (*cd)[v_keep];
    float4 ckill = (*cd)[v_kill];

    int V0 = m->v.count, E0 = m->e.count, F0 = m->f.count;

    EdgeCollapseResult res;
    auto ok = collapseEdge(*m, ei, std::nullopt, /*blend=*/0.5f, &res);
    test_assert(bool(ok));
    test_assert(validateMesh(*m, "octa-collapse"));

    /* Reported ids are consistent with the actual count deltas. */
    test_assert(res.v_keep == v_keep);
    test_assert(res.killed_vert == v_kill);
    test_assert(V0 - m->v.count == 1);
    test_assert(F0 - m->f.count ==
                int(res.killed_faces.size()) - int(res.created_faces.size()));
    test_assert(E0 - m->e.count ==
                int(res.killed_edges.size()) - int(res.created_edges.size()));
    /* The collapsed edge is among the killed edges. */
    {
      bool found = false;
      for (int e : res.killed_edges) {
        if (e == ei) found = true;
      }
      test_assert(found);
    }
    /* blend=0.5 -> survivor color is the midpoint of the two endpoints. */
    {
      float4 cm = (*cd)[v_keep];
      float4 expect = ckeep * 0.5f + ckill * 0.5f;
      for (int i = 0; i < 4; i++) {
        test_assert(std::fabs(cm[i] - expect[i]) < 1e-5f);
      }
    }
    alloc::Delete<Mesh>(m);
  }

  /* Link-condition refusal: an edge whose endpoints share a common neighbor
   * not formed by a face (here vertex E, joined by wire edges) would collapse
   * to a non-manifold result. collapseEdge must refuse and leave the mesh
   * untouched. */
  {
    Mesh m;
    int a = m.make_vertex(float3(0, 0, 0));
    int b = m.make_vertex(float3(1, 0, 0));
    int c = m.make_vertex(float3(0.5f, 1, 0));
    int d = m.make_vertex(float3(0.5f, -1, 0));
    int e = m.make_vertex(float3(0.5f, 0, 1));
    int f0[3] = {a, b, c};
    int f1[3] = {b, a, d};
    m.make_face(std::span<int>(f0, 3));
    m.make_face(std::span<int>(f1, 3));
    m.make_edge(a, e); /* extra common neighbor of a,b, with no face */
    m.make_edge(b, e);

    int ab = findEdge(m, a, b);
    test_assert(ab != ELEM_NONE);
    int V0 = m.v.count, E0 = m.e.count, F0 = m.f.count;

    EdgeCollapseResult res;
    auto ok = collapseEdge(m, ab, std::nullopt, 0.0f, &res);
    test_assert(!bool(ok)); /* refused */
    test_assert(m.v.count == V0 && m.e.count == E0 && m.f.count == F0);
    test_assert(validateMesh(m, "link-refusal"));
  }

  printf("edge_collapse test: %d meshes, %d collapses, %d rejected\n",
         stats.meshes, stats.collapses, stats.rejected);

  return test_end();
}
