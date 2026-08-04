#pragma once

/** Grids-native editable level state (grids-native brush path, G1).
 *
 * A `GridLevelDomain` is the flat, dense-by-level-vert-id view of one multires
 * level that sculpt strokes edit directly — no materialized `mesh::Mesh`, no
 * triangle BVH, no meshlog. Positions ARE the Multires chain cache's
 * `LevelPos::pos` (edited in place, so the chain stays authoritative); normals,
 * mask and the 1-ring CSR are dense sidecars owned here. Boundary verts exist
 * exactly once (the dense-id layout), so there is no replica stitching: the
 * only place grid replicas appear is the store exchange (mask flush writes
 * every alias through the occurrence table).
 *
 * Owned by `Multires` (see Multires::gridDomain), which drops it whenever the
 * level's cached chain entry resets or a mesh-path edit folds into the store —
 * a domain pointer is valid until the next such fold point. */

#include "grids.h"
#include "subdiv.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <span>

namespace sculptcore::subdiv {

struct Multires;
struct GridTree;

struct GridLevelDomain {
  using float3 = litestl::math::float3;
  template <typename T> using Vector = litestl::util::Vector<T>;

  GridLevelDomain() = default;
  GridLevelDomain(const GridLevelDomain &) = delete;
  GridLevelDomain &operator=(const GridLevelDomain &) = delete;
  ~GridLevelDomain();

  /** Build the domain for `level` (1-based) of `mr`: binds `pos` to the chain
   * cache, builds the occurrence table + lattice 1-ring CSR, fills normals,
   * and mirrors the "mask" store channel (zeros when absent). */
  void build(Multires &mr, int level);

  int level() const
  {
    return level_;
  }
  int vertCount() const
  {
    return vertCount_;
  }
  int gridCount() const
  {
    return gridCount_;
  }
  /** Quad cells per grid side (S); lattices are (S+1)^2 verts. */
  int gridSide() const
  {
    return side_;
  }

  /** Dense editable positions — the Multires chain entry, edited in place. */
  Vector<float3> &pos()
  {
    return *pos_;
  }
  const Vector<float3> &pos() const
  {
    return *pos_;
  }

  /** Grid `g`'s (S+1)^2 lattice->vert-id table (row-major, v-major). */
  const int *gridVerts(int g) const;

  /** Neighbors of `v` in the CSR ring (unique vert ids, lattice order). */
  std::span<const int> neighbors(int v) const
  {
    return std::span<const int>(&ring1[ring1Offsets[v]],
                                size_t(ring1Offsets[v + 1] - ring1Offsets[v]));
  }

  /** All (grid,u,v) lattice slots aliasing vert `v` (3 ints per occurrence);
   * canonical (lowest-grid) occurrence first. */
  std::span<const int> occurrences(int v) const
  {
    return std::span<const int>(&occCoords[occOffsets[v] * 3],
                                size_t(occOffsets[v + 1] - occOffsets[v]) * 3);
  }

  /** Full geometric-normal fill (grid quad fans; every vert). */
  void refreshAllNormals();
  /** Refresh normals of `touched` plus every corner of their incident cells
   * (the exact dependency closure of the cell-Newell normal — the lattice
   * 8-neighborhood, seam-crossing, diagonals included). */
  void refreshNormals(std::span<const int> touched);

  /** Mirror the "mask" store channel into `mask` (zeros when no channel). */
  void syncMaskFromStore();
  /** Land `mask` in the store channel (created on demand), writing every
   * occurrence so seam replicas stay consistent. */
  void flushMaskToStore();
  /** Restricted flush: land only `verts` (a stroke's touched set). */
  void flushMaskToStore(std::span<const int> verts);
  /** Find-or-create the "mask" store channel; returns its index. A mask
   * stroke ensures it up front so undo capture sees a live channel. */
  int ensureMaskChannel();

  /** The domain's spatial structure, built lazily (grid_tree.h).
   * `leafVertTarget` <= 0 uses the GridTree default; honored on first build. */
  GridTree *ensureTree(int leafVertTarget = 0);

  /** Dense geometric vertex normals (normalized). */
  Vector<float3> no;
  /** Dense mask mirror (one float per vert). */
  Vector<float> mask;

  /** Lattice 1-ring CSR: vert v's unique edge-neighbors are
   * ring1[ring1Offsets[v] .. ring1Offsets[v+1]). Static per level. */
  Vector<int> ring1Offsets; // vertCount+1
  Vector<int> ring1;

  /** Canonical owning grid per vert: 3 ints {grid, u, v}
   * (levelVertGridCoordsOut's first-owner rule). */
  Vector<int> vertGrid;

  /** Occurrence CSR: every lattice slot of every grid, grouped by vert id.
   * occOffsets is in occurrences (multiply by 3 to index occCoords). */
  Vector<int> occOffsets; // vertCount+1
  Vector<int> occCoords;  // 3 ints per occurrence {grid, u, v}

  Multires *multires()
  {
    return mr_;
  }

private:
  float3 vertNormal(int vid) const;
  void buildOccurrences();
  void buildRing1();

  Multires *mr_ = nullptr;
  int level_ = 0;
  int side_ = 0; // == GridsStore::sideForLevel(level_)
  int vertCount_ = 0;
  int gridCount_ = 0;
  Vector<float3> *pos_ = nullptr;
  GridTree *tree_ = nullptr;

  /** Halo-dedup stamps for refreshNormals (per vert, generation-keyed). */
  Vector<uint32_t> normalStamp_;
  uint32_t normalGen_ = 0;
};

} // namespace sculptcore::subdiv
