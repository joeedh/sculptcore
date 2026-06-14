#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/rand.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "meshlog/meshlog_base.h"
#include "spatial/spatial.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

test_init;

/* Local assert that flips retval on failure (shared test_assert keeps
 * retval=0 on the failure branch). */
#define TASSERT(expr)                                                                    \
  do {                                                                                   \
    if (!(expr)) {                                                                        \
      retval = 1;                                                                        \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                  \
      fflush(stderr);                                                                    \
    }                                                                                    \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace sculptcore::meshlog;
using namespace litestl;
using namespace litestl::util;
using litestl::math::float3;

namespace {

/* --- Mesh integrity (mirrors test_edge_collapse.cc::validateMesh) --- */
bool validateMesh(Mesh &m, const char *tag)
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

/* Quantize a vertex position into a stable 64-bit hash of its exact float
 * bits. Reorder moves values by std::move (bit-preserving) so exact bits are
 * the right granularity — no epsilon needed. */
uint64_t posHash(const float3 &p)
{
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < 3; i++) {
    uint32_t bits;
    float f = p[i];
    std::memcpy(&bits, &f, sizeof(bits));
    h ^= uint64_t(bits);
    h *= 1099511628211ull;
  }
  return h;
}

/* Permutation-invariant geometric signature: the sorted multiset of edges
 * keyed by the unordered pair of endpoint position hashes. Independent of
 * element indexing, so it is preserved across any correct reorder. */
Vector<int64_t> geomEdgeSignature(Mesh &m)
{
  Vector<int64_t> sig;
  for (int ei : m.e) {
    uint64_t a = posHash(m.v.co[m.e.vs[ei][0]]);
    uint64_t b = posHash(m.v.co[m.e.vs[ei][1]]);
    uint64_t lo = a < b ? a : b;
    uint64_t hi = a < b ? b : a;
    uint64_t k = lo * 1099511628211ull ^ hi;
    sig.append(int64_t(k));
  }
  std::sort(sig.begin(), sig.end());
  return sig;
}

bool sigEqual(const Vector<int64_t> &a, const Vector<int64_t> &b)
{
  if (a.size() != b.size()) return false;
  for (int i = 0; i < int(a.size()); i++) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

/* Snapshot every live vertex's position keyed by storage index. Capacity is
 * stable across reorder/rebuild (no elements added/removed), so an index-keyed
 * snapshot is directly comparable after an inverse round-trip. */
Vector<float3> snapshotPositions(Mesh &m)
{
  Vector<float3> out;
  out.resize(m.v.capacity());
  for (int vi : m.v) {
    out[vi] = m.v.co[vi];
  }
  return out;
}

bool positionsMatch(Mesh &m, const Vector<float3> &snap, const char *tag)
{
  for (int vi : m.v) {
    if (m.v.co[vi][0] != snap[vi][0] || m.v.co[vi][1] != snap[vi][1] ||
        m.v.co[vi][2] != snap[vi][2]) {
      fprintf(stderr, "[%s] vert %d position drift after round-trip\n", tag, vi);
      return false;
    }
  }
  return true;
}

/* A random full bijection over [0, cap) via Fisher-Yates. */
Vector<int> randPerm(int cap, Random &rnd)
{
  Vector<int> p;
  for (int i = 0; i < cap; i++) p.append(i);
  for (int i = cap - 1; i > 0; i--) {
    int j = int(rnd.get_int() % uint32_t(i + 1));
    std::swap(p[i], p[j]);
  }
  return p;
}

void build_grid(Mesh &m, int N)
{
  Vector<int> v;
  v.resize((N + 1) * (N + 1));
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
}

/* --- Test 1: arbitrary random per-domain permutations preserve topology
 *     and move geometry exactly along the vertex map. --- */
void test_random_reorder(int N, uint32_t seed)
{
  char tag[64];
  snprintf(tag, sizeof(tag), "rand-N%d-s%u", N, seed);

  Mesh m;
  build_grid(m, N);
  TASSERT(validateMesh(m, tag));

  Vector<float3> origPos = snapshotPositions(m);
  Vector<int64_t> origSig = geomEdgeSignature(m);

  Random rnd(seed);
  Vector<int> vmap = randPerm(int(m.v.capacity()), rnd);
  Vector<int> emap = randPerm(int(m.e.capacity()), rnd);
  Vector<int> cmap = randPerm(int(m.c.capacity()), rnd);
  Vector<int> lmap = randPerm(int(m.l.capacity()), rnd);
  Vector<int> fmap = randPerm(int(m.f.capacity()), rnd);

  m.reorder_verts(vmap);
  m.reorder_edges(emap);
  m.reorder_corners(cmap);
  m.reorder_lists(lmap);
  m.reorder_faces(fmap);

  TASSERT(validateMesh(m, tag));
  TASSERT(sigEqual(origSig, geomEdgeSignature(m)));

  /* Geometry follows the vertex map exactly: the vert formerly at old index
   * now lives at vmap[old] with bit-identical position. */
  for (int oldi = 0; oldi < int(origPos.size()); oldi++) {
    /* Only verts that were live before are meaningful; freed-slot data is
     * unspecified. Reconstruct liveness from the original mesh by checking
     * the new home is live and matches. */
    int newi = vmap[oldi];
    if (m.v.freemap[newi]) continue;
    if (m.v.co[newi][0] != origPos[oldi][0] || m.v.co[newi][1] != origPos[oldi][1] ||
        m.v.co[newi][2] != origPos[oldi][2]) {
      fprintf(stderr, "[%s] vert old %d -> new %d position mismatch\n", tag, oldi, newi);
      retval = 1;
    }
  }
}

/* --- Test 2: SpatialTree locality reorder recorded through MeshLog, with a
 *     full undo (inverse) / redo (forward) round-trip. --- */
void test_locality_undo_redo(int N, int leafLimit, uint32_t seed)
{
  char tag[64];
  snprintf(tag, sizeof(tag), "loc-N%d-l%d-s%u", N, leafLimit, seed);

  Mesh m;
  build_grid(m, N);

  MeshLog log;
  log.setActiveMesh(&m);

  spatial::SpatialTree tree(&m);
  tree.leaf_limit = leafLimit;
  tree.buildAll();

  TASSERT(validateMesh(m, tag));
  Vector<float3> origPos = snapshotPositions(m);
  Vector<int64_t> origSig = geomEdgeSignature(m);

  Vector<int> vmap, emap, cmap, lmap, fmap;
  tree.computeLocalityMaps(vmap, emap, cmap, lmap, fmap);

  log.beginStep(false);
  log.pushReorderChunk(vmap, emap, cmap, lmap, fmap);
  tree.applyReorder(vmap, emap, cmap, lmap, fmap);
  log.endStep();

  TASSERT(validateMesh(m, tag));
  TASSERT(sigEqual(origSig, geomEdgeSignature(m)));
  Vector<float3> postPos = snapshotPositions(m);

  /* Undo: inverse permutation must restore the original index-keyed state. */
  log.undo(&m, &tree);
  TASSERT(validateMesh(m, tag));
  TASSERT(sigEqual(origSig, geomEdgeSignature(m)));
  TASSERT(positionsMatch(m, origPos, tag));

  /* Redo: forward permutation must reproduce the post-reorder state. */
  log.redo(&m, &tree);
  TASSERT(validateMesh(m, tag));
  TASSERT(sigEqual(origSig, geomEdgeSignature(m)));
  TASSERT(positionsMatch(m, postPos, tag));
}

} // namespace

int main()
{
  for (int N : {4, 8, 16, 24}) {
    for (int trial = 0; trial < 4; trial++) {
      test_random_reorder(N, uint32_t(0x1234 + trial * 7 + N * 131));
    }
  }

  for (int N : {8, 16, 24}) {
    for (int leaf : {16, 64}) {
      for (int trial = 0; trial < 2; trial++) {
        test_locality_undo_redo(N, leaf, uint32_t(0xabcd + trial * 13 + N * 17 + leaf));
      }
    }
  }

  printf("mesh_reorder test done\n");
  return retval;
}
