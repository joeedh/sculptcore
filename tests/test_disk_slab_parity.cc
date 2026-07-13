#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/rand.h"
#include "litestl/util/vector.h"
#include "mesh/disk_slab.h"
#include "mesh/mesh_enums.h"

#include <cstdint>
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

using namespace sculptcore::mesh;
using litestl::math::int2;
using litestl::math::int4;
using litestl::util::Random;
using litestl::util::Vector;

namespace {

/* diskPack mirror (mesh_types.h pulls in the whole mesh; keep this test on the
 * slab header alone). */
int pack(int e, int side)
{
  return (e << 1) | side;
}

/* Reference model: today's side-bit-encoded disk cycles, verbatim
 * Mesh::disk_insert / disk_remove semantics on local arrays. The slab must
 * reproduce this model's e_of_v sequences byte-for-byte. */
struct CycleRef {
  Vector<int2> vs;   /* per edge: endpoint verts */
  Vector<int4> disk; /* per edge: prev/next per side, packed links */
  Vector<int> v_e;   /* per vert: head edge or ELEM_NONE */

  void ensure(int verts, int edges)
  {
    while (int(v_e.size()) < verts) {
      v_e.append(ELEM_NONE);
    }
    while (int(vs.size()) < edges) {
      vs.append(int2(ELEM_NONE, ELEM_NONE));
      disk.append(int4(ELEM_NONE));
    }
  }

  int side(int e, int v) const
  {
    return vs[e][0] == v ? 0 : 1;
  }

  void insert(int e1, int v1)
  {
    int side1 = side(e1, v1);
    int self = pack(e1, side1);
    if (v_e[v1] == ELEM_NONE) {
      v_e[v1] = e1;
      disk[e1][side1 * 2] = self;
      disk[e1][side1 * 2 + 1] = self;
      return;
    }
    int e2 = v_e[v1];
    int side2 = side(e2, v1);
    int prevLink = disk[e2][side2 * 2];
    int prev = prevLink >> 1, side3 = prevLink & 1;
    disk[e2][side2 * 2] = self;
    disk[e1][side1 * 2] = prevLink;
    disk[e1][side1 * 2 + 1] = pack(e2, side2);
    disk[prev][side3 * 2 + 1] = self;
  }

  void remove(int e1, int v1)
  {
    int side1 = side(e1, v1);
    int prevLink = disk[e1][side1 * 2];
    int nextLink = disk[e1][side1 * 2 + 1];
    int prev = prevLink >> 1, sidep = prevLink & 1;
    int next = nextLink >> 1, siden = nextLink & 1;
    disk[prev][sidep * 2 + 1] = nextLink;
    disk[next][siden * 2] = prevLink;
    if (e1 == v_e[v1]) {
      v_e[v1] = next;
    }
    if (e1 == v_e[v1]) {
      v_e[v1] = ELEM_NONE;
    }
  }

  /* Packed e_of_v walk sequence, exactly EdgeOfVertIter's order. */
  Vector<int, 32> walk(int v) const
  {
    Vector<int, 32> out;
    int e0 = v_e[v];
    if (e0 == ELEM_NONE) {
      return out;
    }
    int s = side(e0, v), e = e0;
    do {
      out.append(pack(e, s));
      int link = disk[e][s * 2 + 1];
      e = link >> 1;
      s = link & 1;
    } while (e != e0 && int(out.size()) < 1000000);
    return out;
  }
};

/* Slab side of the model. */
struct SlabModel {
  DiskSlabArena arena;
  Vector<int2> v_slot;

  void ensure(int verts)
  {
    while (int(v_slot.size()) < verts) {
      v_slot.append(int2(0, 0));
    }
  }

  Vector<int, 32> walk(int v)
  {
    Vector<int, 32> out;
    const int2 &slot = v_slot[v];
    int n = DiskSlabArena::count(slot);
    const int *p = arena.span(slot);
    for (int i = 0; i < n; i++) {
      out.append(p[i]);
    }
    return out;
  }
};

struct Harness {
  CycleRef ref;
  SlabModel slab;
  int verts;

  Vector<int> liveEdges;
  Vector<int> freeEdgeIds;
  int nextEdgeId = 0;

  explicit Harness(int verts_) : verts(verts_)
  {
    ref.ensure(verts, 0);
    slab.ensure(verts);
  }

  bool vertsEqual(int v, const char *tag)
  {
    Vector<int, 32> a = ref.walk(v);
    Vector<int, 32> b = slab.walk(v);
    if (a.size() != b.size()) {
      fprintf(stderr, "[%s] vert %d: ref len %d != slab len %d\n", tag, v,
              int(a.size()), int(b.size()));
      return false;
    }
    for (int i = 0; i < int(a.size()); i++) {
      if (a[i] != b[i]) {
        fprintf(stderr, "[%s] vert %d: entry %d: ref %d != slab %d\n", tag, v, i, a[i],
                b[i]);
        return false;
      }
    }
    return true;
  }

  int makeEdge(int v1, int v2)
  {
    int e;
    if (freeEdgeIds.size() > 0) {
      e = freeEdgeIds.pop_back();
    } else {
      e = nextEdgeId++;
      ref.ensure(verts, nextEdgeId);
    }
    ref.vs[e] = int2(v1, v2);
    ref.insert(e, v1);
    ref.insert(e, v2);
    slab.arena.insert(slab.v_slot[v1], pack(e, 0));
    slab.arena.insert(slab.v_slot[v2], pack(e, 1));
    liveEdges.append(e);
    return e;
  }

