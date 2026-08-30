/**
 * The AttrType::WEIGHTS column (mesh/attr_weights.h) — the layer that binds a
 * plain 4-byte-per-element attribute column to the interned DeformPool.
 *
 * The column itself is deliberately dumb, so what has to be pinned is the
 * reference discipline around it: that every path which drops a column value
 * (overwrite, element reallocation, layer removal, shrink, mesh teardown)
 * releases the run it dropped, and that no path duplicates one without
 * retaining it. DeformPool::auditRefcounts, fed from the column itself, is the
 * check — a mismatch is exactly a write that bypassed the funnel.
 */

#include "test_util.h"

#include "mesh/attr_merge.h"
#include "mesh/attr_weights.h"
#include "mesh/deform_pool.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "mesh/utils/attr_interp.h"
#include "mesh/utils/edge_split.h"
#include "mesh/utils/triangulate.h"
#include "meshlog/meshlog_base.h"

#include "litestl/util/span.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>
#include <thread>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using litestl::util::span;
using litestl::util::Vector;

// mesh_shapes allocates through the tracked allocator, so the mesh must go back
// the same way — plain `delete` corrupts the heap.
struct MeshPtr {
  Mesh *m = nullptr;
  MeshPtr(int dimen)
  {
    m = createCube(dimen, 0.5f);
    m->thawTopo();
    triangulateMesh(*m);
    m->recalc_normals();
  }
  ~MeshPtr()
  {
    litestl::alloc::Delete<Mesh>(m);
  }
  MeshPtr(const MeshPtr &) = delete;
  Mesh *operator->()
  {
    return m;
  }
  Mesh &operator*()
  {
    return *m;
  }
};

static Vector<DeformWeight> run(std::initializer_list<DeformWeight> ws)
{
  Vector<DeformWeight> v;
  for (const DeformWeight &w : ws) {
    v.append(w);
  }
  return v;
}

static span<const DeformWeight> asRun(Vector<DeformWeight> &v)
{
  return span<const DeformWeight>(v.data(), v.size());
}

// Every reference the mesh's WEIGHTS columns hold, versus what the pool thinks.
static int auditMesh(Mesh &m, const string &name)
{
  DeformPool *pool = m.deformPoolOrNull();
  if (!pool) {
    return 0;
  }

  Vector<WeightSlot> roots;
  findVertWeights(m, name).collectRoots(roots);

  return pool->auditRefcounts(span<const WeightSlot>(roots.data(), roots.size()));
}

// The pool is created on demand and shared by every element group, so a weights
// column on any domain — and a meshlog chunk logging one — resolves the same
// slot indices.
static void testPoolOwnership()
{
  MeshPtr m(3);

  test_assert(m->deformPoolOrNull() == nullptr);
  test_assert(m->v.attrs.deform_pool == nullptr);

  DeformPool &pool = m->deformPool();

  test_assert(m->deformPoolOrNull() == &pool);
  test_assert(m->v.attrs.deform_pool == &pool);
  test_assert(m->e.attrs.deform_pool == &pool);
  test_assert(m->c.attrs.deform_pool == &pool);
  test_assert(m->f.attrs.deform_pool == &pool);

  // Idempotent — a second call must not swap the pool out from under a column.
  test_assert(&m->deformPool() == &pool);
}

static void testReadWrite()
{
  MeshPtr m(3);
  WeightsRef w = ensureVertWeights(*m, "weights");
  test_assert(w.exists());

  // A fresh column is all slot 0, i.e. "no weights", with nothing interned.
  for (int v : m->v) {
    test_assert(w.slot(v).index == 0);
    test_assert(w.runSize(v) == 0);
    test_assert(w.weight(v, 0) == 0.0f);
  }

  auto a = run({{1, 0.75f}, {0, 0.25f}});
  const int v0 = *m->v.begin();
  w.setRun(v0, asRun(a));

  // Stored canonicalized: group-ascending, whatever order it went in as.
  DeformWeight buf[4];
  test_assert(w.getRun(v0, buf, 4) == 2);
  test_assert((buf[0] == DeformWeight{0, 0.25f}));
  test_assert((buf[1] == DeformWeight{1, 0.75f}));
  test_assert(w.weight(v0, 1) == 0.75f);
  test_assert(w.weight(v0, 7) == 0.0f);

  test_assert(auditMesh(*m, "weights") == 0);
}

// Interning means equal weight sets collapse onto one slot; the column can then
// be compared by index, and the pool holds one run for the whole region.
static void testSharing()
{
  MeshPtr m(3);
  WeightsRef w = ensureVertWeights(*m, "weights");
  DeformPool &pool = m->deformPool();

  auto a = run({{0, 1.0f}});
  int n = 0;
  for (int v : m->v) {
    w.setRun(v, asRun(a));
    n++;
  }
  test_assert(n > 8);

  const WeightSlot first = w.slot(*m->v.begin());
  for (int v : m->v) {
    test_assert(w.slot(v) == first);
  }
  test_assert(pool.liveSlotCount() == 2); // the empty run plus this one
  test_assert(auditMesh(*m, "weights") == 0);
}

