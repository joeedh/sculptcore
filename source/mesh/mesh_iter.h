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
/** Sequential scan of vertex `v_`'s incident-edge slab. `e_` keeps the old
 * head-seed signature: callers pass `v.e[v_]` (always the disk head) or
 * ELEM_NONE; the walk itself rides the slab span. Invariant (unchanged from
 * the cycle era): the vertex's disk must not be spliced or grown mid-walk. */
struct EdgeOfVertIter {
  MeshBase *m;

  inline EdgeOfVertIter(MeshBase *m_, int v_, int e_) : m(m_), v(v_)
  {
    if (e_ == ELEM_NONE) {
      p = pend = nullptr;
      return;
    }
    const math::int2 &slot = m_->v.disk[v_];
    p = m_->disk_arena.span(slot);
    pend = p + DiskSlabArena::count(slot);
  }

  inline EdgeOfVertIter(const EdgeOfVertIter &b) : m(b.m), v(b.v), p(b.p), pend(b.pend)
  {
  }

  inline int operator*() const
  {
    return diskEdge(*p);
  }

  /** Which slot of the current edge's `.edge.vs` is `v` (the packed side). */
  inline int side() const
  {
    return diskSide(*p);
  }

  inline bool operator==(const EdgeOfVertIter &b) const
  {
    return (p == pend) == (b.p == b.pend);
  }

  inline bool operator!=(const EdgeOfVertIter &b) const
  {
    return !(operator==(b));
  }

  EdgeOfVertIter &operator++()
  {
    if (p == pend) {
      return *this;
    }

    // CLAUDENOTE: M0 disk-bandwidth scaffolding (plan 2026-07-12-1324)
    {
      auto &dp = diskprof::get();
      if (dp.enabled) {
        dp.e_of_v_steps++;
      }
    }

    p++;
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
  int v;
  const int *p, *pend;
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
