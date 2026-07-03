#pragma once

/** Multires level materialization + LRU (displacementAndSubSurf plan, S3).
 *
 * The grids store is the canonical multires state; editing happens on a
 * materialized `mesh::Mesh` of the active level so the whole existing stack
 * (spatial tree, executor, meshlog, draw) applies unchanged (architecture
 * report §4.1). A level's positions are the discrete displaced-subdivision
 * pyramid: base_L = stencil_L(pos_{L-1}), pos_L = base_L + frame·disp_L, with
 * the F3 frame provider evaluated on the smoothed base — the per-level
 * position chain is cached and recomputed deterministically, so an unedited
 * materialization is bit-stable.
 *
 * writeback() re-expresses a level mesh's positions into frame-relative
 * store deltas, SKIPPING verts whose position is bit-identical to the
 * materialized baseline — so an edit-free switch/writeback leaves the store
 * byte-identical (the S3 losslessness gate), and float drift from the
 * frame-projection round-trip is paid only where an edit actually happened. */

#include "grids.h"
#include "subdiv.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

namespace sculptcore::mesh {
struct Mesh;
}
namespace sculptcore::spatial {
struct SpatialTree;
}

namespace sculptcore::subdiv {

/** One resident (materialized) level: the mesh + its spatial tree. Owned by
 * the Multires LRU; pointers are stable until the slot is evicted. */
struct MultiresSlot {
  int level = 0;
  mesh::Mesh *mesh = nullptr;
  spatial::SpatialTree *tree = nullptr;
  uint64_t lastUse = 0;
};

struct Multires {
  Multires() = default;
  Multires(const Multires &) = delete;
  Multires &operator=(const Multires &) = delete;
  ~Multires();

  /** Refine `cage` (not owned) `maxLevel` times, seed the store's levels, and
   * release the refiner's eagerly-built level meshes — levels rematerialize
   * on demand from the grid tables. */
  void init(mesh::Mesh &cage, int maxLevel);

  int maxLevel() const
  {
    return int(refiner.levels.size());
  }
  int activeLevel() const
  {
    return activeLevel_;
  }

  /** Write back the current active level (if any), then materialize `level`
   * (1-based) and make it active. */
  MultiresSlot *setActiveLevel(int level);

  /** Materialize `level` into the LRU (or refresh its stamp if resident)
   * without touching the active level. */
  MultiresSlot *materialize(int level);

  /** Re-express `level`'s resident mesh positions into store displacement
   * (frame-relative, vs the smoothed base). Bit-identical-to-baseline verts
   * are skipped; seam replicas all receive the write. Finer cached positions
   * and finer resident levels are invalidated when anything changed. Returns
   * the number of changed verts (0 for a non-resident level). */
  int writeback(int level);

  /** Drop cached position chains and resident meshes strictly above `level`
   * (after a level-`level` edit lands in the store). */
  void invalidateAbove(int level);
  /** Drop everything derived (all levels' caches + residents) — cage edited. */
  void invalidateAll();

  MultiresSlot *findSlot(int level);

  /** Build a level's topology-only mesh from the grid tables (dense vert ids
   * matching the stencil rows; one quad per grid cell; positions zeroed).
   * Caller owns the result. Used internally by materialization and by the
   * GPU-amplification A/B (S5) to host amplified positions. */
  mesh::Mesh *buildLevelTopo(int level);

  /** Resident-level budget; eviction is LRU by lastUse, never the active
   * level (plan: users toggle two levels constantly, so default 3). */
  int lruBudget = 3;

  GridsStore store;
  Refiner refiner;

private:
  /** Ensure the cached position chain is valid through `level`; returns it. */
  litestl::util::Vector<litestl::math::float3> &ensureChain(int level);
  bool dispNonZero(int level);
  void evictSlot(int index);
  void evictOverBudget();

  struct LevelPos {
    bool valid = false;
    litestl::util::Vector<litestl::math::float3> pos;
  };

  mesh::Mesh *cage_ = nullptr;
  int activeLevel_ = 0; /* 0 = the cage itself (no materialized level) */
  uint64_t useCounter_ = 0;
  litestl::util::Vector<LevelPos> posCache_; /* [0] = level 1 */
  litestl::util::Vector<MultiresSlot> slots_;
};

} // namespace sculptcore::subdiv
