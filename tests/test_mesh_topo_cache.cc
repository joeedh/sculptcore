#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "mesh/mesh_topo_cache.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>

test_init;

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
using namespace litestl;
using namespace litestl::util;
using litestl::math::float3;

namespace {

/* Full radial-edge integrity check (disk/radial/loop closure + cross-links),
 * copied from test_mesh_reorder.cc. */
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
    const auto &slot = m.v.disk[vi];
    int n = DiskSlabArena::count(slot);
    const int *sp_ = m.disk_arena.span(slot);
    if (n == 0 || diskEdge(sp_[0]) != e0) {
      fprintf(stderr, "[%s] vert %d disk head/slab mismatch\n", tag, vi);
      return false;
    }
    for (int i = 0; i < n; i++) {
      int ec = diskEdge(sp_[i]), side = diskSide(sp_[i]);
      if (ec < 0 || ec >= int(m.e.capacity()) || m.e.freemap[ec] ||
          m.e.vs[ec][side] != vi) {
        fprintf(stderr, "[%s] vert %d disk slab entry invalid e=%d\n", tag, vi, ec);
        return false;
      }
      for (int j = i + 1; j < n; j++) {
        if (sp_[j] == sp_[i]) {
          fprintf(stderr, "[%s] vert %d disk slab duplicate e=%d\n", tag, vi, ec);
          return false;
        }
      }
    }
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

  return true;
}

/* Independent reference 1-ring: scan every live edge, collect the far endpoint
 * of those incident to v. This shares no code with the CSR builder (which walks
 * the disk cycle via EdgeOfVertIter), so a match validates the cache against an
 * orthogonal derivation rather than the same loop. */
