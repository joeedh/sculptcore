/**
 * The deform-weight pool (mesh/deform_pool.h): the interned, refcounted side
 * table an AttrType::WEIGHTS column indexes into.
 *
 * The properties worth pinning are the ones the rest of the design leans on —
 * that equal weight sets always land on one slot (so a column element can be
 * compared by index), that a slot lives exactly as long as its references (so
 * meshlog and mesh columns can name it freely), and that both hold under
 * concurrent interning.
 */

#include "test_util.h"

#include "mesh/deform_pool.h"

#include "litestl/util/span.h"
#include "litestl/util/vector.h"

#include <cstdio>
#include <thread>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using litestl::util::span;
using litestl::util::Vector;

static span<const DeformWeight> asRun(const Vector<DeformWeight> &v)
{
  return span<const DeformWeight>(const_cast<Vector<DeformWeight> &>(v).data(), v.size());
}

static Vector<DeformWeight> run(std::initializer_list<DeformWeight> ws)
{
  Vector<DeformWeight> v;
  for (const DeformWeight &w : ws) {
    v.append(w);
  }
  return v;
}

static bool runIs(const DeformPool &pool, WeightSlot slot, std::initializer_list<DeformWeight> ws)
{
  DeformWeight buf[16];
  const int n = pool.copyRun(slot, buf, 16);
  if (n != int(ws.size())) {
    return false;
  }
  int i = 0;
  for (const DeformWeight &w : ws) {
    if (buf[i] != w) {
      return false;
    }
    i++;
  }
  return true;
}

// Every spelling of a weight set — unsorted, duplicated, padded with zeros —
// must reduce to one slot, because equality of two columns' weights is decided
// by comparing indices.
static void testCanonicalization()
{
  DeformPool pool;

  auto a = run({{2, 0.25f}, {0, 0.75f}});
  auto b = run({{0, 0.5f}, {2, 0.25f}, {0, 0.25f}});
  auto c = run({{0, 0.75f}, {1, 0.0f}, {2, 0.25f}});

  WeightSlot sa = pool.intern(asRun(a));
  WeightSlot sb = pool.intern(asRun(b));
  WeightSlot sc = pool.intern(asRun(c));

  test_assert(sa == sb);
  test_assert(sa == sc);
  test_assert(runIs(pool, sa, {{0, 0.75f}, {2, 0.25f}}));

  // A run that cancels to nothing is the empty run, not a two-entry one.
  auto cancels = run({{3, 0.5f}, {3, -0.5f}});
  test_assert(pool.intern(asRun(cancels)).index == 0);

  pool.release(sa);
  pool.release(sb);
  pool.release(sc);
}

static void testEmptySlot()
{
  DeformPool pool;

  WeightSlot empty = pool.intern(span<const DeformWeight>());
  test_assert(empty.index == 0);
  test_assert(pool.runSize(empty) == 0);
  test_assert(pool.weight(empty, 0) == 0.0f);
  test_assert(pool.liveSlotCount() == 1);

  // Immortal: releasing the empty slot, however often, must not kill it.
  for (int i = 0; i < 4; i++) {
    pool.release(empty);
  }
  pool.sweep();
  test_assert(pool.liveSlotCount() == 1);
  test_assert(pool.intern(span<const DeformWeight>()).index == 0);
}

static void testLookup()
{
  DeformPool pool;

  auto a = run({{5, 0.25f}, {1, 0.5f}, {9, 0.25f}});
  WeightSlot sa = pool.intern(asRun(a));

  test_assert(pool.runSize(sa) == 3);
  test_assert(pool.weight(sa, 1) == 0.5f);
  test_assert(pool.weight(sa, 5) == 0.25f);
  test_assert(pool.weight(sa, 9) == 0.25f);
  test_assert(pool.weight(sa, 0) == 0.0f);
  test_assert(pool.weight(sa, 7) == 0.0f);
  test_assert(pool.weight(sa, 100) == 0.0f);

  // Undersized destination: nothing is written past `max`, but the caller
  // still learns the true size so it can retry.
  DeformWeight buf[3] = {{-1, -1.0f}, {-1, -1.0f}, {-1, -1.0f}};
  test_assert(pool.copyRun(sa, buf, 2) == 3);
  test_assert((buf[0] == DeformWeight{1, 0.5f}));
  test_assert((buf[1] == DeformWeight{5, 0.25f}));
  test_assert((buf[2] == DeformWeight{-1, -1.0f}));

  pool.release(sa);
}

