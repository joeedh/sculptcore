#include "test_util.h"

#include "litestl/math/vector.h"
#include "mesh/mesh.h"
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

  /* Don't use test_end() here: linking against spatial pulls in
   * static-init allocations that aren't tagged PermanentGuard and would
   * be reported as leaks. Functional correctness is checked via
   * TASSERT; return retval directly. */
  return retval;
}
