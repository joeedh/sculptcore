#pragma once

#include "litestl/util/vector.h"

#include <cstdint>

namespace sculptcore::mesh {
struct Mesh;

/* Compressed-sparse-row 1-ring vertex adjacency, indexed by raw vertex id.
 * Vertex v's neighbors are nbr_verts[offsets[v] .. offsets[v+1]); free slots
 * get an empty range. Neighbor order matches an EdgeOfVertIter disk walk so
 * the cached path is bit-identical to the live walk it replaces. */
struct VertNbrCSR {
  litestl::util::Vector<uint32_t> offsets; /* size capacity+1 */
  litestl::util::Vector<int> nbr_verts;
  int count = 0; /* live vert count the CSR was built for */

  void clear()
  {
    offsets.clear();
    nbr_verts.clear();
    count = 0;
  }
};

/* A compact, order-preserving snapshot of the full radial-edge topology,
 * rich enough to rebuild every live TOPO link column exactly. In frozen mode
 * the live links are dropped to save RAM and this is the authoritative
 * connectivity; any topology edit thaws back to live links via rebuildLinks().
 *
 * All arrays are indexed by the element's existing dense slot id (frozen mode
 * never moves elements), so rebuilding restores bit-identical link values.
 * The four CSR runs are captured by walking the live cycles in order, so the
 * rebuilt disk / radial / loop cycles preserve the original ordering. */
struct FrozenTopo {
  /* Edge endpoints (replaces e.vs), indexed by edge slot. */
  litestl::util::Vector<int> edge_v0;
  litestl::util::Vector<int> edge_v1;

  /* vert -> incident edges, disk order (rebuilds v.e + e.disk). */
  litestl::util::Vector<uint32_t> vert_edge_off; /* size V_cap+1 */
  litestl::util::Vector<int> vert_edges;

  /* edge -> radial corners, radial order (rebuilds e.c + c.radial_next/prev). */
  litestl::util::Vector<uint32_t> edge_corner_off; /* size E_cap+1 */
  litestl::util::Vector<int> edge_corners;

  /* face -> owned lists, in f.l/l.next order (rebuilds f.l + l.next + l.f). */
  litestl::util::Vector<uint32_t> face_list_off; /* size F_cap+1 */
  litestl::util::Vector<int> face_lists;

  /* list -> corners, loop order (rebuilds l.c + c.l/next/prev + c.v/e). */
  litestl::util::Vector<uint32_t> list_corner_off; /* size L_cap+1 */
  litestl::util::Vector<int> list_corners;

  bool built = false;

  void clear();
  /* Capture the live topology into this snapshot. */
  void build(Mesh &m);
  /* Recompute every live TOPO link column in place from this snapshot. The
   * caller must have re-materialized the TOPO attribute pages first. */
  void rebuildLinks(Mesh &m);
};

/* Cached frozen-topology adjacency shared by the C++ and GPU brush paths.
 * Lazily (re)built; validity is keyed on Mesh::topo_stamp so any topology
 * edit forces a rebuild on the next ensure*(). Owned by Mesh; derived data,
 * never serialized. */
struct MeshTopoCache {
  VertNbrCSR ring1;
  uint64_t ring1_stamp = ~0ull; /* Mesh::topo_stamp at build; ~0 = never built */
  FrozenTopo frozen;
  uint64_t frozen_stamp = ~0ull; /* Mesh::topo_stamp at frozen.build() */

  void invalidate()
  {
    ring1_stamp = ~0ull;
    frozen_stamp = ~0ull;
  }

  bool valid(const Mesh &m) const;
  const VertNbrCSR &ensureRing1(Mesh &m);
  /** Capture the frozen-topology snapshot, reusing the last one when no
   * topology edit has landed since (see Mesh::freezeTopo). */
  void ensureFrozen(Mesh &m);
};
} // namespace sculptcore::mesh
