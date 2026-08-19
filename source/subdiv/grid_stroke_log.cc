#include "grid_stroke_log.h"

#include "grid_draw_source.h"

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
  gridChannels_.clear();
  if (d_) {
    leafStamp_.resize(tree_->leaves.size());
    gridStamp_.resize(d_->gridCount());
    gridChannels_.resize(d_->gridCount());
    for (int i = 0; i < int(leafStamp_.size()); i++) {
      leafStamp_[i] = 0;
    }
    for (int i = 0; i < int(gridStamp_.size()); i++) {
      gridStamp_[i] = 0;
      gridChannels_[i] = 0;
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
  // A stroke can capture the same grid on several channels (positions + mask),
  // so the stamp carries a bitmask of the channels taken. Scanning s.blocks for
  // that instead made the dedup quadratic over the touched-grid set.
  const uint32_t bit = channel < 32 ? uint32_t(1) << channel : 0;
  if (gridStamp_[grid] != gen_) {
    gridStamp_[grid] = gen_;
    gridChannels_[grid] = 0;
  } else if (bit) {
    if (gridChannels_[grid] & bit) {
      return;
    }
  } else {
    for (const GridBlock &b : s.blocks) { // channel >= 32: no bit to spend
      if (b.grid == grid && b.channel == d_->multires()->store.channelName(channel)) {
        return;
      }
    }
  }
  gridChannels_[grid] |= bit;

  Multires *mr = d_->multires();
  int level = d_->level();
  mr->store.ensureLevelResident(level);
  int floats = mr->store.channelElemsPerGrid(level, channel) *
               mr->store.channelElemSize(channel);
  GridBlock b;
  b.grid = grid;
  b.channel = mr->store.channelName(channel);
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
  Vector<GridBlock *> swappedSession;
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
  for (GridBlock &b : s.blocks) {
    const int channel = mr->store.findChannel(b.channel);
    if (channel < 0) {
      continue; // the channel was dropped since capture: nothing to restore to
    }
    const int floats = mr->store.channelElemsPerGrid(level, channel) *
                       mr->store.channelElemSize(channel);
    if (floats != int(b.data.size())) {
      continue; // re-added at another size or domain — not the block's column
    }
    float *dst = mr->store.elem(level, channel, b.grid, 0, 0);
    for (int i = 0; i < floats; i++) {
      std::swap(b.data[i], dst[i]);
    }
    if (mr->store.channelAuthored(channel)) {
      // An authored channel is what the draw path's derived samples mirror, so
      // the swap has to be pushed back out to them (no-op for the rest).
      swappedSession.append(&b);
    }
  }

  if (touchedVerts.size() > 0) {
    d_->refreshNormals(std::span<const int>(touchedVerts.data(), touchedVerts.size()));
  }
  tree_->refreshBounds(std::span<const int>(touchedLeaves.data(), touchedLeaves.size()));
  if (GridDrawSource *ds = mr->drawSource()) {
    // Drawn pos/no/mask of the swapped leaves changed — mark by owned verts
    // (occurrence mapping also reaches the neighbor cells reading them).
    Vector<int> marked;
    for (LeafSnap &ls : s.leaves) {
      const GridTree::Leaf &tl = tree_->leaves[ls.leaf];
      for (int v : tl.ownedVerts) {
        marked.append(v);
      }
    }
    ds->markVerts(std::span<const int>(marked.data(), marked.size()));
  }
  /* One pass per distinct session channel: the derived samples the draw path
   * reads mirror the channel, so a swap has to be pushed back out to them. */
  for (size_t i = 0; i < swappedSession.size(); i++) {
    const litestl::util::string &name = swappedSession[i]->channel;
    bool seen = false;
    for (size_t j = 0; j < i && !seen; j++) {
      seen = swappedSession[j]->channel == name;
    }
    if (seen) {
      continue;
    }
    Vector<int> grids;
    for (GridBlock *b : swappedSession) {
      if (b->channel == name) {
        grids.append(b->grid);
      }
    }
    mr->gridAttrs().refreshSamplesFromChannel(name, level, grids.data(), int(grids.size()));
    if (GridDrawSource *ds = mr->drawSource()) {
      ds->markGrids(std::span<const int>(grids.data(), grids.size()));
    }
  }
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
