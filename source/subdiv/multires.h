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

#include "litestl/binding/binding.h"
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

  /** Explicit down-refit (S4): least-squares-fit level−1's positions to the
   * current level-`level` surface (Jacobi-CG on the stencil normal equations,
   * warm-started from the current chain), store the fit as level−1
   * displacement, and re-express this level's displacement against the new
   * base so its own surface is preserved. Coarser levels are untouched; finer
   * levels re-derive. A stale level−1 resident is refreshed (its mesh/tree
   * pointers change). Returns the number of level−1 verts changed; requires
   * level >= 2. */
  int downRefit(int level);

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

  /** Synthesize the per-corner atlas UVs for a level mesh (X1): each grid is a
   * chart in a ⌈√G⌉-per-row cell layout with an inset gutter. A pure function
   * of (gridCount, grid id, lattice coord) — identical across levels and
   * backends, so finest-level VDM texels sample correctly from any level's
   * UVs. Called by materialize(); public for the S5-style topo-mesh hosts. */
  void assignGridUVs(mesh::Mesh &m, int level);

  /** Resident-level budget; eviction is LRU by lastUse, never the active
   * level (plan: users toggle two levels constantly, so default 3). */
  int lruBudget = 3;

  /** Spatial-tree tuning applied when a level is materialized (0 = the
   * SpatialTree default). The app sets these to its draw-path values so
   * adopted level trees match app-built ones. */
  int treeLeafLimit = 0;
  int treeDepthLimit = 0;
  int treeGpuTriTarget = 0;

  GridsStore store;
  Refiner refiner;

  static litestl::binding::types::Struct<Multires> *defineBindings();

private:
  /** Ensure the cached position chain is valid through `level`; returns it. */
  litestl::util::Vector<litestl::math::float3> &ensureChain(int level);
  /** Re-express `pos` (dense by level vert id) as level-`level` store
   * displacement: disp = frameᵀ·(pos − base), base = stencil(prev chain),
   * frames on the smoothed base. Writes verts where `mask` is null or set. */
  void storeDispFromPositions(int level,
                              const litestl::util::Vector<litestl::math::float3> &pos,
                              const litestl::util::Vector<bool> *mask);
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
