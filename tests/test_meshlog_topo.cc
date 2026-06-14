#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "mesh/utils/edge_flip.h"
#include "mesh/utils/edge_split.h"
#include "meshlog/meshlog_base.h"

#include <array>
#include <cstdio>

test_init;

/* Local assert that actually flips retval to nonzero on failure
 * (the shared test_assert macro has retval=0 on the failure branch). */
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
using namespace sculptcore::meshlog;
using litestl::math::float3;

namespace {

/* Build a 4-vert quad: returns {v0, v1, v2, v3, e01, e12, e23, e30, f}. */
struct Quad {
  int v[4];
  int e[4];
  int f;
};

Quad make_quad(Mesh &m)
{
  Quad q;
  q.v[0] = m.make_vertex(float3(0.0f, 0.0f, 0.0f));
  q.v[1] = m.make_vertex(float3(1.0f, 0.0f, 0.0f));
  q.v[2] = m.make_vertex(float3(1.0f, 1.0f, 0.0f));
  q.v[3] = m.make_vertex(float3(0.0f, 1.0f, 0.0f));
  q.e[0] = m.make_edge(q.v[0], q.v[1]);
  q.e[1] = m.make_edge(q.v[1], q.v[2]);
  q.e[2] = m.make_edge(q.v[2], q.v[3]);
  q.e[3] = m.make_edge(q.v[3], q.v[0]);
  int verts[4] = {q.v[0], q.v[1], q.v[2], q.v[3]};
  q.f = m.make_face(std::span<int>(verts, 4));
  return q;
}

/* Two triangles sharing the diagonal edge v0-v2 of a unit quad. The shared
 * diagonal is the edge an in-place flip/split operates on. */
struct TwoTris {
  int v[4];
  int e_diag;  /* v0-v2 */
  int f[2];
};

TwoTris make_two_tris(Mesh &m)
{
  TwoTris t;
  t.v[0] = m.make_vertex(float3(0.0f, 0.0f, 0.0f));
  t.v[1] = m.make_vertex(float3(1.0f, 0.0f, 0.0f));
  t.v[2] = m.make_vertex(float3(1.0f, 1.0f, 0.0f));
  t.v[3] = m.make_vertex(float3(0.0f, 1.0f, 0.0f));
  int t0[3] = {t.v[0], t.v[1], t.v[2]};
  int t1[3] = {t.v[0], t.v[2], t.v[3]};
  t.f[0] = m.make_face(std::span<int>(t0, 3));
  t.f[1] = m.make_face(std::span<int>(t1, 3));
  t.e_diag = m.find_edge(t.v[0], t.v[2]);
  return t;
}

/* Per-id topology + geometry signature. In-place Euler ops reuse element ids and
 * meshlog restores them exactly, so a faithful undo must reproduce this id-by-id
 * (not merely the same shape under relabeling). Edges are stored unordered
 * (min,max) since an edge is undirected; faces store their loop's verts rotated
 * to start at the min vert (winding-preserving), so a flip that rewires a face
 * in place is still detected if its loop changes. */
struct TopoSig {
  litestl::util::Vector<float3> vco;            /* per vert id; (NAN) if free */
  litestl::util::Vector<std::array<int, 2>> ev; /* per edge id; {-1,-1} if free */
  litestl::util::Vector<litestl::util::Vector<int>> fv; /* per face id; empty if free */
};

TopoSig captureSig(Mesh &m)
{
  TopoSig s;
  s.vco.resize(m.v.capacity());
  for (int v = 0; v < int(m.v.capacity()); v++) {
    s.vco[v] = m.v.freemap[v] ? float3(1e30f, 1e30f, 1e30f) : m.v.co[v];
  }
  s.ev.resize(m.e.capacity());
  for (int e = 0; e < int(m.e.capacity()); e++) {
    if (m.e.freemap[e]) {
      s.ev[e] = {-1, -1};
    } else {
      int a = m.e.vs[e][0], b = m.e.vs[e][1];
      s.ev[e] = a < b ? std::array<int, 2>{a, b} : std::array<int, 2>{b, a};
    }
  }
  s.fv.resize(m.f.capacity());
  for (int f = 0; f < int(m.f.capacity()); f++) {
    if (m.f.freemap[f]) {
      continue;
    }
    litestl::util::Vector<int> loop;
    int li = m.f.l[f], lc0 = m.l.c[li], lcc = lc0;
    do {
      loop.append(m.c.v[lcc]);
      lcc = m.c.next[lcc];
    } while (lcc != lc0);
    /* Rotate to start at the min vert (winding preserved). */
    int mi = 0;
    for (int i = 1; i < int(loop.size()); i++) {
      if (loop[i] < loop[mi]) {
        mi = i;
      }
    }
    litestl::util::Vector<int> rot;
    for (int i = 0; i < int(loop.size()); i++) {
      rot.append(loop[(mi + i) % int(loop.size())]);
    }
    s.fv[f] = std::move(rot);
  }
  return s;
}

bool sig_equal(const TopoSig &a, const TopoSig &b)
{
  if (a.vco.size() != b.vco.size() || a.ev.size() != b.ev.size() ||
      a.fv.size() != b.fv.size()) {
    return false;
  }
  for (int v = 0; v < int(a.vco.size()); v++) {
    if ((a.vco[v] - b.vco[v]).length() > 1e-6f) {
      return false;
    }
  }
  for (int e = 0; e < int(a.ev.size()); e++) {
    if (a.ev[e][0] != b.ev[e][0] || a.ev[e][1] != b.ev[e][1]) {
      return false;
    }
  }
  for (int f = 0; f < int(a.fv.size()); f++) {
    if (a.fv[f].size() != b.fv[f].size()) {
      return false;
    }
    for (int i = 0; i < int(a.fv[f].size()); i++) {
      if (a.fv[f][i] != b.fv[f][i]) {
        return false;
      }
    }
  }
  return true;
}

struct MeshCounts {
  int vc, ec, cc, lc, fc;
};

MeshCounts counts(Mesh &m)
{
  return {m.v.count, m.e.count, m.c.count, m.l.count, m.f.count};
}

bool counts_equal(const MeshCounts &a, const MeshCounts &b)
{
  return a.vc == b.vc && a.ec == b.ec && a.cc == b.cc && a.lc == b.lc && a.fc == b.fc;
}

void print_counts(const char *tag, const MeshCounts &c)
{
  printf("  %s: v=%d e=%d c=%d l=%d f=%d\n", tag, c.vc, c.ec, c.cc, c.lc, c.fc);
}

} // namespace