// Overwriting is the funnel's core case: the run being replaced loses its
// reference, and once nothing names it a sweep reclaims it.
static void testOverwriteReleases()
{
  MeshPtr m(3);
  WeightsRef w = ensureVertWeights(*m, "weights");
  DeformPool &pool = m->deformPool();

  auto a = run({{0, 1.0f}});
  auto b = run({{1, 1.0f}});

  const int v0 = *m->v.begin();
  w.setRun(v0, asRun(a));
  const WeightSlot sa = w.slot(v0);

  w.setRun(v0, asRun(b));
  test_assert(w.slot(v0) != sa);
  test_assert(auditMesh(*m, "weights") == 0);

  pool.sweep();
  test_assert(pool.liveSlotCount() == 2); // empty + b; a is gone

  // Writing the same value twice must not double-count.
  w.setRun(v0, asRun(b));
  test_assert(auditMesh(*m, "weights") == 0);

  w.clear(v0);
  test_assert(w.slot(v0).index == 0);
  pool.sweep();
  test_assert(pool.liveSlotCount() == 1);
  test_assert(auditMesh(*m, "weights") == 0);
}

// Nothing releases a slot when its element is freed — the column keeps the dead
// index until the slot is reused. AttrGroup::set_default is where that debt is
// settled, so a kill/make cycle must be reference-neutral.
static void testElementReuse()
{
  MeshPtr m(3);
  WeightsRef w = ensureVertWeights(*m, "weights");
  DeformPool &pool = m->deformPool();

  auto a = run({{4, 1.0f}});
  const int v0 = *m->v.begin();
  w.setRun(v0, asRun(a));
  const WeightSlot sa = w.slot(v0);

  m->kill_vertex(v0);
  // Still named by the freed element, so still live.
  test_assert(w.slot(v0) == sa);
  test_assert(auditMesh(*m, "weights") == 0);

  const int vnew = m->make_vertex(math::float3(0.0f, 0.0f, 0.0f));
  test_assert(vnew == v0); // the freed slot is reused, which is the point
  test_assert(w.slot(vnew).index == 0);
  test_assert(auditMesh(*m, "weights") == 0);

  pool.sweep();
  test_assert(pool.liveSlotCount() == 1);
}

// Dropping the layer drops every reference it held, not just the live elements'.
static void testRemoveLayer()
{
  MeshPtr m(3);
  WeightsRef w = ensureVertWeights(*m, "weights");
  DeformPool &pool = m->deformPool();

  auto a = run({{0, 0.5f}, {2, 0.5f}});
  for (int v : m->v) {
    w.setRun(v, asRun(a));
  }
  test_assert(pool.liveSlotCount() == 2);

  int index = -1;
  for (int i = 0; i < int(m->v.attrs.attrs.size()); i++) {
    if (m->v.attrs.attrs[i].name == string("weights")) {
      index = i;
    }
  }
  test_assert(index >= 0);

  m->v.attrs.remove_attr(index);
  test_assert(!findVertWeights(*m, "weights").exists());

  pool.sweep();
  test_assert(pool.liveSlotCount() == 1);
  test_assert(pool.auditRefcounts(span<const WeightSlot>()) == 0);
}

// A split's dst is a fresh vertex, so the generic "copy src0" rule would install
// src0's index with no reference taken. The registered CUSTOM handler is what
// keeps the column and the pool in agreement across topology edits.
static void testMergeAcrossSplit()
{
  MeshPtr m(3);
  WeightsRef w = ensureVertWeights(*m, "weights");

  auto a = run({{3, 1.0f}});
  for (int v : m->v) {
    w.setRun(v, asRun(a));
  }

  AttrRef &ref = m->v.attrs.ensure(AttrType::WEIGHTS, "weights");
  test_assert(ref.merge == AttrMerge::CUSTOM);
  test_assert(ref.merge_fn != nullptr);

  int edge = ELEM_NONE;
  for (int ei : m->e) {
    if (m->e.c[ei] != ELEM_NONE) {
      edge = ei;
      break;
    }
  }
  test_assert(edge != ELEM_NONE);

  EdgeSplitResult res;
  test_assert(bool(splitEdge(*m, edge, &res)));

  const int vnew = res.new_vert;
  test_assert(vnew != ELEM_NONE);

  // Carried forward, and accounted for.
  test_assert(w.slot(vnew) == w.slot(*m->v.begin()));
  test_assert(w.weight(vnew, 3) == 1.0f);
  test_assert(auditMesh(*m, "weights") == 0);
}

