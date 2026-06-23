#pragma once

#ifdef __SANITIZE_ADDRESS__
//#include <sanitizer/asan_interface.h> 
#endif

#include "attribute.h"
#include "litestl/util/assert.h"
#include "litestl/util/boolvector.h"
#include "litestl/util/callback_list.h"
#include "litestl/util/span.h"

#include "mesh_enums.h"

#include <chrono>
#include <cstdio>
#include <utility>

using litestl::util::Assert;

using namespace litestl;
namespace sculptcore::mesh {

/* Phase-0 profiling accumulators (plan: defrag-scoped-compaction.md). reorder()
 * adds to these so applyReorderIncremental can split reorder_X into
 * attribute-permute vs free-structure-rebuild vs (the remainder =) reference-scan.
 * Always accumulate (chrono is cheap); the caller resets + reads them. */
inline double &reorderAttrPermuteMs()
{
  static double v = 0.0;
  return v;
}
inline double &reorderFreeRebuildMs()
{
  static double v = 0.0;
  return v;
}
struct Mesh;

struct ElemData {
  ElemType domain;
  AttrGroup attrs;
  int count = 0;

  util::BoolVector<> freemap;
  util::CallbackList<void(Mesh *m, int v1, int v2)> on_swap;

  static binding::types::Struct<ElemData> *defineBindings()
  {
    using binding::types::Struct;
    Struct<ElemData> *st =
        new Struct<ElemData>("sculptcore::mesh::ElemData", sizeof(ElemData));

    BIND_STRUCT_MEMBER(st, attrs);
    BIND_STRUCT_MEMBER(st, capacity_);
    BIND_STRUCT_MEMBER(st, count);
    BIND_STRUCT_METHOD_SIG(st, alloc, void, MARGS("count", "clear"), (int, bool));
    BIND_STRUCT_METHOD_SIG(st, alloc, int, MARGS(), (void));

    return st;
  }

  struct iterator {
    iterator(ElemData &owner, int i) : owner_(owner), i_(i)
    {
      if (i == 0) { /* Skip leading free slots so begin() lands on the first
                     * live element (which may be element 0 itself). */
        while (i_ < owner_.capacity_ && owner_.freemap[i_]) {
          i_++;
        }
      }
    }

    iterator(const iterator &b) : owner_(b.owner_), i_(b.i_)
    {
    }

    bool operator==(const iterator &b)
    {
      return b.i_ == i_;
    }

    bool operator!=(const iterator &b)
    {
      return !operator==(b);
    }

    int operator*()
    {
      return i_;
    }

    iterator &operator++()
    {
      i_++;

      while (i_ < owner_.capacity_ && owner_.freemap[i_]) {
        i_++;
      }

      return *this;
    }

  private:
    ElemData &owner_;
    int i_;
  };

  friend struct iterator;

  iterator begin()
  {
    return iterator(*this, 0);
  }

  iterator end()
  {
    return iterator(*this, capacity_);
  }

  void alloc(int freed_elem, bool alloc_attrs = true)
  {
    Assert(freemap[freed_elem], "freed_elem is actually freed");

    // Leave the stale entry in its page bucket; pop_free_in_page skips it.
    freemap.set(freed_elem, false);
    free_count--;

    if (alloc_attrs) {
      attrs.set_default(freed_elem);
    }

    count++;
  }

  /* Allocate a new elem */
  int alloc()
  {
    int i = pop_any_free();
    if (i == ELEM_NONE) {
      add_page();
      i = pop_any_free();
    }

    freemap.set(i, false);
    free_count--;
    count++;

    attrs.set_default(i);

    return i;
  }

  /* Allocate a new elem, preferring a free slot in @p hint_elem's page so the
   * new element is spatially local to its neighbor in DRAM. hint_elem ==
   * ELEM_NONE (or its page being full) falls back to alloc(). */
  int alloc_near(int hint_elem)
  {
    if (hint_elem != ELEM_NONE) {
      int p = hint_elem >> ATTR_PAGESHIFT;
      if (p >= 0 && p < int(page_free.size())) {
        int i = pop_free_in_page(p);
        if (i != ELEM_NONE) {
          freemap.set(i, false);
          free_count--;
          count++;
          attrs.set_default(i);
          return i;
        }
      }
    }
    return alloc();
  }

  /* Free an elem */
  void release(int elem)
  {
    freemap.set(elem, true);
    count--;
    free_count++;

    int p = elem >> ATTR_PAGESHIFT;
    page_free[p].append(elem);
    if (!page_on_stack[p]) {
      nonempty_pages.append(p);
      page_on_stack.set(p, true);
    }
  }

  /* Number of currently-free slots within capacity. */
  int free_slots()
  {
    return free_count;
  }

  /* Drop trailing pages that are entirely free, freeing their attribute storage
   * (the bulk DRAM). Reclaims memory after the live set shrinks + compacts to the
   * front (mechanism B). Returns the number of pages freed. capacity_ stays
   * page-aligned, so this only ever removes whole pages. */
  int free_trailing_pages()
  {
    int start_cap = capacity_;
    while (capacity_ >= ATTR_PAGESIZE) {
      int page_start = capacity_ - ATTR_PAGESIZE;
      bool all_free = true;
      for (int i = page_start; i < capacity_; i++) {
        if (!freemap[i]) {
          all_free = false;
          break;
        }
      }
      if (!all_free) {
        break;
      }
      capacity_ = page_start;
    }

    int freed = (start_cap - capacity_) >> ATTR_PAGESHIFT;
    if (freed == 0) {
      return 0;
    }

    attrs.shrink_capacity(capacity_);
    freemap.resize(capacity_);
    rebuild_free_structures();
    return freed;
  }