int main()
{
  /* Test 1: round-trip creation of a single vert. */
  {
    Mesh m;
    Quad q = make_quad(m);
    (void)q;
    MeshCounts before = counts(m);

    MeshLog log;
    log.setActiveMesh(&m);

    log.beginStep();
    int vnew = m.make_vertex(float3(2.0f, 2.0f, 2.0f), log.callbacks());
    log.endStep();

    MeshCounts after = counts(m);
    TASSERT(after.vc == before.vc + 1);
    TASSERT(m.v.co[vnew][0] == 2.0f);

    log.undo(&m, nullptr);
    MeshCounts undone = counts(m);
    print_counts("create undo", undone);
    TASSERT(counts_equal(undone, before));

    log.redo(&m, nullptr);
    MeshCounts redone = counts(m);
    print_counts("create redo", redone);
    TASSERT(counts_equal(redone, after));
    TASSERT(m.v.co[vnew][0] == 2.0f);
  }

  /* Test 2: round-trip kill of a face. */
  {
    Mesh m;
    Quad q = make_quad(m);
    MeshCounts before = counts(m);

    MeshLog log;
    log.setActiveMesh(&m);

    log.beginStep();
    m.kill_face(q.f, log.callbacks());
    log.endStep();

    MeshCounts after = counts(m);
    print_counts("kill_face after", after);
    TASSERT(after.fc == before.fc - 1);
    TASSERT(after.lc == before.lc - 1);
    TASSERT(after.cc == before.cc - 4);
    /* verts and edges remain. */
    TASSERT(after.vc == before.vc);
    TASSERT(after.ec == before.ec);

    log.undo(&m, nullptr);
    MeshCounts undone = counts(m);
    print_counts("kill_face undone", undone);
    TASSERT(counts_equal(undone, before));

    log.redo(&m, nullptr);
    MeshCounts redone = counts(m);
    print_counts("kill_face redone", redone);
    TASSERT(counts_equal(redone, after));
  }

  /* Test 3: create-then-kill within one step → net no-op. */
  {
    Mesh m;
    make_quad(m);
    MeshCounts before = counts(m);

    MeshLog log;
    log.setActiveMesh(&m);

    log.beginStep();
    int vtmp = m.make_vertex(float3(5.0f, 5.0f, 5.0f), log.callbacks());
    m.kill_vertex(vtmp, log.callbacks());
    log.endStep();

    MeshCounts after = counts(m);
    TASSERT(counts_equal(after, before));

    log.undo(&m, nullptr);
    TASSERT(counts_equal(counts(m), before));

    log.redo(&m, nullptr);
    TASSERT(counts_equal(counts(m), before));
  }

  /* Test 4: kill an Existed vert (no prior change) → Existed/Dead record,
   * undo restores it at its original index. */
  {
    Mesh m;
    /* Isolated vertex with no incident edges. */
    int v_iso = m.make_vertex(float3(7.0f, 8.0f, 9.0f));

    Quad q = make_quad(m);
    (void)q;

    MeshCounts before = counts(m);
    float3 saved_co = m.v.co[v_iso];

    MeshLog log;
    log.setActiveMesh(&m);

    log.beginStep();
    m.kill_vertex(v_iso, log.callbacks());
    log.endStep();

    TASSERT(m.v.count == before.vc - 1);

    log.undo(&m, nullptr);
    TASSERT(m.v.count == before.vc);
    TASSERT(m.v.co[v_iso][0] == saved_co[0]);
    TASSERT(m.v.co[v_iso][1] == saved_co[1]);
    TASSERT(m.v.co[v_iso][2] == saved_co[2]);

    log.redo(&m, nullptr);
    TASSERT(m.v.count == before.vc - 1);
  }

  /* Test 5: in-place edge flip undo/redo restores topology bit-for-bit.
   * The flip reuses the edge + both face ids (no create/kill), so a faithful
   * undo must reproduce the exact per-id connectivity, not just the same shape. */
  {
    Mesh m;
    TwoTris t = make_two_tris(m);
    TASSERT(t.e_diag != ELEM_NONE);
    MeshCounts before = counts(m);
    TopoSig sigBefore = captureSig(m);

    MeshLog log;
    log.setActiveMesh(&m);

    log.beginStep();
    EdgeFlipResult res;
    bool ok = bool(flipEdge(m, t.e_diag, &res, log.callbacks()));
    log.endStep();
    TASSERT(ok);

    MeshCounts after = counts(m);
    print_counts("flip after", after);
    TASSERT(counts_equal(after, before)); /* flip is dV=dE=dF=0 */
    /* The reused edge now holds the other diagonal (v1-v3). */
    int da = m.e.vs[t.e_diag][0], db = m.e.vs[t.e_diag][1];
    bool isV1V3 = (da == t.v[1] && db == t.v[3]) || (da == t.v[3] && db == t.v[1]);
    TASSERT(isV1V3);
    TopoSig sigAfter = captureSig(m);
    TASSERT(!sig_equal(sigAfter, sigBefore)); /* connectivity actually changed */

    log.undo(&m, nullptr);
    print_counts("flip undone", counts(m));
    TASSERT(counts_equal(counts(m), before));
    TASSERT(sig_equal(captureSig(m), sigBefore)); /* bit-for-bit restore */

    log.redo(&m, nullptr);
    print_counts("flip redone", counts(m));
    TASSERT(counts_equal(counts(m), after));
    TASSERT(sig_equal(captureSig(m), sigAfter));
  }

  /* Test 6: in-place edge split undo/redo restores topology bit-for-bit.
   * The split reuses the edge id (relinked to the v0-vm child) and each incident
   * face id (one bisected half), creating vm, the vm-v1 child, spokes, and the
   * second halves. Undo must restore the original ids exactly. */
  {
    Mesh m;
    TwoTris t = make_two_tris(m);
    TASSERT(t.e_diag != ELEM_NONE);
    MeshCounts before = counts(m);
    TopoSig sigBefore = captureSig(m);
    int da0 = m.e.vs[t.e_diag][0], db0 = m.e.vs[t.e_diag][1];

    MeshLog log;
    log.setActiveMesh(&m);

    log.beginStep();
    EdgeSplitResult res;
    bool ok = bool(splitEdge(m, t.e_diag, &res, log.callbacks()));
    log.endStep();
    TASSERT(ok);

    MeshCounts after = counts(m);
    print_counts("split after", after);
    /* Interior diagonal: 2 incident tris -> dV=+1, dE=+3, dF=+2. */
    TASSERT(after.vc == before.vc + 1);
    TASSERT(after.ec == before.ec + 3);
    TASSERT(after.fc == before.fc + 2);
    TASSERT(res.new_vert != ELEM_NONE);
    /* The reused edge is now the v0-vm child (one endpoint is the midpoint). */
    int sa = m.e.vs[t.e_diag][0], sb = m.e.vs[t.e_diag][1];
    TASSERT(sa == res.new_vert || sb == res.new_vert);
    TopoSig sigAfter = captureSig(m);

    log.undo(&m, nullptr);
    print_counts("split undone", counts(m));
    TASSERT(counts_equal(counts(m), before));
    TASSERT(sig_equal(captureSig(m), sigBefore)); /* bit-for-bit restore */
    /* The reused edge id is back to its original endpoints. */
    int ra = m.e.vs[t.e_diag][0], rb = m.e.vs[t.e_diag][1];
    TASSERT((ra == da0 && rb == db0) || (ra == db0 && rb == da0));

    log.redo(&m, nullptr);
    print_counts("split redone", counts(m));
    TASSERT(counts_equal(counts(m), after));
    TASSERT(sig_equal(captureSig(m), sigAfter));
  }

  /* Don't use test_end() here: linking against spatial pulls in
   * static-init allocations that aren't tagged PermanentGuard and would
   * be reported as leaks. Functional correctness is checked via
   * TASSERT; return retval directly. */
  return retval;
}