  void killEdge(int idx)
  {
    int e = liveEdges[idx];
    int v1 = ref.vs[e][0], v2 = ref.vs[e][1];
    ref.remove(e, v1);
    ref.remove(e, v2);
    TASSERT(slab.arena.remove(slab.v_slot[v1], pack(e, 0)));
    TASSERT(slab.arena.remove(slab.v_slot[v2], pack(e, 1)));
    ref.vs[e] = int2(ELEM_NONE, ELEM_NONE);
    ref.disk[e] = int4(ELEM_NONE);
    liveEdges[idx] = liveEdges[liveEdges.size() - 1];
    liveEdges.pop_back();
    freeEdgeIds.append(e);
  }

  /* Collapse-merge: move vsrc's ring onto vdst in walk order (batch append),
   * killing edges that connect vsrc-vdst — the order class collapseEdge's
   * relink/kill sequence produces. */
  void mergeVerts(int vdst, int vsrc)
  {
    Vector<int, 32> ring = ref.walk(vsrc);
    for (int i = 0; i < int(ring.size()); i++) {
      int e = ring[i] >> 1;
      int other = ref.vs[e][0] == vsrc ? ref.vs[e][1] : ref.vs[e][0];
      /* find live index */
      int idx = -1;
      for (int j = 0; j < int(liveEdges.size()); j++) {
        if (liveEdges[j] == e) {
          idx = j;
          break;
        }
      }
      TASSERT(idx >= 0);
      if (other == vdst) {
        killEdge(idx);
        continue;
      }
      /* relink endpoint vsrc -> vdst (remove from both, reinsert), keeping
       * the same edge id — mirrors relink_edge_verts. */
      int v1 = ref.vs[e][0], v2 = ref.vs[e][1];
      ref.remove(e, v1);
      ref.remove(e, v2);
      TASSERT(slab.arena.remove(slab.v_slot[v1], pack(e, 0)));
      TASSERT(slab.arena.remove(slab.v_slot[v2], pack(e, 1)));
      int n1 = v1 == vsrc ? vdst : v1;
      int n2 = v2 == vsrc ? vdst : v2;
      ref.vs[e] = int2(n1, n2);
      ref.disk[e] = int4(ELEM_NONE);
      ref.insert(e, n1);
      ref.insert(e, n2);
      slab.arena.insert(slab.v_slot[n1], pack(e, 0));
      slab.arena.insert(slab.v_slot[n2], pack(e, 1));
    }
  }

  bool allEqual(const char *tag)
  {
    for (int v = 0; v < verts; v++) {
      if (!vertsEqual(v, tag)) {
        return false;
      }
    }
    return true;
  }
};

void test_directed_cases()
{
  Harness h(4);

  /* Singleton + head removal + re-add. */
  int e01 = h.makeEdge(0, 1);
  TASSERT(h.allEqual("singleton"));
  int e02 = h.makeEdge(0, 2);
  int e03 = h.makeEdge(0, 3);
  TASSERT(h.allEqual("fan3"));
  /* Remove the head edge of vert 0 (e01 is v0's head). */
  h.killEdge(0);
  (void)e01;
  TASSERT(h.allEqual("head-removal"));
  /* Remove remaining, back to empty, then re-add. */
  while (h.liveEdges.size() > 0) {
    h.killEdge(0);
  }
  TASSERT(h.allEqual("empty"));
  int eNew = h.makeEdge(2, 3);
  (void)eNew;
  (void)e02;
  (void)e03;
  TASSERT(h.allEqual("re-add"));
}

void test_growth_boundaries()
{
  /* Star vertex: push valence through every pow2 boundary up to 33, then
   * peel back down through them (exercises grow-copy + shift-remove). */
  const int leaves = 33;
  Harness h(leaves + 1);
  for (int i = 0; i < leaves; i++) {
    h.makeEdge(0, 1 + i);
    TASSERT(h.vertsEqual(0, "star-grow"));
  }
  TASSERT(h.allEqual("star-full"));
  Random rnd(7);
  while (h.liveEdges.size() > 0) {
    h.killEdge(int(rnd.get_int() % uint32_t(h.liveEdges.size())));
    TASSERT(h.vertsEqual(0, "star-shrink"));
  }
  TASSERT(h.allEqual("star-empty"));
}

void test_random_churn(uint32_t seed)
{
  const int verts = 40;
  Harness h(verts);
  Random rnd(seed);

  for (int step = 0; step < 4000; step++) {
    uint32_t op = rnd.get_int() % 100;
    if (op < 55 || h.liveEdges.size() == 0) {
      int v1 = int(rnd.get_int() % verts);
      int v2 = int(rnd.get_int() % verts);
      if (v1 == v2) {
        continue;
      }
      h.makeEdge(v1, v2);
    } else if (op < 90) {
      h.killEdge(int(rnd.get_int() % uint32_t(h.liveEdges.size())));
    } else {
      int vdst = int(rnd.get_int() % verts);
      int vsrc = int(rnd.get_int() % verts);
      if (vdst == vsrc) {
        continue;
      }
      h.mergeVerts(vdst, vsrc);
    }
    if (!h.allEqual("churn")) {
      fprintf(stderr, "seed %u step %d diverged\n", seed, step);
      retval = 1;
      return;
    }
  }
}

} // namespace

int main()
{
  test_directed_cases();
  test_growth_boundaries();
  for (uint32_t trial = 0; trial < 8; trial++) {
    test_random_churn(0xd15c0000u + trial * 7919u);
  }
  printf("disk_slab parity test done (retval=%d)\n", retval);
  return retval;
}
