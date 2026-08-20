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
 * frame-projection round-trip is paid only where an edit actually happened.
 *
 * The pyramid is derived downward-blind — a level's positions depend only on
 * the levels BELOW it — so an edit lands wholly at the level it was made on
 * and coarser levels keep showing the pre-edit surface. propagateDown() closes
 * that gap on demand: it restricts a level's surface into the one below and
 * re-expresses its own displacement against the new base, so the fine surface
 * survives untouched while the coarse level becomes a summary of it. Levels
 * that owe their neighbour below such a step are tracked in downPropPending_
 * and settled by a downward setActiveLevel(). */

#include "grid_attrs.h"
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

namespace sculptcore::vdm {
struct VdmStore;
}

namespace sculptcore::subdiv {

struct GridLevelDomain;
struct GridDrawSource;

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
  /** The base cage this stack refines (not owned; null before init). */
  mesh::Mesh *cage() const
  {
    return cage_;
  }
  int activeLevel() const
  {
    return activeLevel_;
  }

  /** Write back the current active level (if any), then materialize `level`
   * (1-based) and make it active. Switching DOWN settles any propagateDown
   * debt on the way (see downPropPending_), which is what makes a coarser
   * level show the detail sculpted above it.
   *
   * `propagate` = false suppresses that cascade, for a switch that replays
   * history rather than expressing user intent — an undo auto-switching back
   * to the level a step was made on, where propagating would fold the very
   * detail about to be undone into the level below. The debt is left standing,
   * so the next real user switch still settles it. */
  MultiresSlot *setActiveLevel(int level, bool propagate = true,
                               bool materializeSlot = true);

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

  /** Push level `level`'s surface down one step so the coarser level reflects
   * it: level−1's positions become the full-weighting restriction of this
   * level's (the stencil-weight-normalized average of the fine positions each
   * coarse vert feeds, `R = diag(Aᵀ1)⁻¹Aᵀ`), stored as level−1 displacement,
   * with this level's displacement re-expressed against the new base so its own
   * surface is preserved. Unlike downRefit's pseudo-inverse this is an
   * averaging operator — its rows sum to 1, so it cannot overshoot the surface
   * it summarizes — and it is idempotent: pos_{L−1} is a pure function of
   * pos_L. Slot/cache handling matches downRefit. Requires level >= 2. */
  int propagateDown(int level);

  /** Down-propagation debt: true when `level` carries detail the level below
   * has not been given (see downPropPending_). It is NOT derivable from the
   * store — a level whose displacement is zero still differs from the
   * restriction of the level above it — so an undo blob that dropped it would
   * silently re-open the "coarse levels do not follow a fine edit" bug after
   * an undo. Multires_serializeStore/restoreStore carry it for that reason;
   * nothing else should need these. `level` out of range reads false / is
   * ignored. */
  bool downPropDebt(int level) const
  {
    return level >= 2 && level < int(downPropPending_.size()) ? downPropPending_[level]
                                                              : false;
  }
  void setDownPropDebt(int level, bool value)
  {
    if (level >= 2 && level < int(downPropPending_.size())) {
      downPropPending_[level] = value;
    }
  }
  void clearDownPropDebt()
  {
    for (int l = 0; l < int(downPropPending_.size()); l++) {
      downPropPending_[l] = false;
    }
  }

  /** Append one finer Catmull-Clark level (zero displacement — a smooth
   * subdivision of the current finest surface), preserving every existing
   * level's detail, and make the new finest level active. Folds pending edits
   * on the active level first. Returns the new maxLevel, or the unchanged
   * maxLevel when already at the level cap. */
  int addLevel();

  /** Pop the finest level — the inverse of addLevel(), used by its ToolOp's
   * undo/redo. Folds pending edits first, then rebuilds the stack one level
   * shallower. Returns the new maxLevel (unchanged when maxLevel() <= 1). */
  int removeTopLevel();

  /** Drop cached position chains and resident meshes strictly above `level`
   * (after a level-`level` edit lands in the store). */
  void invalidateAbove(int level);
  /** Drop everything derived (all levels' caches + residents) — cage edited. */
  void invalidateAll();

  MultiresSlot *findSlot(int level);

  /** Dense editable positions for `level` (ensures the cached chain through
   * it). The grids-native brush path edits this in place — the chain stays
   * authoritative, so invalidateAbove/ensureChain semantics are untouched. */
  litestl::util::Vector<litestl::math::float3> &levelPositions(int level)
  {
    return ensureChain(level);
  }