// Three distinct verts of the cube: two interpolation sources and a dst.
static void pickThree(Mesh &m, int &v0, int &v1, int &dst)
{
  auto it = m.v.begin();
  v0 = *it;
  ++it;
  v1 = *it;
  ++it;
  dst = *it;
}

// The handler's value rule: union the two sparse runs, treating a group absent
// from one side as weight 0 there rather than "unchanged", and do not normalize.
static void testMergeInterpolates()
{
  MeshPtr m(3);
  WeightsRef w = ensureVertWeights(*m, "weights");

  int v0, v1, dst;
  pickThree(*m, v0, v1, dst);

  auto a = run({{1, 1.0f}, {2, 0.25f}});
  auto b = run({{2, 0.75f}, {5, 1.0f}});
  w.setRun(v0, asRun(a));
  w.setRun(v1, asRun(b));

  interpAttrs(m->v.attrs, dst, v0, v1, 0.5f, m.m);

  test_assert(w.runSize(dst) == 3);
  test_assert(w.weight(dst, 1) == 0.5f); // only in a: lerp(1, 0)
  test_assert(w.weight(dst, 2) == 0.5f); // in both: lerp(0.25, 0.75)
  test_assert(w.weight(dst, 5) == 0.5f); // only in b: lerp(0, 1)
  // Sums to 1.5. Blender does not renormalize on interpolation and neither does
  // this — silently rescaling a rigged mesh mid-sculpt would be the worse bug.
  test_assert(auditMesh(*m, "weights") == 0);
}

// The endpoints take an existing interned run whole rather than rebuilding it,
// which is both the fast path and what makes a collapse exactly a copy.
static void testMergeEndpoints()
{
  MeshPtr m(3);
  WeightsRef w = ensureVertWeights(*m, "weights");

  int v0, v1, dst;
  pickThree(*m, v0, v1, dst);

  auto a = run({{1, 1.0f}});
  auto b = run({{2, 1.0f}});
  w.setRun(v0, asRun(a));
  w.setRun(v1, asRun(b));

  interpAttrs(m->v.attrs, dst, v0, v1, 0.0f, m.m);
  test_assert(w.slot(dst) == w.slot(v0));

  interpAttrs(m->v.attrs, dst, v0, v1, 1.0f, m.m);
  test_assert(w.slot(dst) == w.slot(v1));

  // A collapse hands the same source twice; the run must survive intact.
  interpAttrs(m->v.attrs, dst, v0, v0, 0.5f, m.m);
  test_assert(w.slot(dst) == w.slot(v0));
  test_assert(auditMesh(*m, "weights") == 0);
}

// Interpolation is transitive, so both bounds have to hold or a run grows
// without limit: sub-epsilon entries are dropped, and the influence count caps
// by magnitude.
static void testMergeBounds()
{
  MeshPtr m(3);
  WeightsRef w = ensureVertWeights(*m, "weights");

  int v0, v1, dst;
  pickThree(*m, v0, v1, dst);

  auto a = run({{1, 1.0f}});
  auto b = run({{7, 1e-7f}});
  w.setRun(v0, asRun(a));
  w.setRun(v1, asRun(b));

  interpAttrs(m->v.attrs, dst, v0, v1, 0.5f, m.m);
  test_assert(w.runSize(dst) == 1);
  test_assert(w.weight(dst, 1) == 0.5f);
  test_assert(w.weight(dst, 7) == 0.0f);

  Vector<DeformWeight> big0, big1;
  for (int i = 0; i < DEFORM_MAX_INFLUENCES; i++) {
    big0.append(DeformWeight{i, 1.0f});
    big1.append(DeformWeight{100 + i, 1.0f});
  }
  w.setRun(v0, asRun(big0));
  w.setRun(v1, asRun(big1));

  interpAttrs(m->v.attrs, dst, v0, v1, 0.5f, m.m);
  test_assert(w.runSize(dst) == DEFORM_MAX_INFLUENCES);
  test_assert(auditMesh(*m, "weights") == 0);
}

