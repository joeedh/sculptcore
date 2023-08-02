#pragma once

#include "attribute.h"
#include "attribute_builtin.h"
#include "math/vector.h"
#include "util/boolvector.h"
#include "util/string.h"

#include "mesh_base.h"
#include "mesh_enums.h"

#include <algorithm>
#include <concepts>
#include <cstdio>
#include <type_traits>

namespace sculptcore::mesh {

struct ElemData {
  ElemType domain;
  AttrGroup attrs;
  int count = 0;

  util::BoolVector<> freemap;
  util::Vector<int> freelist;

  struct iterator {
    iterator(ElemData &owner, int i) : owner_(owner), i_(i)
    {
      if (i == 0) { /* Find first element. */
        i--;
        operator++();
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

  int capacity_ = 0;
};
} // namespace sculptcore::mesh