  /** The grids-native editable view of `level` (grid_domain.h), built lazily
   * and owned here. Dropped whenever the level's cached chain entry resets or
   * a mesh-path edit folds into the store (writeback / down-fit / VDM capture)
   * — callers re-fetch after any such fold point, like slot pointers. */
  GridLevelDomain *gridDomain(int level);

  /** Whether `level`'s grid domain is currently alive (gridDomain would
   * return the cached view rather than paying a rebuild). Hosts gate
   * domain-backed queries (raycast) on this after fold points. */
  bool hasGridDomain(int level) const
  {
    return level >= 1 && level <= int(domains_.size()) && domains_[level - 1] != nullptr;
  }

  /** Monotonic domain lifecycle counter: bumped on every domain build and
   * every drop. Consumers that cache a `GridLevelDomain *` MUST compare this,
   * not the pointer — a drop + rebuild routinely reuses the same allocation
   * (same size, back-to-back free/alloc), so pointer equality cannot detect
   * a rebuild and a stale binding dangles into the freed tree. */
  uint64_t domainGeneration() const
  {
    return domainGen_;
  }

  /** Whether `level`'s resident slot mesh is BEHIND the store (a grids fold
   * ran without a host mirror). While set, writeback(level) must not diff
   * the slot — the pre-stroke slot positions would read as fresh mesh-path
   * edits and fold OVER the grids stroke, silently destroying it (the
   * level-switch / save / undo-heal paths all reach writeback implicitly).
   * writeback() self-heals instead: see its guard. */
  bool slotStale(int level) const
  {
    return (slotStaleMask_ >> level) & 1u;
  }
  /** The host mirrored every diverged vert into the slot (or the slot was
   * rebuilt from the store): the slot is current again. */
  void clearSlotStale(int level)
  {
    slotStaleMask_ &= ~(1u << level);
  }
  /** Copy the level's domain positions/normals over the resident slot mesh
   * (all verts) and flag its tree for geometry re-upload + bounds regen;
   * clears the stale bit. Rebuilds the domain if a fold dropped it (the
   * store is current either way). No-op without a resident slot. */
  void syncSlotFromDomain(int level);

  /** The registered grids draw source, if any (grid_draw_source.h). Owned by
   * the extdraw registry — this is a backref for the stroke/undo dirty feeds;
   * ~Multires tells the source to detach. */
  GridDrawSource *drawSource()
  {
    return drawSource_;
  }
  void setDrawSource(GridDrawSource *s)
  {
    drawSource_ = s;
  }

  /** The store channel a sculpt writeback lands in right now: the edit
   * target's channel when one is set and enabled, else channel 0 — the same
   * rule storeDispFromPositions applies. The grids stroke log keys its
   * store-block capture on this. */
  int writebackChannel() const;

  /** Grids-native stroke-end fold: re-express the domain-edited positions of
   * `level`'s `changed` verts as store displacement, walking only `grids`
   * (the touched verts' occurrence grids), then invalidate finer levels and
   * set the down-propagation debt. The chain entry IS the edited state, so
   * there is no baseline update — O(region), not O(level). */
  void gridsWriteback(int level,
                      const litestl::util::Vector<bool> &changed,
                      const litestl::util::Vector<int> &grids);

  /** Seed `level` from grid-sample absolute positions (levelGridVertsOut
   * layout: gridCount·(S+1)² samples, seam replicas equal, last-writer-wins)
   * WITHOUT materializing anything: the chain + base/frames are ensured (one
   * throwaway topo mesh for the frame provider), positions land in the chain
   * entry in place, and the whole level re-expresses into the store. The
   * down-propagation is NOT cascaded — the debt flag is set and the first
   * downward switch settles it, so a top-level seed (mode enter) pays no
   * coarse-level work up front. Any resident slot of `level` (or finer) is
   * dropped; the caller re-activates. Returns the sample count, -1 on a
   * count mismatch. */
  int seedLevelPositions(int level, const float (*samples)[3], int sampleNum);

  /** Build a level's topology-only mesh from the grid tables (dense vert ids
   * matching the stencil rows; one quad per grid cell; positions zeroed).
   * Caller owns the result. Used internally by materialization and by the
   * GPU-amplification A/B (S5) to host amplified positions. */
  mesh::Mesh *buildLevelTopo(int level);

