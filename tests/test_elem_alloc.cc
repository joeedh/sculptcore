#include "test_util.h"

#include "litestl/util/rand.h"
#include "litestl/util/vector.h"
#include "mesh/elem_data.h"

#include <cstdint>
#include <cstdio>

test_init;

/* Local assert that flips retval on failure (shared test_assert keeps
 * retval=0 on the failure branch). */
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
using namespace litestl;
using namespace litestl::util;

namespace {

int countFree(ElemData &ed)
{
  int n = 0;
  for (int i = 0; i < int(ed.capacity()); i++) {
    if (ed.freemap[i]) {
      n++;
    }
  }
  return n;
}

/* model.live[i] must equal !freemap[i] across the whole capacity, and the
 * tracked free_slots() count must match a brute-force scan. */
bool checkConsistency(ElemData &ed, Vector<bool> &live, const char *tag)
{
  for (int i = 0; i < int(ed.capacity()); i++) {
    bool modelLive = i < int(live.size()) ? live[i] : false;
    if (modelLive == ed.freemap[i]) {
      fprintf(stderr,
              "[%s] slot %d: model live=%d freemap=%d mismatch\n",
              tag,
              i,
              int(modelLive),
              int(ed.freemap[i]));
      return false;
    }
  }
  if (ed.free_slots() != countFree(ed)) {
    fprintf(stderr,
            "[%s] free_slots()=%d != scanned %d\n",
            tag,
            ed.free_slots(),
            countFree(ed));
    return false;
  }
  return true;
}

/* alloc_near must place a new element in the hint's page when that page has a
 * hole, and fall back cleanly when it is full. */
void test_page_locality()
{
  const char *tag = "locality";
  const int shift = ATTR_PAGESHIFT;

  ElemData ed(VERTEX, 0);
  for (int i = 0; i < 5000; i++) {
    ed.alloc();
  }
  TASSERT(ed.capacity() >= 2 * ATTR_PAGESIZE); // spilled into a 2nd page

  int p0slot = 100, p1slot = 4500;
  TASSERT((p0slot >> shift) == 0 && (p1slot >> shift) == 1);
  ed.release(p0slot);
  ed.release(p1slot);

  int a = ed.alloc_near(50); // hint in page 0
  TASSERT((a >> shift) == 0);
  int b = ed.alloc_near(4600); // hint in page 1
  TASSERT((b >> shift) == 1);

  // page 0 is full again now; the hint can't be honored, fall back to any free.
  int c = ed.alloc_near(50);
  TASSERT(!ed.freemap[c]);

  Vector<bool> live; // reconstruct liveness lazily for the consistency scan
  live.resize(ed.capacity());
  for (int i = 0; i < int(ed.capacity()); i++) {
    live[i] = !ed.freemap[i];
  }
  TASSERT(checkConsistency(ed, live, tag));
}

/* Randomized alloc / alloc_near / release churn against a reference model. The
 * load-bearing invariant is that a returned slot was free immediately before —
 * i.e. no slot is ever double-allocated. */
void test_random_churn(uint32_t seed, bool useHints)
{
  char tag[64];
  snprintf(tag, sizeof(tag), "churn-s%u-h%d", seed, int(useHints));

  ElemData ed(VERTEX, 0);
  Vector<bool> live;
  Vector<int> liveList;
  Random rnd(seed);

  for (int iter = 0; iter < 20000; iter++) {
    bool doAlloc = liveList.size() == 0 || (rnd.get_int() % 100) < 60;

    if (doAlloc) {
      int idx;
      if (useHints && liveList.size() > 0 && (rnd.get_int() & 1)) {
        int hint = liveList[rnd.get_int() % uint32_t(liveList.size())];
        idx = ed.alloc_near(hint);
      } else {
        idx = ed.alloc();
      }

      while (int(live.size()) <= idx) {
        live.append(false);
      }
      TASSERT(!live[idx]); // double-allocation guard
      live[idx] = true;
      liveList.append(idx);
    } else {
      int li = int(rnd.get_int() % uint32_t(liveList.size()));
      int idx = liveList[li];
      liveList[li] = liveList[liveList.size() - 1];
      liveList.pop_back();
      TASSERT(live[idx]);
      live[idx] = false;
      ed.release(idx);
    }

    if ((iter & 255) == 0) {
      TASSERT(checkConsistency(ed, live, tag));
    }
  }

  TASSERT(checkConsistency(ed, live, tag));
  printf("[%s] cap=%d live=%d free=%d\n",
         tag,
         int(ed.capacity()),
         int(liveList.size()),
         ed.free_slots());
}

/* The explicit-index alloc(i, false) leaves a stale entry in i's page bucket;
 * later allocations must skip it and never hand i back while it is live. */
void test_explicit_alloc_stale()
{
  const char *tag = "explicit-stale";
  ElemData ed(VERTEX, 0);
  for (int i = 0; i < 10; i++) {
    ed.alloc();
  }

  ed.release(5);
  TASSERT(ed.freemap[5]);
  ed.alloc(5, false); // explicit reclaim, leaves stale bucket entry for slot 5
  TASSERT(!ed.freemap[5]);

  for (int i = 0; i < 64; i++) {
    int x = ed.alloc();
    TASSERT(x != 5);
  }

  Vector<bool> live;
  live.resize(ed.capacity());
  for (int i = 0; i < int(ed.capacity()); i++) {
    live[i] = !ed.freemap[i];
  }
  TASSERT(checkConsistency(ed, live, tag));
}

/* free_trailing_pages reclaims only whole trailing pages that are entirely free,
 * keeps the free-list bookkeeping consistent, and leaves mid-array holes alone. */
void test_free_trailing_pages()
{
  const char *tag = "free-trailing";
  ElemData ed(VERTEX, 0);

  // Fill exactly three pages (alloc returns 0,1,2,... so ids == indices).
  for (int i = 0; i < 3 * ATTR_PAGESIZE; i++) {
    ed.alloc();
  }
  TASSERT(ed.capacity() == size_t(3 * ATTR_PAGESIZE));
  TASSERT(ed.free_slots() == 0);

  // Free the whole last page -> free_trailing_pages drops exactly one page.
  for (int i = 2 * ATTR_PAGESIZE; i < 3 * ATTR_PAGESIZE; i++) {
    ed.release(i);
  }
  TASSERT(ed.free_trailing_pages() == 1);
  TASSERT(ed.capacity() == size_t(2 * ATTR_PAGESIZE));
  TASSERT(ed.free_slots() == 0);

  // A mid-array hole does not make the trailing page free -> reclaims nothing.
  ed.release(100);
  TASSERT(ed.free_trailing_pages() == 0);
  TASSERT(ed.capacity() == size_t(2 * ATTR_PAGESIZE));

  // Allocation still works post-shrink and reuses the lone hole.
  int r = ed.alloc();
  TASSERT(r == 100);

  Vector<bool> live;
  live.resize(ed.capacity());
  for (int i = 0; i < int(ed.capacity()); i++) {
    live[i] = !ed.freemap[i];
  }
  TASSERT(checkConsistency(ed, live, tag));

  // Free both remaining pages' worth -> capacity collapses to 0, still usable.
  for (int i = 0; i < 2 * ATTR_PAGESIZE; i++) {
    if (ed.freemap[i]) {
      continue;
    }
    ed.release(i);
  }
  TASSERT(ed.free_trailing_pages() == 2);
  TASSERT(ed.capacity() == 0);
  int r2 = ed.alloc(); // must grow a fresh page and hand back a valid slot
  TASSERT(r2 >= 0 && r2 < int(ed.capacity()));
}

} // namespace

int main()
{
  test_page_locality();
  test_explicit_alloc_stale();
  test_free_trailing_pages();

  for (uint32_t trial = 0; trial < 6; trial++) {
    test_random_churn(0x1000u + trial * 911u, false);
    test_random_churn(0x2000u + trial * 911u, true);
  }

  printf("elem_alloc test done\n");
  return retval;
}