static void testRefcounting()
{
  DeformPool pool;

  auto a = run({{0, 1.0f}});
  auto b = run({{1, 1.0f}});

  WeightSlot sa0 = pool.intern(asRun(a));
  WeightSlot sa1 = pool.intern(asRun(a));
  WeightSlot sb = pool.intern(asRun(b));

  test_assert(sa0 == sa1);
  test_assert(sa0 != sb);
  test_assert(pool.liveSlotCount() == 3); // empty + a + b

  pool.release(sa0);
  test_assert(pool.liveSlotCount() == 3); // sa1 still holds a reference
  test_assert(runIs(pool, sa1, {{0, 1.0f}}));

  pool.release(sa1);
  test_assert(pool.liveSlotCount() == 2);

  // Dead but not yet swept: re-interning the same run resurrects the slot it
  // had, so any index still sitting in an undo step keeps its meaning.
  WeightSlot again = pool.intern(asRun(a));
  test_assert(again == sa0);
  test_assert(pool.liveSlotCount() == 3);
  test_assert(runIs(pool, again, {{0, 1.0f}}));

  pool.release(again);
  pool.release(sb);
}

static void testReassign()
{
  DeformPool pool;

  auto a = run({{0, 1.0f}});
  auto b = run({{1, 1.0f}});

  WeightSlot cell;
  pool.reassign(cell, pool.intern(asRun(a)));
  pool.release(cell); // drop the intern's reference; the cell holds one now
  test_assert(runIs(pool, cell, {{0, 1.0f}}));

  WeightSlot sb = pool.intern(asRun(b));
  pool.reassign(cell, sb);
  pool.release(sb);
  test_assert(runIs(pool, cell, {{1, 1.0f}}));
  test_assert(pool.liveSlotCount() == 2); // empty + b; a was released

  pool.reassign(cell, WeightSlot(0));
  test_assert(pool.liveSlotCount() == 1);
}

// Sweeping compacts the arenas but must not renumber slots — a WEIGHTS column
// or a meshlog row holds bare indices with no way to be told they moved.
static void testSweepKeepsIndices()
{
  DeformPool pool;

  Vector<WeightSlot> kept;
  Vector<WeightSlot> doomed;
  for (int i = 0; i < 64; i++) {
    auto keep = run({{i, 0.5f}, {i + 1000, 0.5f}});
    auto drop = run({{i, 0.25f}, {i + 2000, 0.75f}});
    kept.append(pool.intern(asRun(keep)));
    doomed.append(pool.intern(asRun(drop)));
  }

  const size_t live_before = pool.liveSlotCount();
  const size_t bytes_before = pool.byteSize();

  for (const WeightSlot &s : doomed) {
    pool.release(s);
  }
  pool.sweep();

  test_assert(pool.liveSlotCount() == live_before - 64);
  test_assert(pool.byteSize() < bytes_before);

  for (int i = 0; i < 64; i++) {
    test_assert(runIs(pool, kept[i], {{i, 0.5f}, {i + 1000, 0.5f}}));
  }

  // The intern table survived the rebuild: an equal run still dedups.
  auto probe = run({{7, 0.5f}, {1007, 0.5f}});
  WeightSlot again = pool.intern(asRun(probe));
  test_assert(again == kept[7]);
  pool.release(again);

  for (const WeightSlot &s : kept) {
    pool.release(s);
  }
}

// The audit is the tool that catches a WEIGHTS write which bypassed the
// retain/release funnel, so it has to be able to fail.
static void testAudit()
{
  DeformPool pool;

  Vector<WeightSlot> roots;
  for (int i = 0; i < 16; i++) {
    auto r = run({{i, 1.0f}});
    roots.append(pool.intern(asRun(r)));
    if (i % 3 == 0) {
      roots.append(pool.intern(asRun(r))); // a second holder of the same run
    }
  }

  test_assert(pool.auditRefcounts(span<const WeightSlot>(roots.data(), roots.size())) == 0);

  // A reference the pool thinks exists but no column names — what a leaked
  // retain looks like.
  Vector<WeightSlot> short_roots;
  for (size_t i = 1; i < roots.size(); i++) {
    short_roots.append(roots[i]);
  }
  test_assert(
      pool.auditRefcounts(span<const WeightSlot>(short_roots.data(), short_roots.size())) == 1);

  for (const WeightSlot &s : roots) {
    pool.release(s);
  }
}

