#pragma once

/** Grids-native stroke undo (grids-native brush path, G2).
 *
 * Per stroke step, the first dab to touch a leaf snapshots that leaf's
 * owned-vert PRE-positions (and pre-mask for mask strokes) plus the write
 * target's store blocks of the leaf's grids — CCG-style block undo,
 * O(touched leaves) per stroke. Undo/redo SWAP the blocks back in, so
 * positions and store bytes restore bit-exact (copies, not re-derivations)
 * and one snapshot serves both directions; each seek refreshes the captured
 * leaves' normals + bounds, invalidates finer chain levels, and restores the
 * level's down-propagation debt flag.
 *
 * Seam coverage: a touched seam vert's store write lands in every grid that
 * contains it, including grids of neighboring leaves — those leaves are
 * always part of the dab's node set (their padded AABBs contain the vert), so
 * their capture covers the write.
 *
 * History is valid while the domain/tree it was captured against stays alive
 * (level switches fold through the store-blob fallback — the addon phase). */

#include "grid_domain.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <span>

namespace sculptcore::subdiv {

struct GridTree;

struct GridStrokeLog {
  using float3 = litestl::math::float3;
  template <typename T> using Vector = litestl::util::Vector<T>;

  /** Bind to a domain (and its tree). Clears history. */
  void attach(GridLevelDomain *d);

  /** Open a step (truncates any redo branch). */
  void beginStep();
  /** First-touch capture of `leaf` into the open step: owned pre-positions
   * when `positions`, owned pre-mask when `maskToo`. Store blocks are NOT
   * captured here — the store is untouched until the stroke-end fold, so
   * captureGrids() defers them to exactly the touched set. */
  void captureLeaf(int leaf, bool positions, bool maskToo);
  /** Capture `channel`'s store blocks for `grids` (the stroke's touched-grid
   * set), called at stroke end BEFORE the writeback/flush overwrites them. */
  void captureGrids(std::span<const int> grids, int channel);
  /** Close the step; `postDebt` is downPropDebt(level) after the stroke's
   * writeback. Empty steps are dropped. */
  void endStep(bool postDebt);

  bool canUndo() const
  {
    return cursor_ > 0;
  }
  bool canRedo() const
  {
    return cursor_ < int(steps_.size());
  }
  bool undo();
  bool redo();

  int stepCount() const
  {
    return int(steps_.size());
  }
  /** Total captured bytes across the history (the undo-footprint gate). */
  size_t bytes() const;

private:
  struct LeafSnap {
    int leaf = -1;
    bool hasPos = false, hasMask = false;
    Vector<float3> pos;  // owned-vert order
    Vector<float> mask;  // owned-vert order
  };
  struct GridBlock {
    int grid = -1;
    int channel = 0;
    Vector<float> data; // (S+1)^2 * elemSize floats, the store block layout
  };
  struct Step {
    Vector<LeafSnap> leaves;
    Vector<GridBlock> blocks;
    bool preDebt = false;
    bool postDebt = false;
  };

  void captureGridBlock(Step &s, int grid, int channel);
  /** Swap a step's snapshots with the live domain/store state and refresh
   * normals/bounds/chain — symmetric, so it serves undo and redo alike. */
  void applySwap(Step &s);

  GridLevelDomain *d_ = nullptr;
  GridTree *tree_ = nullptr;
  Vector<Step> steps_;
  int cursor_ = 0;    // steps applied
  bool open_ = false; // a step is being captured
  /** First-touch dedup stamps for the open step. */
  Vector<uint32_t> leafStamp_;
  Vector<uint32_t> gridStamp_;
  uint32_t gen_ = 0;
};

} // namespace sculptcore::subdiv
