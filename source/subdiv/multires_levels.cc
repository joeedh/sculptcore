/** Level lifecycle and readers: seeding, VDM capture, add/remove level and the
 * stencil / index tables exposed to hosts. */

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

int Multires::seedLevelPositions(int level, const float (*samples)[3], int sampleNum)
{
  if (level < 1 || level > maxLevel()) {
    return 0;
  }
  SubdivLevel &lvl = refiner.levels[level - 1];
  if (sampleNum != int(lvl.gridVerts.size())) {
    return -1;
  }
  // Chain + base/frames BEFORE the in-place edit — materializing the base
  // lazily after the write would copy the seeded positions as the base (the
  // posIsBase hazard the grid domain closes the same way).
  ensureChain(level);
  ensureBaseAndFrames(level);
  dropDomains(level - 1);
  Vector<float3> &pos = posCache_[level - 1].pos;
  for (int i = 0; i < sampleNum; i++) {
    int vid = lvl.gridVerts[i];
    if (vid >= 0) {
      pos[vid] = float3(samples[i][0], samples[i][1], samples[i][2]);
    }
  }
  storeDispFromPositions(level, pos, nullptr, /*toEditTarget=*/false);
  // The seed lands wholly at this level; the level below now owes (settled
  // by the first downward switch, exactly like a writeback's debt).
  if (level >= 2 && level < int(downPropPending_.size())) {
    downPropPending_[level] = true;
  }
  invalidateAbove(level);
  // A resident slot of this level shows pre-seed positions; drop it.
  for (int i = int(slots_.size()) - 1; i >= 0; i--) {
    if (slots_[i].level == level) {
      evictSlot(i);
    }
  }
  return sampleNum;
}

int Multires::captureDetailToVdm(int level, vdm::VdmStore &vstore)
{
  if (level < 1 || level > int(refiner.levels.size()) ||
      vstore.params.backend != vdm::VdmBackend::PTEX)
  {
    return 0;
  }
  {
    // Capture is defined on channel 0 only: zeroing it and dropping the
    // surface to the smooth base would double-count any contributing layer
    // channel. Refuse until a layer×VDM migration exists (post-V2).
    Vector<ChannelMix> mix;
    compositeMix(mix);
    if (int(mix.size()) > 1) {
      return 0;
    }
  }
  SubdivLevel &lvl = refiner.levels[level - 1];
  int S = lvl.gridSide, w = S + 1;

  // The smoothed base this level's disp is relative to (writeback's twin).
  Vector<float3> cageCo, base;
  const Vector<float3> *prev;
  if (level == 1) {
    gatherVertCo(*cage_, cageCo);
    prev = &cageCo;
  } else {
    ensureChain(level - 1);
    prev = &posCache_[level - 2].pos;
  }
  lvl.stencil.eval(*prev, base);

  int texels = 0;
  for (int g = 0; g < store.gridCount(); g++) {
    int R = vstore.gridRes(g);
    if (R <= 0) {
      continue;
    }
    for (int y = 0; y < R; y++) {
      float pv = (float(y) + 0.5f) / float(R) * float(S);
      int cv = int(pv);
      cv = cv > S - 1 ? S - 1 : cv;
      float fv = pv - float(cv);
      for (int x = 0; x < R; x++) {
        float pu = (float(x) + 0.5f) / float(R) * float(S);
        int cu = int(pu);
        cu = cu > S - 1 ? S - 1 : cu;
        float fu = pu - float(cu);
        const float *d00 = store.elem(level, 0, g, cu, cv);
        const float *d10 = store.elem(level, 0, g, cu + 1, cv);
        const float *d01 = store.elem(level, 0, g, cu, cv + 1);
        const float *d11 = store.elem(level, 0, g, cu + 1, cv + 1);
        float3 D;
        for (int k = 0; k < 3; k++) {
          D[k] = (d00[k] * (1.0f - fu) + d10[k] * fu) * (1.0f - fv) +
                 (d01[k] * (1.0f - fu) + d11[k] * fu) * fv;
        }
        if (D.length() < 1e-12f) {
          continue; // keep tile sparsity: untouched regions allocate nothing
        }
        float3 cur = vstore.texelP(g, x, y);
        vstore.writeTexelP(g, x, y, cur + D);
        texels++;
      }
    }
  }

  // Zero the captured disp and drop the level onto the smooth base.
  for (int g = 0; g < store.gridCount(); g++) {
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        float *d = store.elem(level, 0, g, u, v);
        d[0] = d[1] = d[2] = 0.0f;
      }
    }
  }
  dropDomains(level - 1);
  posCache_[level - 1].pos = base;
  posCache_[level - 1].valid = true;
  MultiresSlot *slot = findSlot(level);
  if (slot && slot->mesh) {
    for (int i = 0; i < int(base.size()); i++) {
      slot->mesh->v.co[i] = base[i];
    }
    slot->mesh->recalc_normals();
    displace::FrameProviderParams fparams;
    displace::updateFramesAll(*slot->mesh, fparams);
  }
  invalidateAbove(level);

  for (int g = 0; g < store.gridCount(); g++) {
    vstore.syncGridSkirts(g);
  }
  vstore.updateBounds();
  return texels;
}

// Stack-depth cap (mirrors the app's MultiresEnableOp levels range). Each level
// roughly quadruples the vertex count, so an upper bound is required.
static constexpr int kMaxMultiresLevels = 7;