  ElemData(ElemType domain_, int count_)
      : domain(domain_), count(count_), capacity_(count)
  {
    attrs.ensure_capacity(count);
    ensure_page_buckets();
  }

  size_t capacity()
  {
    return capacity_;
  }

  void swap_attrs(int a, int b)
  {
    attrs.swap(a, b);
  }

  /* Permute element storage by @p map (map[old] = new), covering the full
   * [0, capacity) range — the caller must supply a complete bijection,
   * including the free slots. Reorders attribute data and rebuilds the
   * free-slot bookkeeping so holes follow their mapped positions. Reference
   * fields pointing at this domain are the caller's responsibility (see
   * Mesh::reorder_*). */
  void reorder(util::span<int> map)
  {
    using Clock = std::chrono::steady_clock;
    auto a0 = Clock::now();
    attrs.reorder(map);
    auto a1 = Clock::now();

    util::BoolVector<> newfree;
    newfree.resize(capacity_);
    for (int i = 0; i < capacity_; i++) {
      newfree.set(map[i], freemap[i]);
    }
    freemap = std::move(newfree);

    rebuild_free_structures();
    auto a2 = Clock::now();

    reorderAttrPermuteMs() += std::chrono::duration<double, std::milli>(a1 - a0).count();
    reorderFreeRebuildMs() += std::chrono::duration<double, std::milli>(a2 - a1).count();
  }

  /* Scoped variant of reorder(): only @p movedSlots move, as a closed permutation
   * over LIVE slots (every moved slot and its destination is live). That makes the
   * freemap invariant — so we permute only the touched attribute data (O(moved))
   * and skip the O(capacity) freemap rebuild entirely. */
  void reorderScoped(util::span<int> map, util::span<int> movedSlots)
  {
    using Clock = std::chrono::steady_clock;
    for (int s : movedSlots) {
      Assert(!freemap[s], "scoped reorder requires live slots");
    }
    auto a0 = Clock::now();
    attrs.reorderScoped(map, movedSlots);
    auto a1 = Clock::now();
    reorderAttrPermuteMs() += std::chrono::duration<double, std::milli>(a1 - a0).count();
  }

private:
  // Free slots are bucketed by page so alloc_near(hint) can reuse a hole in the
  // hint's page. freemap stays authoritative; page_free / nonempty_pages may
  // carry stale entries (from explicit-index alloc), skipped on pop via freemap.
  util::Vector<util::Vector<int>> page_free;
  util::Vector<int> nonempty_pages;
  util::BoolVector<> page_on_stack;
  int free_count = 0;

  void ensure_page_buckets()
  {
    int npages = int((capacity_ + ATTR_PAGESIZE - 1) >> ATTR_PAGESHIFT);
    while (int(page_free.size()) < npages) {
      page_free.append(util::Vector<int>());
    }
    if (int(page_on_stack.size()) < npages) {
      page_on_stack.resize(npages);
    }
  }

  /* Pop a genuinely-free slot from page @p (skipping stale entries left by the
   * explicit-index alloc); ELEM_NONE if the page has none. The caller claims it
   * (clears freemap, decrements free_count). */
  int pop_free_in_page(int p)
  {
    util::Vector<int> &bucket = page_free[p];
    while (bucket.size() > 0) {
      int i = bucket.pop_back();
      if (freemap[i]) {
        return i;
      }
    }
    return ELEM_NONE;
  }

  int pop_any_free()
  {
    while (nonempty_pages.size() > 0) {
      int p = nonempty_pages.last();
      int i = pop_free_in_page(p);
      if (i != ELEM_NONE) {
        return i;
      }
      nonempty_pages.pop_back();
      page_on_stack.set(p, false);
    }
    return ELEM_NONE;
  }

  void rebuild_free_structures()
  {
    int npages = int((capacity_ + ATTR_PAGESIZE - 1) >> ATTR_PAGESHIFT);

    // Reuse inner-bucket heap: clear each bucket in place and only grow/shrink
    // the outer vector, instead of destroy+realloc-ing every page bucket (the
    // same allocation-churn fix as the node-cache remap).
    while (int(page_free.size()) > npages) {
      page_free.pop_back();
    }
    for (util::Vector<int> &bucket : page_free) {
      bucket.clear();
    }
    while (int(page_free.size()) < npages) {
      page_free.append(util::Vector<int>());
    }

    nonempty_pages.clear();
    page_on_stack = util::BoolVector<>();
    page_on_stack.resize(npages);
    free_count = 0;

    for (int i = capacity_ - 1; i >= 0; i--) {
      if (!freemap[i]) {
        continue;
      }
      int p = i >> ATTR_PAGESHIFT;
      page_free[p].append(i);
      free_count++;
      if (!page_on_stack[p]) {
        nonempty_pages.append(p);
        page_on_stack.set(p, true);
      }
    }
  }

  void add_page()
  {
    int start = capacity_;
    capacity_ += ATTR_PAGESIZE;
    int p = int(start >> ATTR_PAGESHIFT);

    freemap.resize(capacity_);
    ensure_page_buckets();

    util::Vector<int> &bucket = page_free[p];
    for (int i = ATTR_PAGESIZE - 1; i >= 0; i--) {
      int idx = start + i;
      bucket.append(idx);
      freemap.set(idx, true);
    }
    free_count += ATTR_PAGESIZE;
    if (!page_on_stack[p]) {
      nonempty_pages.append(p);
      page_on_stack.set(p, true);
    }

    attrs.ensure_capacity(capacity_);
  }

protected:
  int capacity_ = 0;
};
} // namespace sculptcore::mesh