  /** Synthesize the per-corner atlas UVs for a level mesh (X1): each grid is a
   * chart in a ⌈√G⌉-per-row cell layout with an inset gutter. A pure function
   * of (gridCount, grid id, lattice coord) — identical across levels and
   * backends, so finest-level VDM texels sample correctly from any level's
   * UVs. Written to `vdm::PTEX_ATLAS_ATTR`, not to the mesh's UV map, which is
   * the cage's (see #assignDerivedAttrs). Called by materialize(); public for
   * the S5-style topo-mesh hosts. */
  void assignGridUVs(mesh::Mesh &m, int level);

  /** Stamp a level mesh with the subdivided cage attributes the draw path reads
   * — corner `uv` (AttrUse::UV), vertex `color`, face `group` — from the derived
   * grid-sample layers (#MultiresAttrs). This is what makes a materialized slot
   * draw the same surface data as the grids source; without it a level mesh has
   * no UV map of its own and the host's uv slot resolves nothing. Cheap to
   * repeat, and must be repeated after anything that invalidates those layers
   * (a cage attribute edit, a uv_smooth change). */
  void assignDerivedAttrs(mesh::Mesh &m, int level);

  /** A cage INT face attribute resolved per grid, in the refiner's grid
   * enumeration (one grid per cage corner). False — leaving `out` empty — when
   * the cage carries no such attribute, or when the walk disagrees with the
   * refiner's grid count. */
  bool gridFaceInts(const char *name, litestl::util::Vector<int> &out);

  /** The cage face each grid belongs to, in the refiner's grid enumeration
   * (one grid per cage corner, so all grids of a face are contiguous). False,
   * leaving `out` empty, when the walk disagrees with the refiner's count. */
  bool gridCageFaces(litestl::util::Vector<int> &out);

  /** Push a level's per-cell INT face attribute back down onto the cage — the
   * inverse of #assignDerivedAttrs' stamp, and the only route home for an edit
   * to a derived face layer (face sets).
   *
   * The cells come from the grids store's Face-domain session channel of that
   * name when one holds data for `level` (what a grids-native face stroke
   * writes), and from the materialized level mesh otherwise.
   *
   * Blender writes a face set on multires to the whole base face, unweighted
   * (sculpt_face_set.cc), so the rule here is binary: a cage face takes the
   * value of the lowest-indexed cell, across all of its grids, that disagrees
   * with what the cage already holds; a face no cell disagrees with is left
   * alone. Every cell of a changed face is then re-stamped to the adopted
   * value — leaving a face non-uniform would make the next scatter read its
   * untouched cells as a fresh disagreement and propose reverting the face.
   *
   * Appends the grid ids of every changed face to `r_grids` (the draw source's
   * partial-refill list) and returns the number of cage faces changed. Creates
   * the cage layer on first use. 0 when neither source has this level. */
  int scatterFaceIntToCage(int level, const char *name, litestl::util::Vector<int> &r_grids);

  /** The cage vertex each grid's corner sample belongs to, in the refiner's
   * grid enumeration. Grid `g`'s lattice sample (0, 0) IS this cage vert: the
   * subdivision weights there are one-hot on the grid's own corner in both
   * MultiresAttrs::buildBilinear branches, so the correspondence is exact
   * rather than a fit. A cage vert of valence n appears n times. False,
   * leaving `out` empty, when the walk disagrees with the refiner's count. */
  bool gridCageVerts(litestl::util::Vector<int> &out);

  /** Push a level's per-vertex FLOAT4 attribute back down onto the cage — the
   * vertex-domain twin of #scatterFaceIntToCage, and the only route home for
   * painted colour, which neither the store (engine-owned) nor the level mesh
   * (an evictable cache) persists.
   *
   * This is restriction, not a transpose: it reads only the samples that *are*
   * cage verts (lattice (0, 0) of each grid, see #gridCageVerts) and writes
   * them through unweighted. Spreading a fine sample back across the verts
   * that fed it would need an adjoint and is ill-posed; this needs neither.
   *
   * The samples come from the grids store's Vertex-domain channel of that name
   * when one holds data for `level` (what a grids-native colour stroke writes,
   * through every seam occurrence), and from the materialized level mesh
   * otherwise. Unlike the face twin nothing is re-stamped afterwards: the
   * replicas of a cage vert already agree, so the next scatter reads no
   * disagreement.
   *
   * The cost is a resolution collapse — only cage verts survive, so detail
   * finer than the base mesh is not persistent paint. Creates the cage layer
   * on first use, filled white (an unpainted vert must read as Blender's
   * default vertex colour, not black). Returns the number of cage verts
   * changed; 0 when neither source has this level. */
  int scatterVertFloat4ToCage(int level, const char *name);

