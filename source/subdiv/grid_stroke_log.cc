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
  stepSerial_++;
  d_ = d;
  tree_ = d ? d->ensureTree() : nullptr;
  steps_.clear();
  cursor_ = 0;
  open_ = false;
  leafCaptured_.clear();
  captured_.clear();
  debtCaptured_.clear();
  leafCaptured_.clear();
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
  snapshotAttrDebt(s.preAttrDebt);
  for (const auto &debt : s.preAttrDebt) {
    debtCaptured_.add(debt.levelToken);
  }
  open_ = true;
  stepSerial_++;
}

GridStrokeLog::ChannelIdentity GridStrokeLog::identity(int channel, int level) const
{
  const auto &ch = d_->multires()->store.channels_[channel];
  return {ch.name, ch.incarnation, ch.levels[level - 1].incarnation, level};
}

int GridStrokeLog::resolve(const ChannelIdentity &id) const
{
  const auto &store = d_->multires()->store;
  int channel = store.findChannel(id.channel);
  if (channel < 0 || id.level < 1 || id.level > store.levelCount()) {
    return -1;
  }
  const auto &ch = store.channels_[channel];
  return ch.incarnation == id.channelToken &&
                 ch.levels[id.level - 1].incarnation == id.levelToken
             ? channel
             : -1;
}

GridStrokeLog::GridBlock GridStrokeLog::captureBlock(int grid, int channel, int level)
{
  auto &store = d_->multires()->store;
  int floats = store.channelElemsPerGrid(level, channel) * store.channelElemSize(channel);
  GridBlock b;
  static_cast<ChannelIdentity &>(b) = identity(channel, level);
  b.grid = grid;
  b.data.resize(floats);
  const float *src = store.elem(level, channel, grid, 0, 0);
  std::memcpy(b.data.data(), src, size_t(floats) * sizeof(float));
  return b;
}

void GridStrokeLog::captureGridBlock(Step &s, int grid, int channel)
{
  const auto id = identity(channel, d_->level());
  if (debtCaptured_.add(id.levelToken)) {
    AttrDebt debt;
    static_cast<ChannelIdentity &>(debt) = id;
    debt.debt = d_->multires()->store.channelLevelDebt(d_->level(), channel);
    s.preAttrDebt.append(std::move(debt));
  }
  if (!captured_[id.levelToken].add(grid)) {
    return;
  }
  s.blocks.append(captureBlock(grid, channel, d_->level()));
}

void GridStrokeLog::captureFinerMask(std::span<const int> grids, int channel)
{
  Assert(open_, "captureFinerMask inside a step");
  auto &store = d_->multires()->store;
  Assert(channel >= 0 && channel < store.channelCount(),
         "mask channel exists before fold");
  const auto &ch = store.channels_[channel];
  Assert(ch.name == litestl::util::string(GridLevelDomain::kMaskChannelName) &&
             ch.type == mesh::AttrType::FLOAT && ch.domain == GridElemDomain::Vertex &&
             ch.floatsPerElem == 1,
         "compatible mask channel");
  if (grids.empty()) {
    return;
  }
  Step &s = steps_.last();
  for (int level = d_->level() + 1; level <= store.levelCount(); level++) {
    const auto id = identity(channel, level);
    FinerMask *snap = nullptr;
    for (auto &entry : s.finerMask) {
      if (entry.levelToken == id.levelToken) {
        snap = &entry;
        break;
      }
    }
    if (!snap) {
      s.finerMask.append(FinerMask());
      snap = &s.finerMask.last();
      static_cast<ChannelIdentity &>(*snap) = id;
      snap->debt = store.channelLevelDebt(level, channel);
      snap->wholeLevel = !store.channelLevelAllocated(level, channel);
      if (snap->wholeLevel) {
        // This copy holds no buffers and preserves the actual empty metadata.
        snap->state = store.channels_[channel].levels[level - 1];
      }
    }
    if (!snap->wholeLevel) {
      for (int grid : grids) {
        if (!snap->captured.contains(grid)) {
          snap->captured.add(grid);
          snap->blocks.append(captureBlock(grid, channel, level));
        }
      }
    }
  }
}

