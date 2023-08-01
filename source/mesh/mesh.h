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

    for (int i = 0; i < ATTR_PAGESIZE; i++) {
      freelist.append(start + i);
      freemap.set(start + i, true);
    }

    attrs.ensure_capacity(capacity_);
  }

  int capacity_ = 0;
};

struct VertexData : public ElemData {
  VertexData(int count_) : ElemData(VERTEX, count_)
  {
    co.ensure(attrs);
    select.ensure(attrs);
    no.ensure(attrs);
    e.ensure(attrs);

    for (int i = 0; i < count_; i++) {
      e[i] = ELEM_NONE;
    }
  }

  BuiltinAttr<math::float3, "positions"> co;
  BuiltinAttr<math::float3, "normals"> no;

  BuiltinAttr<bool, "select"> select;

  /* Topology attributes. */
  BuiltinAttr<int, "e"> e;
  BuiltinAttr<int, "l"> l;
};

struct EdgeData : public ElemData {
  using int2 = math::int2;
  using int4 = math::int4;

  EdgeData(int count_) : ElemData(EDGE, count_)
  {
    vs.ensure(attrs);
    disk.ensure(attrs);
    select.ensure(attrs);
  }

  BuiltinAttr<bool, "select"> select;

  /* Topology attributes. */
  BuiltinAttr<int2, ".edge.vs"> vs;
  BuiltinAttr<int4, ".edge.vs.disk"> disk;
};

struct Mesh {
  VertexData v;
  EdgeData e;

  struct VertProxy;

  struct EdgeOfVertIter {
    inline EdgeOfVertIter(Mesh *m_, int v_, int e_) : m(m_), v(v_), e(e_), start_e(e_)
    {
      e = start_e = m->v.e[v];
    }

    inline EdgeOfVertIter(const EdgeOfVertIter &b)
        : m(b.m), v(b.v), e(b.e), start_e(b.start_e)
    {
    }

    inline int operator*()
    {
      return e;
    }

    inline bool operator==(const EdgeOfVertIter &b)
    {
      return b.e == e;
    }

    inline bool operator!=(const EdgeOfVertIter &b)
    {
      return !(operator==(b));
    }

    EdgeOfVertIter &operator++()
    {
      if (e == start_e) {
        if (first) {
          first = false;
        } else {
          /* Flag endpoint. */
          e = ELEM_NONE;
          return *this;
        }
      }

      int side = m->e.vs[e][0] == v ? 0 : 1;
      e = m->e.disk[e][side * 2 + 1]; /* e.next */

      return *this;
    }

    EdgeOfVertIter begin()
    {
      return *this;
    }

    EdgeOfVertIter end()
    {
      return EdgeOfVertIter(m, v, ELEM_NONE);
    }

  private:
    Mesh *m;
    int v, e, start_e;
    bool first = true;
  };

  struct EdgeProxy {
    int e;

    EdgeProxy(Mesh *m_, int e_) : m(m_), e(e_)
    {
    }

    VertProxy v1()
    {
      return VertProxy(m, m->e.vs[e][0]);
    }

    VertProxy v2()
    {
      return VertProxy(m, m->e.vs[e][1]);
    }

  private:
    Mesh *m;
  };

  struct VertProxy {
    int v;

    VertProxy(Mesh *m_, int v_) : v(v_), m(m_)
    {
    }

    math::float3 &co()
    {
      return m->v.co[v];
    }

    math::float3 &no()
    {
      return m->v.no[v];
    }

    bool select() const
    {
      return m->v.select[v];
    }

    void select_set(bool val)
    {
      m->v.select.set(v, val);
    }

    EdgeOfVertIter edges()
    {
      return EdgeOfVertIter(m, v, m->v.e[v]);
    }

  private:
    Mesh *m;
  };

  Mesh(int nv, int ne, int nc, int nf) : v(nv), e(ne)
  {
  }

  int make_vertex(math::float3 co)
  {
    int r = v.alloc();

    v.co[r] = co;
    v.e[r] = ELEM_NONE;
    v.l[r] = ELEM_NONE;

    return r;
  }

  EdgeOfVertIter e_of_v(int v1)
  {
    int e1 = v.e[v1];
    return EdgeOfVertIter(this, v1, e1);
  }

  int make_edge(int v1, int v2)
  {
    int r = e.alloc();

    e.vs[r][0] = v1;
    e.vs[r][1] = v2;

    e.disk[r] = math::int4(ELEM_NONE);

    disk_insert(r, v1);
    disk_insert(r, v2);

    return r;
  }

private:
  void disk_insert(int e1, int v1)
  {
    int side1 = e.vs[e1][0] == v1 ? 0 : 1;

    if (v.e[v1] == ELEM_NONE) {
      v.e[v1] = e1;

      e.disk[e1][side1 * 2] = e1;
      e.disk[e1][side1 * 2] = e1;

      return;
    }

    int e2 = v.e[v1];
    int side2 = e.vs[e2][0] == v1 ? 0 : 1;

    int prev = e.disk[e2][side2 * 2];
    int side3 = e.vs[prev][0] == v1 ? 0 : 1;

    e.disk[e2][side2 * 2] = e1; /* e2.prev */

    e.disk[e1][side1 * 2] = prev;   /* e1.prev */
    e.disk[e1][side1 * 2 + 1] = e2; /* e1.next */

    e.disk[prev][side3 * 2 + 1] = e1; /* prev.next */
  }
};
}; // namespace sculptcore::mesh