  /** #gridFaceInts for `material_index`. Callers treat a false return as every
   * face being material 0, which is what both draw paths default to. */
  bool gridMaterials(litestl::util::Vector<int> &out);

  /** Stamp a level mesh's `material_index` face attribute from the cage's, one
   * value per grid spread over that grid's cells (#gridMaterials). No-op when
   * the cage has no materials. Called by materialize(); public alongside
   * #assignGridUVs for the S5-style topo-mesh hosts. */
  void assignGridMaterials(mesh::Mesh &m, int level);

  /** Resident-level budget; eviction is LRU by lastUse, never the active
   * level (plan: users toggle two levels constantly, so default 3). */
  int lruBudget = 3;

  /** X5: grids-store raw-chunk budget in bytes (0 = off). Enforced after
   * level switches + writebacks: non-active levels evict finest-first to
   * lz4 blobs until under budget; readers rehydrate transparently through
   * `GridsStore::elem`. */
  size_t storeBudgetBytes = 0;
  /** Bound setter (the generic binding has no size_t/uint64 param). */
  void setStoreBudget(int bytes)
  {
    storeBudgetBytes = bytes > 0 ? size_t(bytes) : 0;
  }
  void enforceStoreBudget();

  /** Spatial-tree tuning applied when a level is materialized (0 = the
   * SpatialTree default). The app sets these to its draw-path values so
   * adopted level trees match app-built ones. */
  int treeLeafLimit = 0;
  int treeDepthLimit = 0;
  int treeGpuTriTarget = 0;

  GridsStore store;
  Refiner refiner;

  /** Per-grid-element attribute policy + the derived (subdivided-from-cage)
   * sample layers the draw path reads. See grid_attrs.h. */
  MultiresAttrs &gridAttrs()
  {
    return gridAttrs_;
  }

  /** Flat S2 adjacency for a Ptex VDM store: 8 ints per grid ({grid, side}
   * × 4 sides in GridSideType order; -1 = boundary). The bound caller feeds
   * this to VdmStore::configurePtex — vdm and subdiv stay decoupled. */
  void vdmAdjacencyOut(litestl::util::Vector<int> &out);

  /** Geometry→VDM capture (X4 stage 2): transfer this level's grids-store
   * displacement into `vstore`'s Ptex texels (bilinear over the disp lattice,
   * ADDED onto existing texels), zero the disp, and drop the level's surface
   * onto the smooth base (materialized mesh + baseline updated, finer levels
   * invalidated, skirts synced). Returns texels written.
   * Refuses (returns 0) while any enabled sculpt-layer channel contributes —
   * capture is defined on channel 0 only (layer×VDM migration is post-V2).
   *
   * FRAME SPACE MISMATCH, open: the texels it writes are in the multires
   * lattice frame (#parametricFrames), but the VDM consumers (vdm_bake /
   * _promote / _splat) decode against the F3 provider's FRAME_*_ATTR. The two
   * agreed while multires also used F3; they no longer do. Capture must convert
   * — or the VDM path must adopt the lattice frame — before this is wired to a
   * host. Unreached today: nothing outside the c-api calls it. */
  int captureDetailToVdm(int level, vdm::VdmStore &vstore);

  // ---- Sculpt layers on the stack (sculptLayersV2 M3) ----
  // One FLOAT3 store channel per layer, keyed by a settings-only row on the
  // CAGE's sculptLayers sidecar with the same name (no vertex column — level
  // meshes are derived state). Level positions composite
  // disp_total = ch0 + Σ wᵢ·enabledᵢ·chᵢ in frame space before the
  // base + frame·disp reconstruction; writeback lands in the edit target's
  // channel (cage.activeEditLayer), else channel 0. Every mutator writes the
  // active level back FIRST, so pending edits fold under the old settings,
  // then invalidates + rematerializes (slot pointers change — callers
  // re-fetch, like downRefit). Row order always equals channel order 1..N.