int Multires::addLevel()
{
  if (!cage_ || maxLevel() >= kMaxMultiresLevels) {
    return maxLevel();
  }
  if (activeLevel_ >= 1) {
    writeback(activeLevel_); // fold pending edits into the store first
  }
  int n = maxLevel() + 1;
  // refine() rebuilds the grid tables and posCache_.resize may move LevelPos
  // storage — every domain's aliases dangle either way.
  dropDomains(0);
  // refine() rebuilds all levels, but the stencil/grid tables are a pure
  // function of cage topology + level index, so levels 1..n-1 re-emit
  // bit-identically. Keep the existing cached chains + resident slots (they
  // stay valid) so the grow is lossless — only the fresh finest level is
  // derived, as stencil(level n-1) + zero disp.
  refiner.refine(*cage_, n);
  refiner.releaseMeshes();
  store.addLevel(); // zero-disp finest level for every channel (disp + layers)
  noteMaskChange();
  posCache_.resize(n);
  // The fresh level is stencil(n-1) + zero disp, so level n-1 already knows its
  // surface exactly: nothing to push down.
  downPropPending_.resize(n + 1);
  downPropPending_[n] = false;
  activeLevel_ = 0; // already folded above; let setActiveLevel just materialize
  setActiveLevel(n);
  return maxLevel();
}

int Multires::removeTopLevel()
{
  if (!cage_ || maxLevel() <= 1) {
    return maxLevel();
  }
  int prevActive = activeLevel_;
  if (prevActive >= 1) {
    writeback(prevActive);
  }
  int n = maxLevel() - 1;
  // See addLevel: refine() + posCache_.resize invalidate every domain alias.
  dropDomains(0);
  // Evict residents + drop cached chains for the level being removed; the
  // surviving levels' caches stay valid (topology unchanged), so the shrink is
  // lossless too.
  for (int i = int(slots_.size()) - 1; i >= 0; i--) {
    if (slots_[i].level > n) {
      evictSlot(i);
    }
  }
  refiner.refine(*cage_, n);
  refiner.releaseMeshes();
  store.dropTopLevel();
  noteMaskChange();
  posCache_.resize(n);
  // The dropped level's detail is gone with its displacement; so is its debt.
  downPropPending_.resize(n + 1);
  activeLevel_ = 0;
  setActiveLevel(prevActive > n ? n : prevActive);
  return maxLevel();
}

void Multires::vdmAdjacencyOut(Vector<int> &out)
{
  int G = store.gridCount();
  out.resize(G * 8);
  for (int g = 0; g < G; g++) {
    for (int side = 0; side < 4; side++) {
      const GridLink &l = store.link(g, side);
      out[g * 8 + side * 2] = l.grid;
      out[g * 8 + side * 2 + 1] = l.side;
    }
  }
}

void Multires::stencilMetaOut(int level, Vector<int> &out)
{
  out.clear();
  if (level < 1 || level > maxLevel()) {
    return;
  }
  const StencilTable &st = refiner.levels[level - 1].stencil;
  out.resize(3);
  out[0] = st.coarseCount;
  out[1] = st.fineCount;
  out[2] = int(st.weights.size());
}

void Multires::stencilOffsetsOut(int level, Vector<int> &out)
{
  out.clear();
  if (level < 1 || level > maxLevel()) {
    return;
  }
  const StencilTable &st = refiner.levels[level - 1].stencil;
  out.resize(st.offsets.size());
  for (int i = 0; i < int(st.offsets.size()); i++) {
    out[i] = st.offsets[i];
  }
}

void Multires::stencilIndicesOut(int level, Vector<int> &out)
{
  out.clear();
  if (level < 1 || level > maxLevel()) {
    return;
  }
  const StencilTable &st = refiner.levels[level - 1].stencil;
  out.resize(st.indices.size());
  for (int i = 0; i < int(st.indices.size()); i++) {
    out[i] = st.indices[i];
  }
}

void Multires::stencilWeightsOut(int level, Vector<float> &out)
{
  out.clear();
  if (level < 1 || level > maxLevel()) {
    return;
  }
  const StencilTable &st = refiner.levels[level - 1].stencil;
  out.resize(st.weights.size());
  for (int i = 0; i < int(st.weights.size()); i++) {
    out[i] = st.weights[i];
  }
}

void Multires::levelTriIndicesOut(int level, Vector<int> &out)
{
  out.clear();
  if (level < 1 || level > maxLevel()) {
    return;
  }
  SubdivLevel &lvl = refiner.levels[level - 1];
  int S = lvl.gridSide, w = S + 1;
  out.resize(size_t(refiner.gridCount()) * size_t(S) * size_t(S) * 6);
  int n = 0;
  for (int g = 0; g < refiner.gridCount(); g++) {
    const int *gv = &lvl.gridVerts[g * w * w];
    for (int v = 0; v < S; v++) {
      for (int u = 0; u < S; u++) {
        int a = gv[v * w + u], b = gv[v * w + u + 1];
        int c = gv[(v + 1) * w + u + 1], d = gv[(v + 1) * w + u];
        out[n++] = a;
        out[n++] = b;
        out[n++] = c;
        out[n++] = a;
        out[n++] = c;
        out[n++] = d;
      }
    }
  }
}

void Multires::levelGridVertsOut(int level, Vector<int> &out)
{
  out.clear();
  if (level < 1 || level > maxLevel()) {
    return;
  }
  SubdivLevel &lvl = refiner.levels[level - 1];
  out.resize(lvl.gridVerts.size());
  for (int i = 0; i < int(lvl.gridVerts.size()); i++) {
    out[i] = lvl.gridVerts[i];
  }
}

} // namespace sculptcore::subdiv
