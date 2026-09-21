/** Coarse levels following fine edits: stencil least-squares, restriction and
 * the down-propagation entry points. */

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

/** z = Aᵀ·y over the stencil (scatter form of eval), same fma chain per term.
 * `coarseSize` is the dense coarse dimension (see solveStencilLeastSquares). */
static void applyStencilT(const StencilTable &st,
                          const Vector<float3> &y,
                          Vector<float3> &z,
                          int coarseSize)
{
  z.resize(coarseSize);
  for (int j = 0; j < coarseSize; j++) {
    z[j] = float3(0.0f, 0.0f, 0.0f);
  }
  for (int i = 0; i < st.fineCount; i++) {
    for (int k = st.offsets[i]; k < st.offsets[i + 1]; k++) {
      float w = st.weights[k];
      float3 &acc = z[st.indices[k]];
      acc[0] = std::fma(y[i][0], w, acc[0]);
      acc[1] = std::fma(y[i][1], w, acc[1]);
      acc[2] = std::fma(y[i][2], w, acc[2]);
    }
  }
}

static double vecDot(const Vector<float3> &a, const Vector<float3> &b)
{
  double s = 0.0;
  for (int i = 0; i < int(a.size()); i++) {
    s +=
        double(a[i][0]) * b[i][0] + double(a[i][1]) * b[i][1] + double(a[i][2]) * b[i][2];
  }
  return s;
}

/** Jacobi-preconditioned CG on the stencil normal equations AᵀA·x = Aᵀ·target,
 * warm-started from the incoming `x`. Deterministic (fixed sequential order,
 * double accumulators). Returns iterations used.
 *
 * The solution dimension is x.size() — the DENSE coarse-level vert count —
 * not st.coarseCount, which is the coarse id SPACE (v.capacity() of the
 * source level, typically far larger). Refined levels allocate densely, so
 * every stencil index is < x.size(); iterating to coarseCount would read and
 * write x far out of bounds. */
static int solveStencilLeastSquares(const StencilTable &st,
                                    const Vector<float3> &target,
                                    Vector<float3> &x)
{
  const int n = int(x.size());
  Vector<float3> b, fineTmp, q, r, p, z;
  applyStencilT(st, target, b, n);

  // Jacobi preconditioner: diag(AᵀA)_j = Σ_i w_ij².
  Vector<float> dinv;
  dinv.resize(n);
  for (int j = 0; j < n; j++) {
    dinv[j] = 0.0f;
  }
  for (int k = 0; k < int(st.weights.size()); k++) {
    dinv[st.indices[k]] += st.weights[k] * st.weights[k];
  }
  for (int j = 0; j < n; j++) {
    dinv[j] = dinv[j] > 1e-20f ? 1.0f / dinv[j] : 0.0f;
  }

  auto applyM = [&](const Vector<float3> &in, Vector<float3> &out) {
    st.eval(in, fineTmp);
    applyStencilT(st, fineTmp, out, n);
  };

  applyM(x, q);
  r.resize(n);
  z.resize(n);
  p.resize(n);
  for (int j = 0; j < n; j++) {
    r[j] = b[j] - q[j];
    z[j] = r[j] * dinv[j];
    p[j] = z[j];
  }

  double rz = vecDot(r, z);
  double tol2 = vecDot(b, b) * 1e-14 + 1e-30;
  const int maxIter = 200;
  int it = 0;
  for (; it < maxIter && vecDot(r, r) > tol2; it++) {
    applyM(p, q);
    double pq = vecDot(p, q);
    if (!(pq > 0.0)) {
      break;
    }
    float alpha = float(rz / pq);
    for (int j = 0; j < n; j++) {
      x[j] += p[j] * alpha;
      r[j] += q[j] * -alpha;
    }
    for (int j = 0; j < n; j++) {
      z[j] = r[j] * dinv[j];
    }
    double rzNew = vecDot(r, z);
    float beta = float(rzNew / rz);
    rz = rzNew;
    for (int j = 0; j < n; j++) {
      p[j] = z[j] + p[j] * beta;
    }
  }
  return it;
}

/** Full-weighting restriction of a fine position field onto the coarse level:
 * each coarse vert takes the stencil-weight-normalized average of the fine
 * verts it feeds, `R = diag(Aᵀ1)⁻¹Aᵀ`. Rows sum to 1, so R is an averaging
 * operator and cannot overshoot the surface it summarizes — unlike the
 * pseudo-inverse solveStencilLeastSquares computes, which is a deconvolution
 * and rings. Same ascending-row / per-component fma order as applyStencilT. */
static void restrictToCoarse(const StencilTable &st,
                             const Vector<float3> &fine,
                             Vector<float3> &coarse)
{
  const int n = int(coarse.size());
  Vector<float3> acc;
  Vector<float> wsum;
  acc.resize(n);
  wsum.resize(n);
  for (int j = 0; j < n; j++) {
    acc[j] = float3(0.0f, 0.0f, 0.0f);
    wsum[j] = 0.0f;
  }
  for (int i = 0; i < st.fineCount; i++) {
    for (int k = st.offsets[i]; k < st.offsets[i + 1]; k++) {
      const int j = st.indices[k];
      if (j >= n) {
        continue;
      }
      const float w = st.weights[k];
      float3 &a = acc[j];
      a[0] = std::fma(fine[i][0], w, a[0]);
      a[1] = std::fma(fine[i][1], w, a[1]);
      a[2] = std::fma(fine[i][2], w, a[2]);
      wsum[j] += w;
    }
  }
  for (int j = 0; j < n; j++) {
    // A coarse vert with no stencil column feeds nothing finer; leave it.
    if (wsum[j] > 1e-9f) {
      coarse[j] = acc[j] / wsum[j];
    }
  }
}