  /** Add a layer: settings-only cage row + zero FLOAT3 channel. Returns the
   * settings index (a fresh zero layer at weight 1 changes nothing). */
  int layerAdd();
  /** Remove the row + its channel (destructive — the layerTable/store-blob
   * pair is the undo seam). The edit target ends first (folding pending). */
  void layerRemove(int li);
  void layerSetWeight(int li, float weight);
  void layerSetEnabled(int li, int enabled);
  void layerSetFrozen(int li, int frozen);
  /** Make layer `li` the writeback target (-1 clears): folds pending edits
   * under the old target, enables + pins weight 1. Frozen/invalid refuse.
   * Returns the resulting target. */
  int setEditTarget(int li);
  int editTarget() const;

  // Marshal-safe reads for the app panel (mirror the Mesh sculptLayer*
  // surface; the rows live on the cage).
  int layerCount() const;
  float layerWeight(int li) const;
  int layerEnabled(int li) const;
  int layerFrozen(int li) const;

  /** Snapshot every row's {weight, enabled, frozen} in row (== channel)
   * order — pair with the store blob for layer-remove undo. */
  void layerTableOut(litestl::util::Vector<float> &out);
  /** Rebuild the rows from the store's channels 1..N (names from channels,
   * fields from a layerTableOut snapshot; extra/missing entries default),
   * then refresh levels. The edit target is cleared. Non-const ref: the
   * binding marshals Vector<float> params as bound vector objects. */
  void layerTableRestore(litestl::util::Vector<float> &table);

  /** X3 export seam: the level's CSR stencil (maps level-1 → level) as
   * marshal-safe out-params (this method + the three below). The TS-device
   * SpMV uploads these VERBATIM — ascending-row order + per-component fma is
   * the bit-consistency contract (wgpu_stencil.cc / StencilTable::eval).
   * Meta = {coarseCount, fineCount, nnz}; empty for an out-of-range level. */
  void stencilMetaOut(int level, litestl::util::Vector<int> &out);
  void stencilOffsetsOut(int level, litestl::util::Vector<int> &out);
  void stencilIndicesOut(int level, litestl::util::Vector<int> &out);
  void stencilWeightsOut(int level, litestl::util::Vector<float> &out);

  /** Render-level triangle index buffer straight from the grid tables (two
   * triangles per cell, matching buildLevelTopo's quad winding) — the X3
   * tessellated draw's static topology, no materialized mesh needed. */
  void levelTriIndicesOut(int level, litestl::util::Vector<int> &out);

  /** Per-fine-vert grid identity for the level: 3 ints per vert
   * {grid, latticeU, latticeV} (first-owner grid for seam replicas; grid -1
   * only if a vert somehow appears in no grid). The X3 finalize kernel's
   * per-vert VDM sampling coordinate (param = lattice / gridSide). */
  void levelVertGridCoordsOut(int level, litestl::util::Vector<int> &out);

  /** Per-vert orthonormal tangent frame for `level`, derived from the grid
   * lattice instead of the curvature cross field: central differences of the
   * smooth `base` along ±u/±v (one-sided on the lattice borders), normal from
   * their cross product, tangent by Gram-Schmidt. Each vert uses its canonical
   * (lowest-index) owning grid, so every replica of a border vert gets one
   * value and no tangent directions are averaged. Dense by level vert id,
   * matching the frameNo/frameTa cache layout. This IS the multires frame
   * space: every encode (#storeDispFromPositions) and decode (applyDisp) goes
   * through it, so stored `d` never depends on a cross-field representative. */
  void parametricFrames(int level,
                        const litestl::util::Vector<litestl::math::float3> &base,
                        litestl::util::Vector<litestl::math::float3> &no,
                        litestl::util::Vector<litestl::math::float3> &ta);

  /** The raw level gridVerts table (G · (S+1)² vert ids, grid-major row-major
   * lattices) — the X3 normals kernel's lattice→vert map for geometric
   * normals over the displaced fine surface. */
  void levelGridVertsOut(int level, litestl::util::Vector<int> &out);

  static litestl::binding::types::Struct<Multires> *defineBindings();

