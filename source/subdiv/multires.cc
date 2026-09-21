#include "multires.h"

#include "grid_domain.h"
#include "grid_draw_source.h"
#include "multires_tuning.h"

#include "vdm/vdm_store.h"

#include "displace/frames.h"
#include "mesh/mesh.h"
#include "mesh/mesh_proxy.h"
#include "spatial/spatial.h"
#include "spatial/spatial_base.h"

#include "litestl/util/alloc.h"
#include "litestl/util/assert.h"
#include "litestl/util/task.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace litestl;
using litestl::math::float2;
using litestl::math::float3;
using litestl::math::float4;
using litestl::util::Assert;
using litestl::util::Vector;

namespace sculptcore::subdiv {

Multires::~Multires()
{
  if (drawSource_) {
    // Registry-owned; it keeps its buffers (the host may still poll) but
    // must stop touching this stack.
    drawSource_->onMultiresDestroyed();
    drawSource_ = nullptr;
  }
  dropDomains(0);
  for (int i = int(slots_.size()) - 1; i >= 0; i--) {
    evictSlot(i);
  }
}

void Multires::dropDomains(int aboveLevel)
{
  for (int l = aboveLevel + 1; l <= int(domains_.size()); l++) {
    if (domains_[l - 1]) {
      alloc::Delete(domains_[l - 1]);
      domains_[l - 1] = nullptr;
      domainGen_++;
    }
  }
}

GridLevelDomain *Multires::gridDomain(int level)
{
  Assert(level >= 1 && level <= maxLevel(), "level in refined range");
  if (int(domains_.size()) < maxLevel()) {
    int old = int(domains_.size());
    domains_.resize(maxLevel());
    for (int i = old; i < maxLevel(); i++) {
      domains_[i] = nullptr;
    }
  }
  if (!domains_[level - 1]) {
    GridLevelDomain *d = alloc::New<GridLevelDomain>("grid level domain");
    d->build(*this, level);
    // The domain edits LevelPos::pos in place, so the base/frames must be
    // materialized NOW: on a zero-disp level (posIsBase) a lazy
    // ensureBaseAndFrames after the first edit would copy the already-edited
    // positions as the base and the writeback would derive zero displacement
    // — the plan's chain-cache-coupling risk, closed here.
    ensureBaseAndFrames(level);
    domains_[level - 1] = d;
    domainGen_++;
  }
  return domains_[level - 1];
}

void Multires::refreshFinerMaskMirrors(int aboveLevel)
{
  for (int l = aboveLevel + 1; l <= int(domains_.size()); l++) {
    if (domains_[l - 1]) {
      domains_[l - 1]->syncMaskFromStore();
    }
  }
}

void Multires::init(mesh::Mesh &cage, int maxLevel)
{
  dropDomains(0);
  for (int i = int(slots_.size()) - 1; i >= 0; i--) {
    evictSlot(i);
  }
  posCache_.clear();
  activeLevel_ = 0;
  cage_ = &cage;
  gridAttrs_.invalidateAll();

  refiner.refine(cage, maxLevel);
  refiner.releaseMeshes();

  store.buildFromCage(cage);
  for (int i = 0; i < maxLevel; i++) {
    store.addLevel();
  }
  Assert(store.gridCount() == refiner.gridCount(), "store/refiner grid enumeration");

  posCache_.resize(maxLevel);
  downPropPending_.resize(maxLevel + 1);
  for (int l = 0; l <= maxLevel; l++) {
    downPropPending_[l] = false;
  }
}

mesh::Mesh *Multires::buildLevelTopo(int level)
{
  SubdivLevel &lvl = refiner.levels[level - 1];
  mesh::Mesh *m = alloc::New<mesh::Mesh>("multires level");

  for (int i = 0; i < lvl.vertCount; i++) {
    int nv = m->make_vertex(float3());
    Assert(nv == i, "multires level verts allocate densely");
  }

  // One quad per grid cell; each level face is exactly one cell, and the
  // (u,v)->(u+1,v)->... cell order matches the refiner's child-quad winding.
  int S = lvl.gridSide, w = S + 1;
  for (int g = 0; g < refiner.gridCount(); g++) {
    const int *gv = &lvl.gridVerts[g * w * w];
    for (int v = 0; v < S; v++) {
      for (int u = 0; u < S; u++) {
        int quad[4] = {gv[v * w + u],
                       gv[v * w + u + 1],
                       gv[(v + 1) * w + u + 1],
                       gv[(v + 1) * w + u]};
        m->make_face(std::span<int>(quad, 4));
      }
    }
  }
  return m;
}

void Multires::dabGrids(int level, const float *dabs, int dabCount, Vector<int> &out)
{
  MultiresSlot *slot = findSlot(level);
  if (!dabs || dabCount <= 0 || !slot || !slot->mesh || !slot->tree) {
    return;
  }
  const int S = refiner.levels[level - 1].gridSide;
  const int cells = S * S, gridCount = refiner.gridCount();
  if (cells <= 0 || slot->mesh->f.count != gridCount * cells) {
    return; // not this level's grid mesh, so face id / S² is not a grid id
  }
  Vector<uint8_t> seen;
  seen.resize(size_t(gridCount));
  for (size_t i = 0; i < seen.size(); i++) {
    seen[i] = 0;
  }
  for (int g : out) {
    if (g >= 0 && g < gridCount) {
      seen[g] = 1;
    }
  }
  Vector<spatial::SpatialNode *> nodes;
  for (int i = 0; i < dabCount; i++) {
    nodes.clear();
    const math::float3 co(dabs[i * 4], dabs[i * 4 + 1], dabs[i * 4 + 2]);
    if (!slot->tree->filterNodes(co, dabs[i * 4 + 3], nodes)) {
      continue;
    }
    // Leaf granularity: filterNodes is an AABB test, so this takes a few grids
    // the dab did not reach. Re-deriving one is idempotent, and the alternative
    // (a per-vert distance test) buys nothing the collapse can observe.
    for (spatial::SpatialNode *node : nodes) {
      for (int f : node->data->unique_faces) {
        const int g = f / cells;
        if (g >= 0 && g < gridCount && !seen[g]) {
          seen[g] = 1;
          out.append(g);
        }
      }
    }
  }
}

MultiresSlot *Multires::findSlot(int level)
{
  for (MultiresSlot &s : slots_) {
    if (s.level == level) {
      return &s;
    }
  }
  return nullptr;
}

void Multires::evictSlot(int index)
{
  MultiresSlot &s = slots_[index];
  if (s.tree) {
    alloc::Delete(s.tree);
  }
  if (s.mesh) {
    alloc::Delete(s.mesh);
  }
  slots_.remove_at(index, /*swap_end_only=*/false);
}

void Multires::evictOverBudget()
{
  while (int(slots_.size()) > (lruBudget < 1 ? 1 : lruBudget)) {
    int oldest = -1;
    for (int i = 0; i < int(slots_.size()); i++) {
      if (slots_[i].level == activeLevel_) {
        continue;
      }
      if (oldest < 0 || slots_[i].lastUse < slots_[oldest].lastUse) {
        oldest = i;
      }
    }
    if (oldest < 0) {
      return;
    }
    evictSlot(oldest);
  }
}

void Multires::storeDispFromPositions(int level,
                                      const Vector<float3> &pos,
                                      const Vector<bool> *mask,
                                      bool toEditTarget,
                                      const Vector<int> *grids)
{
  SubdivLevel &lvl = refiner.levels[level - 1];

  // The smoothed base + frames this level's disp is relative to (cached; a
  // clean cache makes a stroke-end writeback O(grid points), not O(rebuild)).
  ensureBaseAndFrames(level);
  LevelPos &lp = posCache_[level - 1];
  const Vector<float3> &base = lp.base;
  const Vector<float3> &no = lp.frameNo;
  const Vector<float3> &ta = lp.frameTa;

  // The write target: the edit target's channel when one is set (its weight
  // is pinned to 1 by setEditTarget, so no division), else channel 0. The
  // target's value absorbs the residual after every OTHER composited channel
  // is subtracted from the total frame-space displacement.
  int tch = 0;
  if (toEditTarget && cage_ && cage_->activeEditLayer >= 0) {
    int ch = channelForLayer(cage_->activeEditLayer);
    if (ch > 0 && cage_->sculptLayers[cage_->activeEditLayer].enabled) {
      tch = ch;
    }
  }
  Vector<ChannelMix> mix;
  compositeMix(mix);

  // Grids own disjoint store slots, so the outer loop parallelizes cleanly —
  // but elem() rehydrates an evicted level on first touch, which is not thread
  // safe. Force residency up front.
  store.ensureLevelResident(level);

  int S = lvl.gridSide, w = S + 1;
  const size_t gridsN = grids ? grids->size() : size_t(store.gridCount());
  task::parallel_for(util::IndexRange(gridsN), [&](util::IndexRange range) {
    for (int gi : range) {
      int g = grids ? (*grids)[gi] : gi;
      const int *gv = &lvl.gridVerts[g * w * w];
      for (int v = 0; v < w; v++) {
        for (int u = 0; u < w; u++) {
          int vid = gv[v * w + u];
          if (mask && !(*mask)[vid]) {
            continue;
          }
          float3 n = no[vid], t = ta[vid];
          float3 b = n.cross(t);
          float3 dp = pos[vid] - base[vid];
          float3 rest(0.0f, 0.0f, 0.0f);
          for (const ChannelMix &m : mix) {
            if (m.channel == tch) {
              continue;
            }
            const float *c = store.elem(level, m.channel, g, u, v);
            rest[0] += c[0] * m.weight;
            rest[1] += c[1] * m.weight;
            rest[2] += c[2] * m.weight;
          }
          float *d = store.elem(level, tch, g, u, v);
          d[0] = dp.dot(t) - rest[0];
          d[1] = dp.dot(b) - rest[1];
          d[2] = dp.dot(n) - rest[2];
        }
      }
    }
  });
}

int Multires::writebackChannel() const
{
  if (cage_ && cage_->activeEditLayer >= 0) {
    int ch = channelForLayer(cage_->activeEditLayer);
    if (ch > 0 && cage_->sculptLayers[cage_->activeEditLayer].enabled) {
      return ch;
    }
  }
  return 0;
}

void Multires::gridsWriteback(int level,
                              const Vector<bool> &changed,
                              const Vector<int> &grids)
{
  if (level < 1 || level > maxLevel() || grids.size() == 0) {
    return;
  }
  Assert(posCache_[level - 1].valid, "grids stroke edits a valid chain entry");
  storeDispFromPositions(level,
                         posCache_[level - 1].pos,
                         &changed,
                         /*toEditTarget=*/true,
                         &grids);
  // The store moved past the slot; until the host mirrors (or writeback
  // heals), the slot must not be diffed as an edit source.
  slotStaleMask_ |= 1u << level;
  invalidateAbove(level);
  if (level >= 2 && level < int(downPropPending_.size())) {
    downPropPending_[level] = true;
  }
}

void Multires::syncSlotFromDomain(int level)
{
  MultiresSlot *slot = findSlot(level);
  if (!slot || !slot->mesh || !slot->tree) {
    return;
  }
  GridLevelDomain *d = gridDomain(level);
  mesh::Mesh &m = *slot->mesh;
  const int count = d->vertCount();
  task::parallel_for(util::IndexRange(size_t(count)), [&](util::IndexRange range) {
    for (int v : range) {
      m.v.co[v] = d->pos()[v];
      m.v.no[v] = d->no[v];
    }
  });
  for (auto *node : slot->tree->leaves()) {
    // Geometry-only, same flags as the host mirror (grid_executor.h).
    node->flag |= spatial::Spatial_UpdateGPUGeom | spatial::Spatial_RegenBounds;
    for (spatial::SpatialNode *p = node->parent;
         p && !(p->flag & spatial::Spatial_RegenBounds);
         p = p->parent)
    {
      p->flag |= spatial::Spatial_RegenBounds;
    }
  }
  clearSlotStale(level);
}

int Multires::writeback(int level)
{
  MultiresSlot *slot = findSlot(level);
  if (!slot) {
    return 0;
  }
  if (slotStale(level)) {
    // A grids fold already put this level's edits in the store and the host
    // never mirrored the slot: the slot-vs-baseline diff below would read
    // the PRE-stroke slot as fresh mesh-path edits and fold them over the
    // grids stroke — silent data loss, reachable implicitly through
    // setActiveLevel / addLevel / removeTopLevel / levelPositionsOut. The
    // store is current, so there is nothing to fold; heal the slot instead.
    syncSlotFromDomain(level);
    return 0;
  }
  Assert(posCache_[level - 1].valid, "resident level has a valid baseline");
  Vector<float3> &baseline = posCache_[level - 1].pos;
  mesh::Mesh &lm = *slot->mesh;

  SubdivLevel &lvl = refiner.levels[level - 1];
  Vector<float3> pos;
  Vector<bool> changed;
  pos.resize(lvl.vertCount);
  changed.resize(lvl.vertCount);
  // Vector<bool> is a byte per element (no bitset specialization), so ranges
  // write disjoint bytes.
  std::atomic<int> changedCount(0);
  task::parallel_for(
      util::IndexRange(size_t(lvl.vertCount)), [&](util::IndexRange range) {
        int local = 0;
        for (int i : range) {
          pos[i] = lm.v.co[i];
          changed[i] = std::memcmp(&pos[i], &baseline[i], sizeof(float3)) != 0;
          local += changed[i] ? 1 : 0;
        }
        changedCount.fetch_add(local, std::memory_order_relaxed);
      });
  int nChanged = changedCount.load(std::memory_order_relaxed);
  if (nChanged == 0) {
    return 0;
  }

  storeDispFromPositions(level, pos, &changed, /*toEditTarget=*/true);
  // A mesh-path edit folded into the store: any grids-domain view of this
  // level (or finer) is stale — drop it, per the fold-point contract.
  dropDomains(level - 1);

  // The edited mesh is the new baseline for this level; everything finer is
  // derived from it and must re-evaluate.
  task::parallel_for(util::IndexRange(size_t(lvl.vertCount)),
                     [&](util::IndexRange range) {
                       for (int i : range) {
                         if (changed[i]) {
                           baseline[i] = pos[i];
                         }
                       }
                     });
  invalidateAbove(level);
  // Coarser levels are NOT derived from this one, so they still show the
  // pre-edit surface until a downward switch restricts it into them.
  if (level >= 2 && level < int(downPropPending_.size())) {
    downPropPending_[level] = true;
  }
  return nChanged;
}

void Multires::invalidateAbove(int level)
{
  dropDomains(level);
  for (int l = level + 1; l <= maxLevel(); l++) {
    posCache_[l - 1].reset();
  }
  for (int i = int(slots_.size()) - 1; i >= 0; i--) {
    if (slots_[i].level > level) {
      evictSlot(i);
    }
  }
}

void Multires::invalidateAll()
{
  dropDomains(0);
  for (int l = 1; l <= maxLevel(); l++) {
    posCache_[l - 1].reset();
  }
  for (int i = int(slots_.size()) - 1; i >= 0; i--) {
    evictSlot(i);
  }
  activeLevel_ = 0;
  gridAttrs_.invalidateAll();
}

litestl::binding::types::Struct<Multires> *Multires::defineBindings()
{
  using namespace litestl::binding;
  types::Struct<Multires> *st =
      new types::Struct<Multires>("sculptcore::subdiv::Multires", sizeof(Multires));
  BIND_STRUCT_METHOD(st, maxLevel, MARGS());
  BIND_STRUCT_METHOD(st, activeLevel, MARGS());
  BIND_STRUCT_METHOD(st, addLevel, MARGS());
  BIND_STRUCT_METHOD(st, removeTopLevel, MARGS());
  BIND_STRUCT_METHOD(st, setStoreBudget, MARGS("bytes"));
  BIND_STRUCT_METHOD(st, layerAdd, MARGS());
  BIND_STRUCT_METHOD(st, layerRemove, MARGS("li"));
  BIND_STRUCT_METHOD(st, layerSetWeight, MARGS("li", "weight"));
  BIND_STRUCT_METHOD(st, layerSetEnabled, MARGS("li", "enabled"));
  BIND_STRUCT_METHOD(st, layerSetFrozen, MARGS("li", "frozen"));
  BIND_STRUCT_METHOD(st, setEditTarget, MARGS("li"));
  BIND_STRUCT_METHOD(st, editTarget, MARGS());
  BIND_STRUCT_METHOD(st, layerCount, MARGS());
  BIND_STRUCT_METHOD(st, layerWeight, MARGS("li"));
  BIND_STRUCT_METHOD(st, layerEnabled, MARGS("li"));
  BIND_STRUCT_METHOD(st, layerFrozen, MARGS("li"));
  BIND_STRUCT_METHOD(st, layerTableOut, MARGS("out"));
  BIND_STRUCT_METHOD(st, layerTableRestore, MARGS("table"));
  BIND_STRUCT_METHOD(st, vdmAdjacencyOut, MARGS("out"));
  BIND_STRUCT_METHOD(st, stencilMetaOut, MARGS("level", "out"));
  BIND_STRUCT_METHOD(st, stencilOffsetsOut, MARGS("level", "out"));
  BIND_STRUCT_METHOD(st, stencilIndicesOut, MARGS("level", "out"));
  BIND_STRUCT_METHOD(st, stencilWeightsOut, MARGS("level", "out"));
  BIND_STRUCT_METHOD(st, levelTriIndicesOut, MARGS("level", "out"));
  BIND_STRUCT_METHOD(st, levelVertGridCoordsOut, MARGS("level", "out"));
  BIND_STRUCT_METHOD(st, levelGridVertsOut, MARGS("level", "out"));
  return st;
}

MultiresSlot *Multires::setActiveLevel(int level, bool propagate, bool materializeSlot)
{
  if (activeLevel_ >= 1 && activeLevel_ != level) {
    writeback(activeLevel_);
    // Stepping down: each level we leave behind hands its surface to the one
    // below, so the coarser level the user asked for reflects the fine detail
    // instead of the pre-edit surface. Gated on the pending flag because
    // restriction is not the inverse of subdivision — re-running it on a level
    // that is already up to date (the up-then-down round trip a stroke flush
    // does) would smooth the user's own coarse edits away.
    for (int l = activeLevel_; propagate && l > level && l >= 2; l--) {
      if (downPropPending_[l]) {
        propagateDown(l);
      }
      // Grid channels ride the same step, on their own per-channel debt: paint
      // is authored surface data, so it has to follow a fine edit downward for
      // the same reason positions do.
      if (store.anyChannelLevelDebt(l)) {
        propagateAttrsDown(l);
      }
    }
  }
  // Mark active BEFORE materializing so eviction protects the incoming level
  // (not the one being switched away from) when the budget is tight.
  activeLevel_ = level;
  // The lazy path (grids-native hosts) skips the slot entirely: writeback +
  // debt settling above operate on the chain, and the caller materializes on
  // first mesh-path need (Multires::materialize) — the slot exists only for
  // mesh-path readers.
  MultiresSlot *slot = materializeSlot ? materialize(level) : findSlot(level);
  enforceStoreBudget();
  return slot;
}

void Multires::enforceStoreBudget()
{
  if (storeBudgetBytes == 0) {
    return;
  }
  // Finest-first (largest arrays, biggest win); never the active level.
  for (int l = int(refiner.levels.size());
       l >= 1 && store.residentBytes() > storeBudgetBytes;
       l--)
  {
    if (l != activeLevel_) {
      store.evictLevel(l);
    }
  }
}

} // namespace sculptcore::subdiv