// The two producers the design has to survive at once: the merge handler, which
// interns and releases wherever its caller runs, and the meshlog's parallel
// capture, which retains from several threads (meshlog/parallel_capture.h).
// Threads own disjoint elements and their own row store — the engine's own
// discipline — so the pool is the only shared mutable state, which is the point.
// The handler is called directly rather than through interpAttrs: no other
// column claims to be writable from several threads, and dragging them in would
// test a promise nothing makes.
static void testConcurrentMergeAndCapture()
{
  static constexpr int THREADS = 8;
  static constexpr int ITERS = 60;
  static constexpr int TSTEPS = 5;

  MeshPtr m(6);
  WeightsRef w = ensureVertWeights(*m, "weights");

  Vector<int> verts;
  for (int v : m->v) {
    verts.append(v);
  }
  test_assert(int(verts.size()) > THREADS * 4);

  // Seeding every element also materializes every page: the threads below only
  // overwrite cells, and a lazy page allocation under them would be a race the
  // pool could not be blamed for.
  for (int i = 0; i < int(verts.size()); i++) {
    auto seed = run({{i % 6, 1.0f}});
    w.setRun(verts[i], asRun(seed));
  }

  // Two sources every thread reads and none writes.
  const int s0 = verts[0], s1 = verts[1];
  auto a = run({{1, 1.0f}, {2, 0.25f}});
  auto b = run({{2, 0.75f}, {5, 1.0f}});
  w.setRun(s0, asRun(a));
  w.setRun(s1, asRun(b));

  AttrRef &ref = m->v.attrs.ensure(AttrType::WEIGHTS, "weights");
  test_assert(ref.merge_fn != nullptr);
  span<const AttrRef> refs(&ref, 1);

  // t is drawn from one small shared set, so the threads fan in onto the same
  // runs instead of each interning a private family — dedup under contention is
  // half of what is being tested.
  auto tOf = [](int i, int iter) {
    return float((i + iter) % TSTEPS) / float(TSTEPS - 1);
  };

  const int per = (int(verts.size()) - 2) / THREADS;
  std::thread workers[THREADS];
  for (int t = 0; t < THREADS; t++) {
    workers[t] = std::thread([&, t]() {
      const int lo = 2 + t * per;
      const int hi = (t == THREADS - 1) ? int(verts.size()) : lo + per;

      // Born and destroyed on this thread, so its capture retains and its
      // teardown releases both race the other threads' interns.
      meshlog::detail::ChunkElemData store(0, ElemType::VERTEX);

      for (int iter = 0; iter < ITERS; iter++) {
        for (int i = lo; i < hi; i++) {
          AttrMergeCtx ctx;
          ctx.mesh = m.m;
          ctx.grp = &m->v.attrs;
          ctx.dst = verts[i];
          ctx.src0 = s0;
          ctx.src1 = s1;
          ctx.t = tOf(i, iter);
          ref.merge_fn(ref, ctx);
          store.appendFrom(m->v.attrs, verts[i], refs);
        }
      }
    });
  }
  for (int t = 0; t < THREADS; t++) {
    workers[t].join();
  }

  // Every dst holds the last iteration's blend, computed here rather than read
  // back: a lost update would otherwise pass by agreeing with itself.
  for (int i = 2; i < int(verts.size()); i++) {
    const float t = tOf(i, ITERS - 1);
    const int v = verts[i];
    if (t <= 0.0f) {
      test_assert(w.slot(v) == w.slot(s0));
    } else if (t >= 1.0f) {
      test_assert(w.slot(v) == w.slot(s1));
    } else {
      test_assert(w.runSize(v) == 3);
      test_assert(std::fabs(w.weight(v, 1) - (1.0f - t)) < 1e-6f);
      test_assert(std::fabs(w.weight(v, 2) - (0.25f + 0.5f * t)) < 1e-6f);
      test_assert(std::fabs(w.weight(v, 5) - t) < 1e-6f);
    }
  }

  // The stores died with their threads, so the column is the only holder left.
  test_assert(auditMesh(*m, "weights") == 0);
  m->deformPool().sweep();
  test_assert(auditMesh(*m, "weights") == 0);

  // Only the runs the surviving cells name: the two sources, the empty run, and
  // one per interior t. A slot that leaked a reference would still be here.
  test_assert(m->deformPool().liveSlotCount() == size_t(TSTEPS - 2) + 3);
}

// Tearing the mesh down must release everything the column held; the pool dies
// with it, so the only way to see a mistake is to audit just before.
static void testTeardown()
{
  MeshPtr m(3);
  WeightsRef w = ensureVertWeights(*m, "weights");

  auto a = run({{0, 1.0f}});
  auto b = run({{1, 0.5f}, {2, 0.5f}});
  bool flip = false;
  for (int v : m->v) {
    w.setRun(v, flip ? asRun(a) : asRun(b));
    flip = !flip;
  }

  test_assert(m->deformPool().liveSlotCount() == 3);
  test_assert(auditMesh(*m, "weights") == 0);
  // The leak check in test_end() covers the rest: ~MeshBase frees the pool after
  // ~AttrGroup has released into it, so a wrong destruction order faults here.
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  testPoolOwnership();
  testReadWrite();
  testSharing();
  testOverwriteReleases();
  testElementReuse();
  testRemoveLayer();
  testMergeAcrossSplit();
  testMergeInterpolates();
  testMergeEndpoints();
  testMergeBounds();
  testConcurrentMergeAndCapture();
  testTeardown();

  return test_end();
}
