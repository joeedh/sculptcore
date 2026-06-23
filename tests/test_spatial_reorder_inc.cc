/* Correctness of SpatialTree::applyReorderIncremental (mechanism B compaction).
 *
 * A reorder is a pure index permutation, so the incremental apply (remap node
 * caches in place, no rebuild) must yield a tree that answers queries identically
 * to the trusted full-rebuild path (applyReorder) for the same permutation, and
 * must preserve mesh integrity + geometry. We:
 *   1. build + (optionally) perturb two identical meshes,
 *   2. compute the SAME locality maps on both,
 *   3. apply full-rebuild on A, incremental on B,
 *   4. assert validateMesh(B), geometry signature unchanged, and castRay over a
 *      ray grid returns bit-identical hits on A vs B.
 */
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/rand.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "spatial/spatial.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

test_init;

#define TASSERT(expr)                                                                    \
  do {                                                                                   \
    if (!(expr)) {                                                                       \
      retval = 1;                                                                        \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                 \
      fflush(stderr);                                                                    \
    }                                                                                    \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace sculptcore::spatial;
using namespace litestl;
using namespace litestl::util;
using litestl::math::float3;

namespace {

void build_grid(Mesh &m, int N)
{
  Vector<int> v;
  v.resize((N + 1) * (N + 1));
  auto vat = [&](int i, int j) -> int & { return v[j * (N + 1) + i]; };
  for (int j = 0; j <= N; j++) {
    for (int i = 0; i <= N; i++) {
      float x = float(i) / float(N) - 0.5f;
      float y = float(j) / float(N) - 0.5f;
      /* Non-flat: each triangle has distinct geometry, so a wrong tree yields a
       * wrong hit p/normal/t (a flat plane would mask such errors). */
      float z = 0.15f * std::sin(x * 7.0f) * std::cos(y * 6.0f);
      vat(i, j) = m.make_vertex(float3(x, y, z));
    }
  }
  for (int j = 0; j < N; j++) {
    for (int i = 0; i < N; i++) {
      int v0 = vat(i, j), v1 = vat(i + 1, j), v2 = vat(i + 1, j + 1), v3 = vat(i, j + 1);
      if (m.find_edge(v0, v1) == ELEM_NONE) m.make_edge(v0, v1);
      if (m.find_edge(v1, v2) == ELEM_NONE) m.make_edge(v1, v2);
      if (m.find_edge(v2, v3) == ELEM_NONE) m.make_edge(v2, v3);
      if (m.find_edge(v3, v0) == ELEM_NONE) m.make_edge(v3, v0);
      int verts[4] = {v0, v1, v2, v3};
      m.make_face(std::span<int>(verts, 4));
    }
  }
}

bool validateMesh(Mesh &m, const char *tag)
{
  for (int ei : m.e) {
    int v1 = m.e.vs[ei][0], v2 = m.e.vs[ei][1];
    if (v1 < 0 || v2 < 0 || v1 >= int(m.v.capacity()) || v2 >= int(m.v.capacity()) ||
        m.v.freemap[v1] || m.v.freemap[v2] || v1 == v2) {
      fprintf(stderr, "[%s] edge %d bad/freed/self vert refs %d %d\n", tag, ei, v1, v2);
      return false;
    }
  }
  for (int fi : m.f) {
    int li = m.f.l[fi], c0 = m.l.c[li], cc = c0, n = 0;
    do {
      if (m.c.l[cc] != li) {
        fprintf(stderr, "[%s] corner %d c.l != list %d\n", tag, cc, li);
        return false;
      }
      cc = m.c.next[cc];
      if (++n > 1000000) { fprintf(stderr, "[%s] list %d open\n", tag, li); return false; }
    } while (cc != c0);
  }
  return true;
}

/* Permutation-invariant geometric edge signature (sorted endpoint-position-hash
 * pairs); unchanged across any correct reorder. */
uint64_t posHash(const float3 &p)
{
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < 3; i++) {
    uint32_t bits;
    float f = p[i];
    std::memcpy(&bits, &f, sizeof(bits));
    h = (h ^ uint64_t(bits)) * 1099511628211ull;
  }
  return h;
}
Vector<int64_t> geomSig(Mesh &m)
{
  Vector<int64_t> sig;
  for (int ei : m.e) {
    uint64_t a = posHash(m.v.co[m.e.vs[ei][0]]), b = posHash(m.v.co[m.e.vs[ei][1]]);
    uint64_t lo = a < b ? a : b, hi = a < b ? b : a;
    sig.append(int64_t(lo * 1099511628211ull ^ hi));
  }
  std::sort(sig.begin(), sig.end());
  return sig;
}
bool sigEqual(const Vector<int64_t> &a, const Vector<int64_t> &b)
{
  if (a.size() != b.size()) return false;
  for (int i = 0; i < int(a.size()); i++) if (a[i] != b[i]) return false;
  return true;
}

/* Cast a grid of downward rays; collect (ok, p, normal, t, face). */
struct RayHits {
  Vector<int> ok;
  Vector<float3> p, normal;
  Vector<float> t;
  Vector<int> face;
};
RayHits castGrid(SpatialTree &tree, int R)
{
  RayHits h;
  for (int j = 0; j < R; j++) {
    for (int i = 0; i < R; i++) {
      float x = (float(i) / float(R - 1) - 0.5f) * 0.98f;
      float y = (float(j) / float(R - 1) - 0.5f) * 0.98f;
      CastRayIsect isect;
      bool ok = tree.castRay(float3(x, y, 1.0f), float3(0, 0, -1.0f), isect);
      h.ok.append(ok ? 1 : 0);
      h.p.append(ok ? isect.p : float3(0, 0, 0));
      h.normal.append(ok ? isect.normal : float3(0, 0, 0));
      h.t.append(ok ? isect.t : 0.0f);
      h.face.append(ok ? isect.faceIndex : -1);
    }
  }
  return h;
}
void compareHits(const RayHits &a, const RayHits &b, const char *tag)
{
  TASSERT(a.ok.size() == b.ok.size());
  int hits = 0;
  for (int i = 0; i < int(a.ok.size()); i++) {
    if (a.ok[i] != b.ok[i]) {
      fprintf(stderr, "[%s] ray %d ok mismatch %d vs %d\n", tag, i, a.ok[i], b.ok[i]);
      retval = 1;
      continue;
    }
    if (!a.ok[i]) continue;
    hits++;
    /* Compare the geometric hit (p/normal/t) only. faceIndex is NOT compared: on
     * shared edges / near-coplanar tris the returned face is traversal-order
     * dependent, and the two trees have different partitions (full rebuilds, incr
     * preserves), so a differing face id with an identical hit point is expected. */
    bool bad = a.t[i] != b.t[i];
    for (int k = 0; k < 3; k++) {
      bad |= a.p[i][k] != b.p[i][k] || a.normal[i][k] != b.normal[i][k];
    }
    if (bad) {
      fprintf(stderr, "[%s] ray %d hit mismatch p(%.5f,%.5f,%.5f) t%.5f vs p(%.5f,%.5f,%.5f) t%.5f\n",
              tag, i, a.p[i][0], a.p[i][1], a.p[i][2], a.t[i],
              b.p[i][0], b.p[i][1], b.p[i][2], b.t[i]);
      retval = 1;
    }
  }
  if (hits == 0) {
    fprintf(stderr, "[%s] no rays hit — test vacuous\n", tag);
    retval = 1;
  }
}

/* Build a tree, optionally scramble element storage with a random full reorder
 * (so the locality maps below are a non-trivial permutation), and return maps. */
void scramble(Mesh &m, SpatialTree &tree, uint32_t seed)
{
  auto randPerm = [](int cap, Random &rnd) {
    Vector<int> p;
    for (int i = 0; i < cap; i++) p.append(i);
    for (int i = cap - 1; i > 0; i--) std::swap(p[i], p[int(rnd.get_int() % uint32_t(i + 1))]);
    return p;
  };
  Random rnd(seed);
  Vector<int> vp = randPerm(int(m.v.capacity()), rnd);
  Vector<int> ep = randPerm(int(m.e.capacity()), rnd);
  Vector<int> cp = randPerm(int(m.c.capacity()), rnd);
  Vector<int> lp = randPerm(int(m.l.capacity()), rnd);
  Vector<int> fp = randPerm(int(m.f.capacity()), rnd);
  tree.applyReorder(vp, ep, cp, lp, fp); // full path (rebuilds) — trusted scramble
}

void test_incremental_matches_full(int N, int leaf, uint32_t seed)
{
  char tag[64];
  snprintf(tag, sizeof(tag), "inc-N%d-l%d-s%u", N, leaf, seed);

  /* Two identical meshes/trees, scrambled identically so storage is fragmented. */
  Mesh mA, mB;
  build_grid(mA, N);
  build_grid(mB, N);
  SpatialTree tA(&mA), tB(&mB);
  tA.leaf_limit = leaf;
  tB.leaf_limit = leaf;
  tA.buildAll();
  tB.buildAll();
  scramble(mA, tA, seed);
  scramble(mB, tB, seed);

  Vector<int64_t> sig0 = geomSig(mA);
  TASSERT(sigEqual(sig0, geomSig(mB)));
  RayHits beforeB = castGrid(tB, 24);

  /* Same locality maps on both (identical trees ⇒ identical maps). */
  Vector<int> vA, eA, cA, lA, fA, vB, eB, cB, lB, fB;
  tA.computeLocalityMaps(vA, eA, cA, lA, fA);
  tB.computeLocalityMaps(vB, eB, cB, lB, fB);

  tA.applyReorder(vA, eA, cA, lA, fA);               // trusted full rebuild
  tB.applyReorderIncremental(vB, eB, cB, lB, fB);    // path under test

  TASSERT(validateMesh(mB, tag));
  TASSERT(sigEqual(sig0, geomSig(mB)));               // geometry preserved
  TASSERT(sigEqual(geomSig(mA), geomSig(mB)));        // same permutation applied

  RayHits hA = castGrid(tA, 24);
  RayHits hB = castGrid(tB, 24);
  compareHits(hA, hB, tag);                           // incremental tree == full tree
  /* And the incremental tree still answers what it did before the reorder. */
  compareHits(beforeB, hB, tag);
}

Vector<int> invertMap(span<int> map)
{
  Vector<int> inv;
  inv.resize(int(map.size()));
  for (int i = 0; i < int(map.size()); i++) inv[map[i]] = i;
  return inv;
}

/* Partial (region-scoped) map: relocate only a subset of leaves' elements (a
 * mostly-identity closed permutation). Assert it is a valid bijection both apply
 * paths agree on, geometry is preserved, and the reorder is exactly invertible
 * (the undo contract). */
void test_partial_matches_full(int N, int leaf, uint32_t seed)
{
  char tag[64];
  snprintf(tag, sizeof(tag), "partial-N%d-l%d-s%u", N, leaf, seed);

  Mesh mA, mB;
  build_grid(mA, N);
  build_grid(mB, N);
  SpatialTree tA(&mA), tB(&mB);
  tA.leaf_limit = leaf;
  tB.leaf_limit = leaf;
  tA.buildAll();
  tB.buildAll();
  scramble(mA, tA, seed);
  scramble(mB, tB, seed);

  Vector<int64_t> sig0 = geomSig(mA);
  RayHits beforeB = castGrid(tB, 24);

  /* Pick an explicit subset (every other leaf) so the map is genuinely partial —
   * identical leaf order on both trees ⇒ identical subset ⇒ identical maps. */
  auto pickSubset = [](SpatialTree &t) {
    Vector<SpatialNode *> all = t.leaves(), sub;
    for (int i = 0; i < int(all.size()); i++)
      if (i % 2 == 0) sub.append(all[i]);
    return sub;
  };
  Vector<SpatialNode *> subA = pickSubset(tA), subB = pickSubset(tB);

  Vector<int> vA, eA, cA, lA, fA, vB, eB, cB, lB, fB;
  Vector<int> movedA[5], movedB[5];
  tA.computeLocalityMapsPartial(subA, vA, eA, cA, lA, fA, movedA);
  tB.computeLocalityMapsPartial(subB, vB, eB, cB, lB, fB, movedB);
  for (int k = 0; k < 5; k++) TASSERT(movedA[k].size() == movedB[k].size());
  /* Genuinely partial: fewer verts moved than total live verts. */
  TASSERT(int(movedB[0].size()) < mB.v.count);

  /* Keep copies of the forward maps for the undo round-trip below. */
  Vector<int> vB0 = vB, eB0 = eB, cB0 = cB, lB0 = lB, fB0 = fB;

  tA.applyReorder(vA, eA, cA, lA, fA);            // trusted full rebuild
  /* Scoped apply (Phase 2): attribute permute restricted to the moved slots. */
  tB.applyReorderIncremental(vB, eB, cB, lB, fB,
                             movedB[0], movedB[1], movedB[2], movedB[3], movedB[4]);

  TASSERT(validateMesh(mB, tag));
  TASSERT(sigEqual(sig0, geomSig(mB)));           // geometry preserved
  TASSERT(sigEqual(geomSig(mA), geomSig(mB)));    // scoped == full rebuild

  RayHits hA = castGrid(tA, 24), hB = castGrid(tB, 24);
  compareHits(hA, hB, tag);
  compareHits(beforeB, hB, tag);

  /* Undo contract: the inverse permutation (same moved set) restores the prior
   * layout — applied scoped too. */
  Vector<int> vi = invertMap(vB0), ei = invertMap(eB0), ci = invertMap(cB0),
              li = invertMap(lB0), fi = invertMap(fB0);
  tB.applyReorderIncremental(vi, ei, ci, li, fi,
                             movedB[0], movedB[1], movedB[2], movedB[3], movedB[4]);
  TASSERT(validateMesh(mB, tag));
  TASSERT(sigEqual(sig0, geomSig(mB)));
  compareHits(beforeB, castGrid(tB, 24), tag);
}

} // namespace

int main()
{
  for (int N : {8, 16, 32}) {
    for (int leaf : {16, 64}) {
      for (uint32_t trial = 0; trial < 2; trial++) {
        uint32_t seed = 0x51 + trial * 7 + N * 13 + leaf;
        test_incremental_matches_full(N, leaf, seed);
        test_partial_matches_full(N, leaf, seed);
      }
    }
  }
  printf("spatial_reorder_inc test done\n");
  return retval;
}
