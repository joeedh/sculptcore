#pragma once

#include "attribute.h"
#include "elem_data.h"
#include "mesh_types.h"

#include <concepts>
#include <span>
#include <utility>

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

using namespace litestl;
namespace sculptcore::mesh {
struct EdgeOfVertIter {
  MeshBase *m;

  inline EdgeOfVertIter(MeshBase *m_, int v_, int e_) : m(m_), v(v_), e(e_), start_e(e_)
  {
    /* One vs load to seat the side; every ++ then rides the embedded side. */
    side = (e_ != ELEM_NONE && m_->e.vs[e_][1] == v_) ? 1 : 0;
  }

  inline EdgeOfVertIter(const EdgeOfVertIter &b)
      : m(b.m), v(b.v), e(b.e), start_e(b.start_e), side(b.side)
  {
  }

  inline int operator*() const
  {
    return e;
  }

  inline bool operator==(const EdgeOfVertIter &b) const
  {
    return b.e == e;
  }

  inline bool operator!=(const EdgeOfVertIter &b) const
  {
    return !(operator==(b));
  }

  EdgeOfVertIter &operator++()
  {
    if (e == ELEM_NONE) {
      return *this;
    }

    int link = m->e.disk[e][side * 2 + 1]; /* e.next, side-bit encoded */
    e = diskEdge(link);
    side = diskSide(link);

    /* Back at the disk-cycle start → exhausted; flag the end sentinel. */
    if (e == start_e) {
      e = ELEM_NONE;
    }

    return *this;
  }

  EdgeOfVertIter begin() const
  {
    return *this;
  }

  EdgeOfVertIter end() const
  {
    return EdgeOfVertIter(m, v, ELEM_NONE);
  }

private:
  int v, e, start_e, side;
};

struct CornerOfEdgeIter {
  MeshBase *m;

  CornerOfEdgeIter(MeshBase *m_, int e_, int c_) : m(m_), e(e_), c(c_), start_c(c_)
  {
  }
  CornerOfEdgeIter(const CornerOfEdgeIter &b) : m(b.m), e(b.e), c(b.c), start_c(b.start_c)
  {
  }
  bool operator==(const CornerOfEdgeIter &b) const
  {
    return b.c == c;
  }
  bool operator!=(const CornerOfEdgeIter &b) const
  {
    return !operator==(b);
  }

  int operator*() const
  {
    return c;
  }

  CornerOfEdgeIter &operator++()
  {
    if (c == ELEM_NONE) {
      return *this;
    }

    c = m->c.radial_next[c];

    return *this;
  }

  CornerOfEdgeIter begin()
  {
    return CornerOfEdgeIter(m, e, start_c);
  }

  CornerOfEdgeIter end()
  {
    return CornerOfEdgeIter(m, e, c = m->c.radial_prev[start_c]);
  }

private:
  int e, c, start_c;
};

} // namespace sculptcore::mesh
