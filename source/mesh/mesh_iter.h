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
  inline EdgeOfVertIter(MeshBase *m_, int v_, int e_) : m(m_), v(v_), e(e_), start_e(e_)
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
  MeshBase *m;
  int v, e, start_e;
  bool first = true;
};
} // namespace sculptcore::mesh