Vector<int> refRing1(Mesh &m, int v)
{
  Vector<int> out;
  for (int e : m.e) {
    if (m.e.vs[e][0] == v) {
      out.append(m.e.vs[e][1]);
    } else if (m.e.vs[e][1] == v) {
      out.append(m.e.vs[e][0]);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

Vector<int> csrRing1(VertNbrCSR &csr, int v)
{
  Vector<int> out;
  uint32_t off = csr.offsets[v];
  uint32_t cnt = csr.offsets[v + 1] - off;
  for (uint32_t i = 0; i < cnt; i++) {
    out.append(csr.nbr_verts[off + i]);
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool ringEqual(const Vector<int> &a, const Vector<int> &b)
{
  if (a.size() != b.size()) return false;
  for (int i = 0; i < int(a.size()); i++) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

void build_grid(Mesh &m, int N)
{
  Vector<int> v;
  v.resize((N + 1) * (N + 1));
  auto vat = [&](int i, int j) -> int & { return v[j * (N + 1) + i]; };

  for (int j = 0; j <= N; j++) {
    for (int i = 0; i <= N; i++) {
      vat(i, j) = m.make_vertex(float3(float(i) / N, float(j) / N, 0.0f));
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

/* Verify the cached CSR 1-ring matches the independent edge-scan reference for
 * every live vertex. */
void assertCacheMatches(Mesh &m, const char *tag)
{
  VertNbrCSR &csr = const_cast<VertNbrCSR &>(m.topo_cache.ensureRing1(m));
  for (int v : m.v) {
    Vector<int> got = csrRing1(csr, v);
    Vector<int> want = refRing1(m, v);
    if (!ringEqual(got, want)) {
      fprintf(stderr, "[%s] vert %d 1-ring mismatch: csr=%d ref=%d\n", tag, v,
              int(got.size()), int(want.size()));
      retval = 1;
    }
  }
}

/* --- Test 1: grid topology --- */
void test_grid(int N)
{
  char tag[32];
  snprintf(tag, sizeof(tag), "grid-N%d", N);

  Mesh m;
  build_grid(m, N);
  assertCacheMatches(m, tag);
  TASSERT(m.topo_cache.valid(m));
}

/* --- Test 2: non-manifold fan (one edge shared by 3 faces) --- */
void test_nonmanifold_fan()
{
  const char *tag = "fan";
  Mesh m;
  int v0 = m.make_vertex(float3(0, 0, 0));
  int v1 = m.make_vertex(float3(1, 0, 0));
  int fans[3] = {
      m.make_vertex(float3(0, 1, 0)),
      m.make_vertex(float3(0, -1, 0)),
      m.make_vertex(float3(0, 0, 1)),
  };
  for (int k = 0; k < 3; k++) {
    int vk = fans[k];
    if (m.find_edge(v0, v1) == ELEM_NONE) m.make_edge(v0, v1);
    if (m.find_edge(v1, vk) == ELEM_NONE) m.make_edge(v1, vk);
    if (m.find_edge(vk, v0) == ELEM_NONE) m.make_edge(vk, v0);
    int verts[3] = {v0, v1, vk};
    m.make_face(std::span<int>(verts, 3));
  }

  assertCacheMatches(m, tag);

  /* v0 sees v1 plus all three fan tips through its disk, regardless of the
   * non-manifold radial run on edge v0-v1. */
  VertNbrCSR &csr = const_cast<VertNbrCSR &>(m.topo_cache.ensureRing1(m));
  TASSERT(csrRing1(csr, v0).size() == 4);
}

/* --- Test 3: invalidation on topology edit (kill_edge) --- */
void test_invalidation()
{
  const char *tag = "inval";
  Mesh m;
  int a = m.make_vertex(float3(0, 0, 0));
  int b = m.make_vertex(float3(1, 0, 0));
  int c = m.make_vertex(float3(2, 0, 0));
  int eab = m.make_edge(a, b);
  m.make_edge(b, c);

  m.topo_cache.ensureRing1(m);
  TASSERT(m.topo_cache.valid(m));
  assertCacheMatches(m, tag);

  /* a's 1-ring is {b} before the kill. */
  VertNbrCSR &csr0 = const_cast<VertNbrCSR &>(m.topo_cache.ring1);
  TASSERT(csrRing1(csr0, a).size() == 1);

  m.kill_edge(eab);
  /* The stamp bump must have flipped validity even before the next rebuild. */
  TASSERT(!m.topo_cache.valid(m));

  /* Rebuild matches the post-edit reference: a is now isolated. */
  assertCacheMatches(m, tag);
  TASSERT(m.topo_cache.valid(m));
  VertNbrCSR &csr1 = const_cast<VertNbrCSR &>(m.topo_cache.ring1);
  TASSERT(csrRing1(csr1, a).size() == 0);
}

/* Capture every TOPO link column (plus the size/list_count bookkeeping
 * rebuildLinks also recomputes) keyed by slot, so a post-rebuild comparison
 * can assert bit-identical restoration. */
struct LinkSnapshot {
  Vector<int> v_e;
  /* Per-vert disk-slab sequences as a CSR (offsets are arena-private, so the
   * snapshot records the packed entry SEQUENCE, which must restore exactly). */
  Vector<int> v_disk_off, v_disk_blob;
  Vector<int> e_c, e_v0, e_v1;
  Vector<int> c_v, c_e, c_l, c_next, c_prev, c_rnext, c_rprev;
  Vector<int> l_c, l_f, l_next, l_size;
  Vector<int> f_l, f_lcount;
};

LinkSnapshot snapshotLinks(Mesh &m)
{
  LinkSnapshot s;
  int Vc = int(m.v.capacity()), Ec = int(m.e.capacity()), Cc = int(m.c.capacity());
  int Lc = int(m.l.capacity()), Fc = int(m.f.capacity());
  s.v_e.resize(Vc);
  s.v_disk_off.resize(Vc + 1);
  s.e_c.resize(Ec); s.e_v0.resize(Ec); s.e_v1.resize(Ec);
  s.c_v.resize(Cc); s.c_e.resize(Cc); s.c_l.resize(Cc);
  s.c_next.resize(Cc); s.c_prev.resize(Cc); s.c_rnext.resize(Cc); s.c_rprev.resize(Cc);
  s.l_c.resize(Lc); s.l_f.resize(Lc); s.l_next.resize(Lc); s.l_size.resize(Lc);
  s.f_l.resize(Fc); s.f_lcount.resize(Fc);
  for (int v : m.v) s.v_e[v] = m.v.e[v];
  for (int v = 0; v < Vc; v++) {
    s.v_disk_off[v] = int(s.v_disk_blob.size());
    if (!m.v.freemap[v]) {
      const auto &slot = m.v.disk[v];
      int n = DiskSlabArena::count(slot);
      const int *p = m.disk_arena.span(slot);
      for (int i = 0; i < n; i++) s.v_disk_blob.append(p[i]);
    }
  }
  s.v_disk_off[Vc] = int(s.v_disk_blob.size());
  for (int e : m.e) {
    s.e_c[e] = m.e.c[e]; s.e_v0[e] = m.e.vs[e][0]; s.e_v1[e] = m.e.vs[e][1];
  }
  for (int c : m.c) {
    s.c_v[c] = m.c.v[c]; s.c_e[c] = m.c.e[c]; s.c_l[c] = m.c.l[c];
    s.c_next[c] = m.c.next[c]; s.c_prev[c] = m.c.prev[c];
    s.c_rnext[c] = m.c.radial_next[c]; s.c_rprev[c] = m.c.radial_prev[c];
  }
  for (int l : m.l) {
    s.l_c[l] = m.l.c[l]; s.l_f[l] = m.l.f[l]; s.l_next[l] = m.l.next[l];
    s.l_size[l] = m.l.size[l];
  }
  for (int f : m.f) { s.f_l[f] = m.f.l[f]; s.f_lcount[f] = m.f.list_count[f]; }
  return s;
}

/* Scribble a sentinel over every live link so a successful rebuild proves the
 * value was reconstructed, not merely left untouched. */
void corruptLinks(Mesh &m)
{
  const int X = 0x6bad;
  for (int v : m.v) { m.v.e[v] = X; m.v.disk[v] = math::int2(0, 0); }
  for (int e : m.e) {
    m.e.c[e] = X; m.e.vs[e][0] = X; m.e.vs[e][1] = X;
  }
  for (int c : m.c) {
    m.c.v[c] = X; m.c.e[c] = X; m.c.l[c] = X;
    m.c.next[c] = X; m.c.prev[c] = X; m.c.radial_next[c] = X; m.c.radial_prev[c] = X;
  }
  for (int l : m.l) { m.l.c[l] = X; m.l.f[l] = X; m.l.next[l] = X; m.l.size[l] = X; }
  for (int f : m.f) { m.f.l[f] = X; m.f.list_count[f] = X; }
}

void compareLinks(Mesh &m, const LinkSnapshot &s, const char *tag)
{
  for (int v : m.v) TASSERT(m.v.e[v] == s.v_e[v]);
  for (int v : m.v) {
    const auto &slot = m.v.disk[v];
    int n = DiskSlabArena::count(slot);
    const int *p = m.disk_arena.span(slot);
    int a = s.v_disk_off[v], b = s.v_disk_off[v + 1];
    TASSERT(n == b - a);
    for (int i = 0; i < n && i < b - a; i++) {
      TASSERT(p[i] == s.v_disk_blob[a + i]);
    }
  }
  for (int e : m.e) {
    TASSERT(m.e.c[e] == s.e_c[e]);
    TASSERT(m.e.vs[e][0] == s.e_v0[e] && m.e.vs[e][1] == s.e_v1[e]);
  }
  for (int c : m.c) {
    TASSERT(m.c.v[c] == s.c_v[c]);
    TASSERT(m.c.e[c] == s.c_e[c]);
    TASSERT(m.c.l[c] == s.c_l[c]);
    TASSERT(m.c.next[c] == s.c_next[c] && m.c.prev[c] == s.c_prev[c]);
    TASSERT(m.c.radial_next[c] == s.c_rnext[c] && m.c.radial_prev[c] == s.c_rprev[c]);
  }
  for (int l : m.l) {
    TASSERT(m.l.c[l] == s.l_c[l] && m.l.f[l] == s.l_f[l]);
    TASSERT(m.l.next[l] == s.l_next[l] && m.l.size[l] == s.l_size[l]);
  }
  for (int f : m.f) TASSERT(m.f.l[f] == s.f_l[f] && m.f.list_count[f] == s.f_lcount[f]);
  (void)tag;
}

/* Append a non-manifold fan (edge shared by 3 faces) to a grid mesh so the
 * round-trip exercises >2 radial corners and multiple disk topologies. */
void add_fan(Mesh &m)
{
  int v0 = m.make_vertex(float3(2, 0, 0));
  int v1 = m.make_vertex(float3(3, 0, 0));
  int tips[3] = {
      m.make_vertex(float3(2, 1, 0)),
      m.make_vertex(float3(2, -1, 0)),
      m.make_vertex(float3(2, 0, 1)),
  };
  for (int k = 0; k < 3; k++) {
    int vk = tips[k];
    if (m.find_edge(v0, v1) == ELEM_NONE) m.make_edge(v0, v1);
    if (m.find_edge(v1, vk) == ELEM_NONE) m.make_edge(v1, vk);
    if (m.find_edge(vk, v0) == ELEM_NONE) m.make_edge(vk, v0);
    int verts[3] = {v0, v1, vk};
    m.make_face(std::span<int>(verts, 3));
  }
}

/* --- Test 4: FrozenTopo build + in-place link rebuild is bit-identical. --- */
void test_frozen_roundtrip(int N)
{
  char tag[32];
  snprintf(tag, sizeof(tag), "frozen-N%d", N);

  Mesh m;
  build_grid(m, N);
  add_fan(m);
  TASSERT(validateMesh(m, tag));

  LinkSnapshot snap = snapshotLinks(m);

  FrozenTopo ft;
  ft.build(m);
  TASSERT(ft.built);

  corruptLinks(m);
  ft.rebuildLinks(m);

  compareLinks(m, snap, tag);
  TASSERT(validateMesh(m, tag));
}

/* --- Test 5: full freezeTopo/thawTopo path (page free + rebuild) --- */
void test_freeze_thaw(int N)
{
  char tag[32];
  snprintf(tag, sizeof(tag), "freeze-N%d", N);

  Mesh m;
  build_grid(m, N);
  add_fan(m);
  TASSERT(validateMesh(m, tag));

  LinkSnapshot snap = snapshotLinks(m);

  /* Reference 1-rings captured before freezing, to confirm the cached CSR keeps
   * serving correct neighbors while the live links are gone. */
  Vector<Vector<int>> want;
  want.resize(m.v.capacity());
  for (int v : m.v) want[v] = refRing1(m, v);

  /* Snapshot c.v before freezing: it is TOPO_KEEP_FROZEN, so it must stay
   * materialized and unchanged while frozen (the per-frame spatial path reads
   * it through cached corner indices). */
  Vector<int> cv_before;
  cv_before.resize(m.c.capacity());
  for (int c : m.c) cv_before[c] = m.c.v[c];

  m.freezeTopo();
  TASSERT(m.topo_frozen);
  /* Cache stays valid + correct while frozen (no stamp bump occurred). */
  TASSERT(m.topo_cache.valid(m));
  {
    VertNbrCSR &csr = const_cast<VertNbrCSR &>(m.topo_cache.ring1);
    for (int v : m.v) TASSERT(ringEqual(csrRing1(csr, v), want[v]));
  }
  /* c.v survived the page-freeing and reads back its pre-freeze values. */
  for (int c : m.c) TASSERT(m.c.v[c] == cv_before[c]);

  /* Explicit thaw rebuilds the live links bit-identically. */
  m.thawTopo();
  TASSERT(!m.topo_frozen);
  compareLinks(m, snap, tag);
  TASSERT(validateMesh(m, tag));

  /* Idempotent round-trip: freeze again, then let a topology mutator auto-thaw.
   * make_vertex adds an isolated vert (no TOPO links of its own) so the rest of
   * the mesh must come back through rebuildLinks. */
  m.freezeTopo();
  TASSERT(m.topo_frozen);
  int nv = m.make_vertex(float3(9, 9, 9));
  TASSERT(!m.topo_frozen); /* mutator auto-thawed */
  TASSERT(m.v.e[nv] == ELEM_NONE);
  /* Pre-existing topology survived the freeze→auto-thaw cycle intact. */
  for (int v : m.v) {
    if (v == nv) continue;
    TASSERT(m.v.e[v] == snap.v_e[v]);
  }
  TASSERT(validateMesh(m, tag));
}

/* --- Test 6: freezing actually drops RAM, thawing restores it --- */
void test_frozen_ram()
{
  const char *tag = "ram";
  /* A grid big enough that the freed link columns dwarf allocator noise.
   * N=64 => ~4k verts / ~8k tris; the TOPO link set is hundreds of KB. */
  Mesh m;
  build_grid(m, 64);
  m.topo_cache.ensureRing1(m); /* warm the cache so its bytes are not counted as the drop */

  int before = alloc::getMemorySize();
  m.freezeTopo();
  int frozen = alloc::getMemorySize();
  m.thawTopo();
  int after = alloc::getMemorySize();

  /* Freezing frees the pure-iteration link pages. */
  TASSERT(frozen < before);
  /* Thawing re-materializes them (back to roughly the pre-freeze footprint;
   * exact equality isn't guaranteed by the allocator, so just bound it). */
  TASSERT(after >= frozen);
  if (!(frozen < before)) {
    fprintf(stderr, "[%s] no RAM drop: before=%d frozen=%d after=%d\n", tag, before,
            frozen, after);
  }
  TASSERT(validateMesh(m, tag));
}

} // namespace

int main()
{
  for (int N : {2, 4, 8, 16}) {
    test_grid(N);
  }
  test_nonmanifold_fan();
  test_invalidation();
  for (int N : {2, 3, 4, 8}) {
    test_frozen_roundtrip(N);
  }
  for (int N : {2, 3, 4, 8}) {
    test_freeze_thaw(N);
  }
  test_frozen_ram();

  printf("mesh_topo_cache test done\n");
  return retval;
}
