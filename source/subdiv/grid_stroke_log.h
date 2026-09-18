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
 * Seam coverage: stroke-end block capture follows each moved vert's occurrence
 * grids,
 * including adjacent leaves that were not selected by the dab. Undo
 * refreshes bounds
 * for every occurrence leaf of restored positions.
 * History is valid while the
 * domain/tree it was captured against stays alive (level switches fold through the
 * store-blob fallback — the addon phase). */

#include "grid_domain.h"

#include "litestl/math/vector.h"
#include "litestl/util/map.h"
#include "litestl/util/set.h"
#include "litestl/util/string.h"
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
  /** Capture the finer mask blocks that the fold will prolongate into. */
  void captureFinerMask(std::span<const int> grids, int channel);
  /** Close the step; `postDebt` is downPropDebt(level) after the stroke's
   * writeback. Empty steps are dropped. */
  void endStep(bool postDebt);

  /** Identifies the current capture transaction; zero means no open step. */
  uint64_t openStepSerial() const
  {
    return open_ ? stepSerial_ : 0;
  }

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
  /** Evict the oldest APPLIED step (undo-limiter truncation from the front).
   * Refused while a step is open, when empty, or when everything is undone
   * (the front step would be redo history). Host absolute cursors stay
   * valid: eviction removes a step below the deepest reachable seek, so
   * seeks simply stop earlier. */
  bool dropOldest();

  int stepCount() const
  {
    return int(steps_.size());
  }
  /** The domain this log is bound to (null when unattached). */
  GridLevelDomain *domain() const
  {
    return d_;
  }
  /** Captured value payload, excluding record/container overhead. */
  size_t bytes() const;
  /** Retained values and records; excludes allocator capacity/slack. */
  size_t retainedBytes() const;

private:
  struct ChannelIdentity {
    litestl::util::string channel;
    uint64_t channelToken = 0, levelToken = 0;
    int level = 0;
  };
  struct AttrDebt : ChannelIdentity {
    bool debt = false;
  };
  struct LeafSnap {
    int leaf = -1;
    bool hasPos = false, hasMask = false;
    Vector<float3> pos; // owned-vert order
    Vector<float> mask; // owned-vert order
    ChannelIdentity maskIdentity;
  };
  struct GridBlock : ChannelIdentity {
    int grid = -1;
    // By NAME, not index: removeChannel shifts every later channel down, so an
    // index captured before it would swap bytes into the wrong column (or off
    // the end of the store) afterwards.
    Vector<float> data; // elemsPerGrid * elemSize floats, the store block layout
  };
  struct FinerMask : ChannelIdentity {
    bool wholeLevel = false, debt = false;
    size_t owningReserve = 0;
    GridsStore::LevelData state;
    Vector<GridBlock> blocks;
    litestl::util::Set<int> captured;
  };
  struct Step {
    Vector<LeafSnap> leaves;
    Vector<GridBlock> blocks;
    Vector<FinerMask> finerMask;
    bool preDebt = false;
    bool postDebt = false;
    // Identity-qualified booleans preserve replacement channels' debt.
    Vector<AttrDebt> preAttrDebt, postAttrDebt;
  };
  struct LeafCapture {
    bool position = false;
    litestl::util::Set<uint64_t> masks;
  };

  void captureGridBlock(Step &s, int grid, int channel);
  ChannelIdentity identity(int channel, int level) const;
  int resolve(const ChannelIdentity &id) const;
  GridBlock captureBlock(int grid, int channel, int level);
  void snapshotAttrDebt(Vector<AttrDebt> &out);
  void applyAttrDebt(const Vector<AttrDebt> &debts);
  /** Swap a step's snapshots with the live domain/store state and refresh
   * normals/bounds/chain — symmetric, so it serves undo and redo alike. */
  void applySwap(Step &s);

  GridLevelDomain *d_ = nullptr;
  GridTree *tree_ = nullptr;
  Vector<Step> steps_;
  int cursor_ = 0;    // steps applied
  bool open_ = false; // a step is being captured
  uint64_t stepSerial_ = 0;
  /** First-touch dedup stamps for the open step. */
  litestl::util::Map<int, LeafCapture> leafCaptured_;
  litestl::util::Map<uint64_t, litestl::util::Set<int>> captured_;
  litestl::util::Set<uint64_t> debtCaptured_;
};

} // namespace sculptcore::subdiv