void GridStrokeLog::captureLeaf(int leaf, bool positions, bool maskToo)
{
  Assert(open_, "captureLeaf inside a step");
  Step &s = steps_.last();

  const ChannelIdentity maskId =
      maskToo ? identity(d_->ensureMaskChannel(), d_->level()) : ChannelIdentity();
  auto &captured = leafCaptured_[leaf];
  const bool needPos = positions && !captured.position;
  const bool needMask = maskToo && captured.masks.add(maskId.levelToken);
  captured.position |= positions;
  if (!needPos && !needMask) {
    return;
  }
  s.leaves.append(LeafSnap());
  LeafSnap *snap = &s.leaves.last();
  snap->leaf = leaf;
  const GridTree::Leaf &tl = tree_->leaves[leaf];
  if (needPos) {
    snap->hasPos = true;
    snap->pos.resize(tl.ownedVerts.size());
    for (int i = 0; i < int(tl.ownedVerts.size()); i++) {
      snap->pos[i] = d_->pos()[tl.ownedVerts[i]];
    }
  }
  if (needMask) {
    snap->maskIdentity = maskId;
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
  leafCaptured_.clear();
  captured_.clear();
  debtCaptured_.clear();
  Step &s = steps_.last();
  if (s.leaves.size() == 0 && s.blocks.size() == 0 && s.finerMask.isEmpty()) {
    steps_.pop_back();
    return;
  }
  for (auto &fine : s.finerMask) {
    fine.captured.clear();
    const int channel = resolve(fine);
    if (fine.wholeLevel && channel >= 0) {
      const auto &store = d_->multires()->store;
      if (store.channelLevelAllocated(fine.level, channel)) {
        fine.owningReserve = size_t(store.gridCount()) *
                             store.channelElemsPerGrid(fine.level, channel) *
                             sizeof(float);
      }
    }
  }
  s.postDebt = postDebt;
  snapshotAttrDebt(s.postAttrDebt);
  cursor_ = int(steps_.size());
}

void GridStrokeLog::snapshotAttrDebt(Vector<AttrDebt> &out)
{
  const auto &store = d_->multires()->store;
  out.clear();
  for (int c = 0; c < store.channelCount(); c++) {
    AttrDebt debt;
    static_cast<ChannelIdentity &>(debt) = identity(c, d_->level());
    debt.debt = store.channelLevelDebt(d_->level(), c);
    out.append(std::move(debt));
  }
}

void GridStrokeLog::applyAttrDebt(const Vector<AttrDebt> &debts)
{
  for (const auto &debt : debts) {
    int channel = resolve(debt);
    if (channel >= 0) {
      d_->multires()->store.setChannelLevelDebt(debt.level, channel, debt.debt);
    }
  }
}

void GridStrokeLog::applySwap(Step &s)
{
  Multires *mr = d_->multires();
  int level = d_->level();
  mr->store.ensureLevelResident(level);

  Vector<int> touchedVerts;
  bool maskChanged = !s.finerMask.isEmpty();
  litestl::util::Map<int, Vector<int>> swappedSession;
  for (LeafSnap &ls : s.leaves) {
    const GridTree::Leaf &tl = tree_->leaves[ls.leaf];
    if (ls.hasPos) {
      for (int i = 0; i < int(tl.ownedVerts.size()); i++) {
        std::swap(ls.pos[i], d_->pos()[tl.ownedVerts[i]]);
        touchedVerts.append(tl.ownedVerts[i]);
      }
    }
    if (ls.hasMask && resolve(ls.maskIdentity) >= 0) {
      maskChanged = true;
      for (int i = 0; i < int(tl.ownedVerts.size()); i++) {
        std::swap(ls.mask[i], d_->mask[tl.ownedVerts[i]]);
      }
    }
  }
  for (GridBlock &b : s.blocks) {
    const int channel = resolve(b);
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
      swappedSession[channel].append(b.grid);
    }
  }

  mr->invalidateAbove(level);
  for (auto &snap : s.finerMask) {
    const int channel = resolve(snap);
    if (channel < 0) {
      continue;
    }
    auto &live = mr->store.channels_[channel].levels[snap.level - 1];
    if (snap.wholeLevel) {
      std::swap(snap.state, live);
    } else {
      for (auto &block : snap.blocks) {
        float *dst = mr->store.elem(snap.level, channel, block.grid, 0, 0);
        for (size_t i = 0; i < block.data.size(); i++) {
          std::swap(block.data[i], dst[i]);
        }
      }
      std::swap(snap.debt, live.downPending);
    }
  }

  if (touchedVerts.size() > 0) {
    d_->refreshNormals(std::span<const int>(touchedVerts.data(), touchedVerts.size()));
  }
  tree_->refreshVertexBounds(
      std::span<const int>(touchedVerts.data(), touchedVerts.size()));
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
  for (auto &entry : swappedSession) {
    const auto &name = mr->store.channelName(entry.key);
    auto &grids = swappedSession[entry.key];
    mr->gridAttrs().refreshSamplesFromChannel(
        name, level, grids.data(), int(grids.size()));
    if (GridDrawSource *ds = mr->drawSource()) {
      ds->markGrids(std::span<const int>(grids.data(), grids.size()));
    }
  }
  if (maskChanged) {
    mr->noteMaskChange();
  }
}

