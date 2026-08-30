#pragma once

#include "litestl/util/span.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"

#include <concepts>
#include <cstdint>

namespace sculptcore::brush {

/* A neighbor source enumerates the 1-ring vertex indices of a vertex for the
 * generated `for_neighbor` loop. Policies are chosen once at brush-command
 * creation (a high-level dispatch), so the inner loop monomorphizes against a
 * single source with no per-neighbor branch. The contract — a static
 * `range(ctx, v)` yielding neighbor vertex ids — is a template on the ctx
 * type, so the concept keys off a marker member rather than a full signature. */
template <class T>
concept NbrSource = requires {
  { T::is_neighbor_source } -> std::convertible_to<bool>;
};

/* Walk the live disk cycle via EdgeOfVertIter, yielding the far vertex of each
 * incident edge. This is the historical for_neighbor lowering. */
struct LiveDiskNbr {
  static constexpr bool is_neighbor_source = true;

  struct Iter {
    mesh::MeshBase *m;
    int outer;
    mesh::EdgeOfVertIter eit;

    int operator*() const
    {
      int e = *eit;
      return (m->e.vs[e][0] == outer) ? m->e.vs[e][1] : m->e.vs[e][0];
    }
    Iter &operator++()
    {
      ++eit;
      return *this;
    }
    bool operator!=(const Iter &o) const
    {
      return eit != o.eit;
    }
  };

  struct Range {
    mesh::MeshBase *m;
    int outer;
    int e0;
    Iter begin() const
    {
      return Iter{m, outer, mesh::EdgeOfVertIter(m, outer, e0)};
    }
    Iter end() const
    {
      return Iter{m, outer, mesh::EdgeOfVertIter(m, outer, ELEM_NONE)};
    }
  };

  template <class Ctx> static Range range(Ctx &ctx, int v)
  {
    mesh::Mesh *m = ctx.node.data->m;
    return Range{m, v, m->v.e[v]};
  }
};

/* Read the cached CSR 1-ring (ensured static for the stroke before the
 * per-node loop). The CSR is built by the same EdgeOfVertIter walk as
 * LiveDiskNbr, so neighbor order — and thus the accumulated result — is
 * identical. */
struct CsrNbr {
  static constexpr bool is_neighbor_source = true;

  template <class Ctx> static litestl::util::span<const int> range(Ctx &ctx, int v)
  {
    // Non-const ref: litestl::util::Vector exposes only a non-const data(); the
    // CSR is logically read-only here but the mesh isn't const in this path.
    mesh::VertNbrCSR &csr = ctx.node.data->m->topo_cache.ring1;
    uint32_t off = csr.offsets[v];
    uint32_t cnt = csr.offsets[v + 1] - off;
    return litestl::util::span<const int>(csr.nbr_verts.data() + off, cnt);
  }
};

} // namespace sculptcore::brush
