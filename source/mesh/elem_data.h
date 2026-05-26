#pragma once

#include "attribute.h"
#include "litestl/util/assert.h"
#include "litestl/util/boolvector.h"
#include "litestl/util/callback_list.h"
#include "litestl/util/span.h"

#include "mesh_enums.h"

#include <cstdio>
#include <utility>

using litestl::util::Assert;

using namespace litestl;
namespace sculptcore::mesh {
struct Mesh;

struct ElemData {
  ElemType domain;
  AttrGroup attrs;
  int count = 0;

  util::BoolVector<> freemap;
  util::Vector<int> freelist;
  util::CallbackList<void(Mesh *m, int v1, int v2)> on_swap;

  static binding::types::Struct<ElemData> *defineBindings()
  {
    using binding::types::Struct;
    Struct<ElemData> *st =
        new Struct<ElemData>("sculptcore::mesh::ElemData", sizeof(ElemData));

    BIND_STRUCT_MEMBER(st, attrs);
    BIND_STRUCT_MEMBER(st, capacity_);
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

    freelist.remove(freed_elem);
    freemap.set(freed_elem, false);

    if (alloc_attrs) {
      attrs.set_default(freed_elem);
    }

    count++;
  }

  /* Allocate a new elem */
  int alloc()
  {
    int i;

    if (freelist.size() > 0) {
      i = freelist.pop_back();
    } else {
      add_page();
      return alloc();
    }

    freemap.set(i, false);
    count++;

    attrs.set_default(i);

    return i;
  }

  /* Free an elem */
  void release(int elem)
  {
    freemap.set(elem, true);
    freelist.append(elem);
    count--;
  }

  ElemData(ElemType domain_, int count_)
      : domain(domain_), count(count_), capacity_(count)
  {
    attrs.ensure_capacity(count);
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
    attrs.reorder(map);

    util::BoolVector<> newfree;
    newfree.resize(capacity_);
    for (int i = 0; i < capacity_; i++) {
      newfree.set(map[i], freemap[i]);
    }
    freemap = std::move(newfree);

    freelist.clear();
    for (int i = capacity_ - 1; i >= 0; i--) {
      if (freemap[i]) {
        freelist.append(i);
      }
    }
  }

private:
  void add_page()
  {
    int start = capacity_;
    capacity_ += ATTR_PAGESIZE;

    freemap.resize(capacity_);

    for (int i = ATTR_PAGESIZE - 1; i >= 0; i--) {
      freelist.append(start + i);
      freemap.set(start + i, true);
    }

    attrs.ensure_capacity(capacity_);
  }

protected:
  int capacity_ = 0;
};
} // namespace sculptcore::mesh