bool GridStrokeLog::undo()
{
  if (!canUndo() || open_) {
    return false;
  }
  Step &s = steps_[cursor_ - 1];
  applySwap(s);
  d_->multires()->setDownPropDebt(d_->level(), s.preDebt);
  applyAttrDebt(s.preAttrDebt);
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
  applyAttrDebt(s.postAttrDebt);
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
    for (const auto &snap : s.finerMask) {
      for (const auto &block : snap.blocks) {
        n += block.data.size() * sizeof(float);
      }
      for (const auto &chunk : snap.state.chunks) {
        n += chunk.size() * sizeof(float);
      }
      n += snap.state.evicted.size();
    }
  }
  return n;
}

size_t GridStrokeLog::retainedBytes() const
{
  size_t n = bytes();
  for (const Step &s : steps_) {
    n += sizeof(Step);
    for (const auto &leaf : s.leaves) {
      n += sizeof(LeafSnap) + leaf.maskIdentity.channel.capacity() + 1;
    }
    for (const auto &b : s.blocks) {
      n += sizeof(GridBlock) + b.channel.capacity() + 1;
    }
    for (const auto &fine : s.finerMask) {
      n += sizeof(FinerMask) + fine.channel.capacity() + 1;
      n += fine.state.chunks.size() * sizeof(Vector<float>);
      // Charge stroke-created levels when the step closes: Blender's undo
      // budget receives a fixed size at push, before undo moves them here.
      size_t held = fine.state.evicted.size();
      for (const auto &chunk : fine.state.chunks) {
        held += chunk.size() * sizeof(float);
      }
      if (fine.owningReserve > held) {
        n += fine.owningReserve - held;
      }
      for (const auto &b : fine.blocks) {
        n += sizeof(GridBlock) + b.channel.capacity() + 1;
      }
    }
    for (const auto &debt : s.preAttrDebt) {
      n += sizeof(AttrDebt) + debt.channel.capacity() + 1;
    }
    for (const auto &debt : s.postAttrDebt) {
      n += sizeof(AttrDebt) + debt.channel.capacity() + 1;
    }
  }
  return n;
}

} // namespace sculptcore::subdiv
