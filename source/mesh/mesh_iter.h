#pragma once

#include "attribute.h"
#include "elem_data.h"
#include "mesh_types.h"

#include <concepts>
#include <span>
#include <utility>

#include "math/vector.h"
#include "util/vector.h"

namespace sculptcore::mesh {
struct EdgeOfVertIter {
  MeshBase *m;

  inline EdgeOfVertIter(MeshBase *m_, int v_, int e_) : m(m_), v(v_), e(e_), start_e(e_)
  {
  }

  inline EdgeOfVertIter(const EdgeOfVertIter &b)
      : m(b.m), v(b.v), e(b.e), start_e(b.start_e)
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

    int side = m->e.vs[e][0] == v ? 0 : 1;
    e = m->e.disk[e][side * 2 + 1]; /* e.next */

    if (e == start_e) {
      if (0 && first) {
        first = false;
      } else {
        /* Flag endpoint. */
        e = ELEM_NONE;
        return *this;
      }
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
  int v, e, start_e;
  bool first = true;
};
} // namespace sculptcore::mesh