static void testCopy()
{
  DeformPool pool;
  pool.group_names.append("Group");
  pool.group_names.append("Group.001");

  auto a = run({{0, 0.5f}, {1, 0.5f}});
  WeightSlot sa = pool.intern(asRun(a));

  DeformPool copy(pool);
  test_assert(copy.group_names.size() == 2);
  test_assert(runIs(copy, sa, {{0, 0.5f}, {1, 0.5f}}));
  test_assert(copy.liveSlotCount() == pool.liveSlotCount());

  // The copy's refcounts are the original's, so the copied columns'
  // references are already accounted for.
  Vector<WeightSlot> roots;
  roots.append(sa);
  test_assert(copy.auditRefcounts(span<const WeightSlot>(roots.data(), roots.size())) == 0);

  DeformPool assigned;
  assigned = pool;
  test_assert(runIs(assigned, sa, {{0, 0.5f}, {1, 0.5f}}));

  pool.release(sa);
  copy.release(sa);
  assigned.release(sa);
}

// The meshlog's parallel capture already fills rows from several threads, and
// the merge interpolator runs wherever its caller does. Interning has to be
// safe under that from the start, not after the first crash.
static void testConcurrentIntern()
{
  static constexpr int THREADS = 8;
  static constexpr int RUNS = 96;
  static constexpr int ITERS = 400;

  DeformPool pool;
  Vector<WeightSlot> got[THREADS];

  std::thread workers[THREADS];
  for (int t = 0; t < THREADS; t++) {
    workers[t] = std::thread([&pool, &got, t]() {
      for (int i = 0; i < ITERS; i++) {
        const int k = (i * 7 + t * 13) % RUNS;
        Vector<DeformWeight> r;
        r.append({k, 0.5f});
        r.append({k + 500, 0.25f});
        r.append({k + 900, 0.25f});
        got[t].append(pool.intern(asRun(r)));
      }
    });
  }
  for (int t = 0; t < THREADS; t++) {
    workers[t].join();
  }

  // Interning is a function of the run, not of the thread that got there
  // first: every thread must hold the same slot for the same weights.
  Vector<WeightSlot> by_run;
  by_run.resize(RUNS);
  for (int i = 0; i < RUNS; i++) {
    by_run[i] = WeightSlot(-1);
  }

  Vector<WeightSlot> roots;
  for (int t = 0; t < THREADS; t++) {
    test_assert(int(got[t].size()) == ITERS);
    for (int i = 0; i < ITERS; i++) {
      const int k = (i * 7 + t * 13) % RUNS;
      const WeightSlot s = got[t][i];
      if (by_run[k].index == -1) {
        by_run[k] = s;
      }
      test_assert(by_run[k] == s);
      test_assert(runIs(pool, s, {{k, 0.5f}, {k + 500, 0.25f}, {k + 900, 0.25f}}));
      roots.append(s);
    }
  }

  test_assert(pool.liveSlotCount() == size_t(RUNS) + 1);

  // No lost or double increment anywhere in the fan-in.
  test_assert(pool.auditRefcounts(span<const WeightSlot>(roots.data(), roots.size())) == 0);

  for (const WeightSlot &s : roots) {
    pool.release(s);
  }
  pool.sweep();
  test_assert(pool.liveSlotCount() == 1);
}

// Concurrent release-to-zero racing intern of the same run: the window the
// shard lock exists to close. Either order is fine, an index that resolves to
// the wrong run is not.
static void testConcurrentChurn()
{
  static constexpr int THREADS = 8;
  static constexpr int ITERS = 500;

  DeformPool pool;
  std::thread workers[THREADS];

  for (int t = 0; t < THREADS; t++) {
    workers[t] = std::thread([&pool, t]() {
      for (int i = 0; i < ITERS; i++) {
        const int k = (i + t) % 12;
        Vector<DeformWeight> r;
        r.append({k, 1.0f});
        WeightSlot s = pool.intern(asRun(r));

        DeformWeight buf[4];
        const int n = pool.copyRun(s, buf, 4);
        test_assert(n == 1);
        test_assert((buf[0] == DeformWeight{k, 1.0f}));

        pool.release(s);
      }
    });
  }
  for (int t = 0; t < THREADS; t++) {
    workers[t].join();
  }

  pool.sweep();
  test_assert(pool.liveSlotCount() == 1);
  test_assert(pool.auditRefcounts(span<const WeightSlot>()) == 0);
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  testCanonicalization();
  testEmptySlot();
  testLookup();
  testRefcounting();
  testReassign();
  testSweepKeepsIndices();
  testAudit();
  testCopy();
  testConcurrentIntern();
  testConcurrentChurn();

  printf("deform_attr test done\n");
  return test_end();
}
