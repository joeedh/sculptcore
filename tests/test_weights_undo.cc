/**
 * AttrType::WEIGHTS across the meshlog (mesh/attr_weights.h + meshlog_base.h).
 *
 * A logged WEIGHTS cell is the same 4-byte slot index the mesh column holds, so
 * the log's raw byte machinery keeps working on it — but a stored index only
 * means anything while the DeformPool still holds a reference for it. Two
 * things therefore have to be true, and both are what this file pins:
 *
 *  - every path that *duplicates* a cell into the log retains, and every path
 *    that drops one releases (a swap does neither, and must not);
 *  - the pool outlives the mesh, because the log routinely does. Scene::~Scene
 *    frees its Mesh in the destructor body while its MeshLog is a member, so
 *    the log releases into a pool whose creator is already gone.
 */

#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/span.h"
#include "mesh/attr_weights.h"
#include "mesh/deform_pool.h"
#include "mesh/mesh.h"
#include "meshlog/meshlog_base.h"

#include <array>
#include <cstdio>

test_init;

/* Local assert that reports file/line, matching test_meshlog_topo.cc. */
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
using litestl::util::span;

namespace {

/* A quad plus one wire vertex. The wire vertex is what the tests kill: it
 * carries the run under test and its removal cascades into nothing. */
struct Fixture {
  int v[4];
  int f;
  int vw;
};

Fixture makeFixture(Mesh &m)
{
  Fixture q;
  q.v[0] = m.make_vertex(float3(0.0f, 0.0f, 0.0f));
  q.v[1] = m.make_vertex(float3(1.0f, 0.0f, 0.0f));
  q.v[2] = m.make_vertex(float3(1.0f, 1.0f, 0.0f));
  q.v[3] = m.make_vertex(float3(0.0f, 1.0f, 0.0f));
  m.make_edge(q.v[0], q.v[1]);
  m.make_edge(q.v[1], q.v[2]);
  m.make_edge(q.v[2], q.v[3]);
  m.make_edge(q.v[3], q.v[0]);
  int verts[4] = {q.v[0], q.v[1], q.v[2], q.v[3]};
  q.f = m.make_face(std::span<int>(verts, 4));
  q.vw = m.make_vertex(float3(2.0f, 0.0f, 0.0f));
  return q;
}

void setRun(Mesh &m, int v, int group, float weight)
{
  DeformWeight w{group, weight};
  ensureVertWeights(m, "weights").setRun(v, span<const DeformWeight>(&w, 1));
}

int attrIndex(Mesh &m, const litestl::util::string &name)
{
  for (int i = 0; i < int(m.v.attrs.attrs.size()); i++) {
    if (m.v.attrs.attrs[i].name == name) {
      return i;
    }
  }
  return -1;
}

} // namespace

/* LogChunkElems' row store (ChunkElemData): cpyFrom duplicates a cell, so it
 * must retain, and ~ChunkElemData's AttrGroup must hand the reference back. */
static void testElemStoreHoldsRun()
{
  Mesh m;
  Fixture q = makeFixture(m);

  setRun(m, q.v[0], 3, 1.0f);
  DeformPool &pool = m.deformPool();
  TASSERT(pool.liveSlotCount() == 2); /* the empty run + A */

  const int idx = attrIndex(m, "weights");
  TASSERT(idx >= 0);
  span<const AttrRef> refs(&m.v.attrs.attrs[idx], 1);

  {
    meshlog::detail::ChunkElemData store(0, ElemType::VERTEX);
    store.appendFrom(m.v.attrs, q.v[0], refs);

    /* The mesh drops A; from here only the captured row names it. */
    setRun(m, q.v[0], 4, 1.0f);
    pool.sweep();
    TASSERT(pool.liveSlotCount() == 3); /* empty + A (logged) + B (live) */
  }

  pool.sweep();
  TASSERT(pool.liveSlotCount() == 2); /* empty + B */
}

/* LogChunkTopo's raw-byte rows (ChunkElemRow): killing a vertex captures its
 * run into begin_body, undo writes it back, and the run stays reachable in
 * between only because the row holds a reference to it. */
static void testTopoUndoRestoresWeights()
{
  Mesh m;
  Fixture q = makeFixture(m);

  setRun(m, q.vw, 7, 0.5f);
  DeformPool &pool = m.deformPool();

  MeshLog log;
  log.setActiveMesh(&m);

  log.beginStep(true);
  m.kill_vertex(q.vw, log.callbacks());
  log.endStep();

  /* Freeing an element does not clear its column entry, so drop the mesh's now
   * meaningless reference by hand — otherwise the log's own is untestable, and
   * the restored weight below could have come from the stale cell. */
  findVertWeights(m, "weights").clear(q.vw);
  pool.sweep();
  TASSERT(pool.liveSlotCount() == 2); /* empty + the run only the log names */

  log.undo(&m, nullptr);
  TASSERT(findVertWeights(m, "weights").weight(q.vw, 7) == 0.5f);

  log.redo(&m, nullptr);
  log.undo(&m, nullptr);
  TASSERT(findVertWeights(m, "weights").weight(q.vw, 7) == 0.5f);
}

/* Dropping the log drops its rows, and a run nothing else names becomes
 * reclaimable — the ~ChunkElemRow / ~LogChunkTopo half of the discipline. */
static void testDroppingLogReleasesRun()
{
  Mesh m;
  Fixture q = makeFixture(m);

  setRun(m, q.vw, 2, 1.0f);
  DeformPool &pool = m.deformPool();

  {
    MeshLog log;
    log.setActiveMesh(&m);

    log.beginStep(true);
    m.kill_vertex(q.vw, log.callbacks());
    log.endStep();

    findVertWeights(m, "weights").clear(q.vw);
    pool.sweep();
    TASSERT(pool.liveSlotCount() == 2);
  }

  pool.sweep();
  TASSERT(pool.liveSlotCount() == 1); /* just the empty run */
}

/* The case the DeformPool user count exists for: the mesh is destroyed while a
 * log step still names slots in its pool. The log must be able to release into
 * a live pool, and the pool must then be freed exactly once — which is what
 * test_end()'s leak check actually proves. */
static void testLogOutlivesMesh()
{
  Mesh *m = litestl::alloc::New<Mesh>("test Mesh");
  Fixture q = makeFixture(*m);

  setRun(*m, q.vw, 1, 1.0f);
  setRun(*m, q.v[0], 5, 0.25f);

  MeshLog *log = litestl::alloc::New<MeshLog>("test MeshLog");
  log->setActiveMesh(m);

  log->beginStep(true);
  m->kill_vertex(q.vw, log->callbacks());
  log->endStep();

  TASSERT(log->totalMemSize() > 0.0);

  litestl::alloc::Delete(m);
  litestl::alloc::Delete(log);
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  testElemStoreHoldsRun();
  testTopoUndoRestoresWeights();
  testDroppingLogReleasesRun();
  testLogOutlivesMesh();

  return test_end();
}
