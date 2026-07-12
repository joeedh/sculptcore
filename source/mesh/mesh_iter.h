#pragma once

#include "attribute.h"
#include "disk_prof.h" // CLAUDENOTE: M0 disk-bandwidth scaffolding (plan 2026-07-12-1324)
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

    // CLAUDENOTE: M0 disk-bandwidth scaffolding (plan 2026-07-12-1324)
    {
      auto &dp = diskprof::get();
      if (dp.enabled) {
        dp.e_of_v_steps++;
      }
    }

    int side = m->e.vs[e][0] == v ? 0 : 1;
    e = m->e.disk[e][side * 2 + 1]; /* e.next */

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
  int v, e, start_e;
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

    // CLAUDENOTE: M0 disk-bandwidth scaffolding (plan 2026-07-12-1324)
    {
      auto &dp = diskprof::get();
      if (dp.enabled) {
        dp.radial_steps++;
      }
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