int Multires::propagateDown(int level)
{
  if (level < 2 || level > maxLevel()) {
    return 0;
  }
  writeback(level); // fold any resident edits; no-op when clean

  // Copies: the coarse store write below invalidates chain references.
  Vector<float3> target = ensureChain(level);
  Vector<float3> coarse = ensureChain(level - 1);

  restrictToCoarse(refiner.levels[level - 1].stencil, target, coarse);
  int nChanged = commitCoarseFit(level, target, coarse);

  // Settled even when nothing moved — the level below already matched, which
  // is exactly no debt. When it did move it inherits the debt one step further.
  downPropPending_[level] = false;
  if (nChanged > 0 && level - 1 >= 2) {
    downPropPending_[level - 1] = true;
  }
  return nChanged;
}

void Multires::noteAttrEdit(int level, int channel)
{
  if (level < 2 || level > maxLevel() || channel < 0 || channel >= store.channelCount()) {
    return;
  }
  if (!store.channelAuthored(channel) || !store.channelPersist(channel)) {
    return;
  }
  store.setChannelLevelDebt(level, channel, true);
}

int Multires::propagateAttrsDown(int level)
{
  if (level < 2 || level > maxLevel()) {
    return 0;
  }
  int n = 0;
  const int maskCh = store.findChannel(util::string(GridLevelDomain::kMaskChannelName));
  for (int c = 0; c < store.channelCount(); c++) {
    if (!store.channelLevelDebt(level, c)) {
      continue;
    }
    // Settled even when the channel turns out to hold nothing at this level:
    // an empty level owes the one below exactly nothing.
    const bool moved = store.restrictChannelDown(c, level);
    store.setChannelLevelDebt(level, c, false);
    if (moved && level - 1 >= 2) {
      store.setChannelLevelDebt(level - 1, c, true);
    }
    if (moved && c == maskCh) {
      // The settle wrote level-1's store content; an alive domain there was
      // built from the pre-settle store and its dense mirror is now stale.
      if (hasGridDomain(level - 1)) {
        gridDomain(level - 1)->syncMaskFromStore();
      }
      noteMaskChange();
    }
    n += moved ? 1 : 0;
  }
  return n;
}

int Multires::downRefit(int level)
{
  if (level < 2 || level > maxLevel()) {
    return 0;
  }
  writeback(level); // fold any resident edits; no-op when clean

  // Copies: the coarse store write below invalidates chain references.
  Vector<float3> target = ensureChain(level);
  Vector<float3> coarse = ensureChain(level - 1);

  SubdivLevel &lvl = refiner.levels[level - 1];
  solveStencilLeastSquares(lvl.stencil, target, coarse);

  int nChanged = commitCoarseFit(level, target, coarse);
  if (nChanged > 0) {
    downPropPending_[level] = false;
    if (level - 1 >= 2) {
      downPropPending_[level - 1] = true;
    }
  }
  return nChanged;
}

int Multires::commitCoarseFit(int level, Vector<float3> &target, Vector<float3> &coarse)
{
  int coarseLevel = level - 1;
  Vector<float3> &cBaseline = posCache_[coarseLevel - 1].pos;
  Vector<bool> changed;
  changed.resize(coarse.size());
  int nChanged = 0;
  for (int i = 0; i < int(coarse.size()); i++) {
    changed[i] = std::memcmp(&coarse[i], &cBaseline[i], sizeof(float3)) != 0;
    nChanged += changed[i] ? 1 : 0;
  }
  if (nChanged == 0) {
    return 0;
  }

  // Both this level's and the coarse level's positions are about to change.
  dropDomains(coarseLevel - 1);
  storeDispFromPositions(coarseLevel, coarse, &changed, /*toEditTarget=*/false);
  posCache_[coarseLevel - 1].pos = coarse;

  // This level's cached base/frames derive from the OLD coarse positions —
  // drop them so the re-expression below recomputes against the refit base.
  {
    LevelPos &lp = posCache_[level - 1];
    lp.framesValid = false;
    lp.posIsBase = false;
    lp.base.clear();
    lp.frameNo.clear();
    lp.frameTa.clear();
  }

  storeDispFromPositions(level, target, nullptr, /*toEditTarget=*/false);
  posCache_[level - 1].pos = std::move(target);

  invalidateAbove(level);

  // The coarse resident (if any) is stale; refresh it, keeping the active
  // level protected. Callers re-fetch slot pointers after this op.
  for (int i = int(slots_.size()) - 1; i >= 0; i--) {
    if (slots_[i].level == coarseLevel) {
      evictSlot(i);
    }
  }
  if (activeLevel_ == coarseLevel) {
    materialize(coarseLevel);
  }
  return nChanged;
}

} // namespace sculptcore::subdiv