  /** One composited channel: {store channel, effective weight}. Public so
   * file-static evaluation helpers can take spans of it. */
  struct ChannelMix {
    int channel = 0;
    float weight = 1.0f;
  };

private:
  /** {channel, effective weight} of every composited channel: channel 0 at
   * weight 1 plus each enabled, nonzero-weight layer row with a channel. */
  void compositeMix(litestl::util::Vector<ChannelMix> &out) const;
  /** The store channel backing settings row `li`, or -1. */
  int channelForLayer(int li) const;
  /** Drop every cached chain + resident slot and rematerialize the active
   * level (composite changed). Does NOT write back — callers fold first. */
  void refreshAfterLayerChange();

  /** Ensure the cached position chain is valid through `level`; returns it. */
  litestl::util::Vector<litestl::math::float3> &ensureChain(int level);
  /** Ensure `level`'s LevelPos carries its smooth base + lattice frames (the
   * chain through `level` must already be valid). Cheap when cached. */
  void ensureBaseAndFrames(int level);
  /** Re-express `pos` (dense by level vert id) as level-`level` store
   * displacement: disp = frameᵀ·(pos − base), base = stencil(prev chain),
   * frames on the smoothed base. Writes verts where `mask` is null or set.
   * The written channel absorbs the residual after every other composited
   * channel is subtracted: the edit target's channel when `toEditTarget`
   * (writeback of sculpting), else channel 0 (structural re-expression —
   * down-refit — which must never write a layer). */
  /** `grids` (optional) restricts the walk to that grid list — the grids-native
   * stroke path's touched set; null walks every grid. */
  void storeDispFromPositions(int level,
                              const litestl::util::Vector<litestl::math::float3> &pos,
                              const litestl::util::Vector<bool> *mask,
                              bool toEditTarget,
                              const litestl::util::Vector<int> *grids = nullptr);
  /** Shared tail of downRefit/propagateDown: commit a freshly fitted `coarse`
   * as level−1 displacement, re-express `target` (this level's unchanged
   * surface) against the new base, and refresh the affected caches + residents.
   * Returns the number of level−1 verts changed (0 leaves everything alone). */
  int commitCoarseFit(int level,
                      litestl::util::Vector<litestl::math::float3> &target,
                      litestl::util::Vector<litestl::math::float3> &coarse);
  bool dispNonZero(int level);
  void evictSlot(int index);
  void evictOverBudget();
  /** Free every grid domain for levels > `aboveLevel` (0 = all). Must run at
   * every point a level's LevelPos content is replaced or posCache_ storage
   * moves (resize) — domains alias LevelPos::pos by address. */
  void dropDomains(int aboveLevel);

  struct LevelPos {
    bool valid = false;
    litestl::util::Vector<litestl::math::float3> pos;
    /** Cached smooth base (stencil of the level below) + F3 frames on it —
     * writeback re-expression reuses these instead of rebuilding a temp level
     * mesh + frames every stroke. Valid only while the level below's positions
     * are unchanged; posIsBase marks a zero-disp materialization (pos == base,
     * base itself not yet copied out). */
    bool framesValid = false;
    bool posIsBase = false;
    litestl::util::Vector<litestl::math::float3> base, frameNo, frameTa;

    void reset()
    {
      valid = framesValid = posIsBase = false;
      pos.clear();
      base.clear();
      frameNo.clear();
      frameTa.clear();
    }
  };

  /** Per level (indexed BY level; [0] unused): this level's surface carries
   * detail the level below has not been told about. Set by a writeback that
   * changed anything, consumed by propagateDown on a downward level switch —
   * which is what keeps an up-then-down round trip from restricting a coarse
   * level that is already current, since R∘stencil is not the identity.
   * Describes store content, so invalidateAll (a cache drop) leaves it — but a
   * wholesale store REPLACEMENT must reset it, and an undo snapshot must carry
   * it, which is what the downPropDebt accessors above are for. */
  litestl::util::Vector<bool> downPropPending_;

  mesh::Mesh *cage_ = nullptr;
  int activeLevel_ = 0; // 0 = the cage itself (no materialized level)
  uint64_t useCounter_ = 0;
  litestl::util::Vector<LevelPos> posCache_; // [0] = level 1
  litestl::util::Vector<MultiresSlot> slots_;
  litestl::util::Vector<GridLevelDomain *> domains_; // [0] = level 1; sparse
  uint64_t domainGen_ = 0;                           // see domainGeneration()
  uint32_t slotStaleMask_ = 0;                       // bit per level; see slotStale()
  GridDrawSource *drawSource_ = nullptr;             // registry-owned backref
  MultiresAttrs gridAttrs_{*this};
};

} // namespace sculptcore::subdiv
