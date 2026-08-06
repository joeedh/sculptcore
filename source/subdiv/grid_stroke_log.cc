#include "grid_stroke_log.h"

#include "grid_tree.h"
#include "multires.h"

#include "litestl/util/assert.h"

#include <cstring>
#include <utility>

namespace sculptcore::subdiv {

using litestl::math::float3;
using litestl::util::Assert;

void GridStrokeLog::attach(GridLevelDomain *d)
{
  d_ = d;
  tree_ = d ? d->ensureTree() : nullptr;
  steps_.clear();
  cursor_ = 0;
  open_ = false;
  gen_ = 0;
  leafStamp_.clear();
  gridStamp_.clear();
  if (d_) {
    leafStamp_.resize(tree_->leaves.size());
    gridStamp_.resize(d_->gridCount());
    for (int i = 0; i < int(leafStamp_.size()); i++) {
      leafStamp_[i] = 0;
    }
    for (int i = 0; i < int(gridStamp_.size()); i++) {
      gridStamp_[i] = 0;
    }
  }
}

void GridStrokeLog::beginStep()
{
  Assert(d_ && !open_, "log attached, no step open");
  // A fresh step invalidates any redo branch (linear history, meshlog-style).
  while (int(steps_.size()) > cursor_) {
    steps_.pop_back();
  }
  steps_.append(Step());
  Step &s = steps_.last();
  s.preDebt = d_->multires()->downPropDebt(d_->level());
  open_ = true;
  gen_++;
}

void GridStrokeLog::captureGridBlock(Step &s, int grid, int channel)
{
  if (gridStamp_[grid] == gen_) {
    // Already captured this stroke — but possibly for a different channel
    // (a stroke writing both positions and mask). Check before skipping.
    for (const GridBlock &b : s.blocks) {
      if (b.grid == grid && b.channel == channel) {
        return;
      }
    }
  }
  gridStamp_[grid] = gen_;

  Multires *mr = d_->multires();
  int level = d_->level();
  mr->store.ensureLevelResident(level);
  int w = d_->gridSide() + 1;
  int floats = w * w * mr->store.channelElemSize(channel);
  GridBlock b;
  b.grid = grid;
  b.channel = channel;
  b.data.resize(floats);
  const float *src = mr->store.elem(level, channel, grid, 0, 0);
  std::memcpy(b.data.data(), src, size_t(floats) * sizeof(float));
  s.blocks.append(std::move(b));
}

void GridStrokeLog::captureLeaf(int leaf, bool positions, bool maskToo)
{
  Assert(open_, "captureLeaf inside a step");
  Step &s = steps_.last();

  LeafSnap *snap = nullptr;
  if (leafStamp_[leaf] == gen_) {
    for (LeafSnap &ls : s.leaves) {
      if (ls.leaf == leaf) {
        snap = &ls;
        break;
      }
    }
    if (snap && snap->hasPos >= positions && snap->hasMask >= maskToo) {
      return;
    }
  }
  leafStamp_[leaf] = gen_;
  if (!snap) {
    s.leaves.append(LeafSnap());
    snap = &s.leaves.last();
    snap->leaf = leaf;
  }

  const GridTree::Leaf &tl = tree_->leaves[leaf];
  if (positions && !snap->hasPos) {
    snap->hasPos = true;
    snap->pos.resize(tl.ownedVerts.size());
    for (int i = 0; i < int(tl.ownedVerts.size()); i++) {
      snap->pos[i] = d_->pos()[tl.ownedVerts[i]];
    }
  }
  if (maskToo && !snap->hasMask) {
    snap->hasMask = true;
    snap->mask.resize(tl.ownedVerts.size());
    for (int i = 0; i < int(tl.ownedVerts.size()); i++) {
      snap->mask[i] = d_->mask[tl.ownedVerts[i]];
    }
  }
}

void GridStrokeLog::captureGrids(std::span<const int> grids, int channel)
{
  Assert(open_, "captureGrids inside a step");
  if (channel < 0 || channel >= d_->multires()->store.channelCount()) {
    return;
  }
  Step &s = steps_.last();
  for (int g : grids) {
    captureGridBlock(s, g, channel);
  }
}

void GridStrokeLog::endStep(bool postDebt)
{
  Assert(open_, "endStep pairs with beginStep");
  open_ = false;
  Step &s = steps_.last();
  if (s.leaves.size() == 0 && s.blocks.size() == 0) {
    steps_.pop_back();
    return;
  }
  s.postDebt = postDebt;
  cursor_ = int(steps_.size());
}

void GridStrokeLog::applySwap(Step &s)
{
  Multires *mr = d_->multires();
  int level = d_->level();
  mr->store.ensureLevelResident(level);

  Vector<int> touchedLeaves;
  Vector<int> touchedVerts;
  for (LeafSnap &ls : s.leaves) {
    const GridTree::Leaf &tl = tree_->leaves[ls.leaf];
    touchedLeaves.append(ls.leaf);
    if (ls.hasPos) {
      for (int i = 0; i < int(tl.ownedVerts.size()); i++) {
        std::swap(ls.pos[i], d_->pos()[tl.ownedVerts[i]]);
        touchedVerts.append(tl.ownedVerts[i]);
      }
    }
    if (ls.hasMask) {
      for (int i = 0; i < int(tl.ownedVerts.size()); i++) {
        std::swap(ls.mask[i], d_->mask[tl.ownedVerts[i]]);
      }
    }
  }
  int w = d_->gridSide() + 1;
  for (GridBlock &b : s.blocks) {
    float *dst = mr->store.elem(level, b.channel, b.grid, 0, 0);
    int floats = w * w * mr->store.channelElemSize(b.channel);
    for (int i = 0; i < floats; i++) {
      std::swap(b.data[i], dst[i]);
    }
  }

  if (touchedVerts.size() > 0) {
    d_->refreshNormals(std::span<const int>(touchedVerts.data(), touchedVerts.size()));
  }
  tree_->refreshBounds(std::span<const int>(touchedLeaves.data(), touchedLeaves.size()));
  // Finer levels derive from this one's positions; the swapped state is new
  // to them either way.
  mr->invalidateAbove(level);
}

bool GridStrokeLog::undo()
{
  if (!canUndo() || open_) {
    return false;
  }
  Step &s = steps_[cursor_ - 1];
  applySwap(s);
  d_->multires()->setDownPropDebt(d_->level(), s.preDebt);
  cursor_--;
  return true;
}

bool GridStrokeLog::redo()
{
  if (!canRedo() || open_) {
    return false;
  }
  Step &s = steps_[cursor_];
  applySwap(s);
  d_->multires()->setDownPropDebt(d_->level(), s.postDebt);
  cursor_++;
  return true;
}

bool GridStrokeLog::dropOldest()
{
  // Only an applied step may be evicted: cursor_ counts applied steps from
  // the front, so cursor_ == 0 means the front step is redo-only history and
  // dropping it would corrupt the redo chain, not trim the undo tail.
  if (open_ || steps_.size() == 0 || cursor_ < 1) {
    return false;
  }
  steps_.pop_front();
  cursor_--;
  return true;
}

size_t GridStrokeLog::bytes() const
{
  size_t n = 0;
  for (const Step &s : steps_) {
    for (const LeafSnap &ls : s.leaves) {
      n += ls.pos.size() * sizeof(float3) + ls.mask.size() * sizeof(float);
    }
    for (const GridBlock &b : s.blocks) {
      n += b.data.size() * sizeof(float);
    }
  }
  return n;
}

} // namespace sculptcore::subdiv
