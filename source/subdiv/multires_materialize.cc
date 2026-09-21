/** Level positions from the grid store: tangent frames, displacement
 * application and slot materialization. */

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

static constexpr float FRAME_EPS = 1e-9f;

static float3 safeNorm(const float3 &v)
{
  float l = v.length();
  return l > FRAME_EPS ? v * (1.0f / l) : float3(0.0f, 0.0f, 0.0f);
}

/** First-owner grid identity per fine vert: 3 ints {grid, latticeU, latticeV},
 * grid -1 for a vert in no grid. Lowest grid index wins, so the choice is a
 * pure function of cage topology — the canonical grid both the parametric
 * frame and the X3 finalize kernel's VDM sampling coordinate derive from. */
static void buildVertGridCoords(const SubdivLevel &lvl, int gridCount, Vector<int> &out)
{
  int S = lvl.gridSide, w = S + 1;
  out.resize(size_t(lvl.vertCount) * 3);
  for (int i = 0; i < lvl.vertCount * 3; i += 3) {
    out[i] = -1;
  }
  for (int g = 0; g < gridCount; g++) {
    const int *gv = &lvl.gridVerts[g * w * w];
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        int vid = gv[v * w + u];
        if (out[vid * 3] < 0) {
          out[vid * 3] = g;
          out[vid * 3 + 1] = u;
          out[vid * 3 + 2] = v;
        }
      }
    }
  }
}

/** Newell normal of the cells incident to lattice point (u,v), summed in
 * ascending (cell v, cell u) order — the deterministic fallback when the
 * lattice differences at the point are degenerate. */
static float3 cellNewellNormal(
    const SubdivLevel &lvl, const int *gv, int u, int v, const Vector<float3> &base)
{
  int S = lvl.gridSide, w = S + 1;
  float3 n(0.0f, 0.0f, 0.0f);
  for (int cv = v - 1; cv <= v; cv++) {
    for (int cu = u - 1; cu <= u; cu++) {
      if (cu < 0 || cv < 0 || cu >= S || cv >= S) {
        continue;
      }
      // buildLevelTopo's quad winding for cell (cu,cv).
      int quad[4] = {gv[cv * w + cu],
                     gv[cv * w + cu + 1],
                     gv[(cv + 1) * w + cu + 1],
                     gv[(cv + 1) * w + cu]};
      for (int k = 0; k < 4; k++) {
        const float3 &p = base[quad[k]], &q = base[quad[(k + 1) & 3]];
        n[0] += (p[1] - q[1]) * (p[2] + q[2]);
        n[1] += (p[2] - q[2]) * (p[0] + q[0]);
        n[2] += (p[0] - q[0]) * (p[1] + q[1]);
      }
    }
  }
  return n;
}

/** Orthonormal tangent frame at lattice point (u,v) of grid `g`, from the
 * level's smooth base and the grid lattice alone: central differences in the
 * interior, one-sided on the two lattice borders. A grid lattice makes no
 * choice among symmetric alternatives, so unlike a cross-field representative
 * there is no ±90° image for a rebuild to land on. Only + - * / sqrt, so the
 * backends agree bitwise without depending on the frame provider. */
static void gridFrame(const SubdivLevel &lvl,
                      int g,
                      int u,
                      int v,
                      const Vector<float3> &base,
                      float3 &nOut,
                      float3 &tOut)
{
  int S = lvl.gridSide, w = S + 1;
  const int *gv = &lvl.gridVerts[size_t(g) * w * w];
  auto at = [&](int uu, int vv) -> const float3 & { return base[gv[vv * w + uu]]; };

  int u0 = u > 0 ? u - 1 : u, u1 = u < S ? u + 1 : u;
  int v0 = v > 0 ? v - 1 : v, v1 = v < S ? v + 1 : v;

  float3 du = at(u1, v) - at(u0, v);
  float3 dv = at(u, v1) - at(u, v0);
  float3 n = du.cross(dv);
  if (n.length() <= FRAME_EPS) {
    // Collapsed cell: the diagonals span the same tangent plane.
    float3 dd = at(u1, v1) - at(u0, v0);
    float3 da = at(u0, v1) - at(u1, v0);
    float3 nd = dd.cross(da);
    if (nd.length() > FRAME_EPS) {
      du = dd;
      n = nd;
    } else {
      n = cellNewellNormal(lvl, gv, u, v, base);
    }
  }

  nOut = safeNorm(n);
  if (nOut.length() <= FRAME_EPS) {
    nOut = float3(0.0f, 0.0f, 1.0f);
  }
  tOut = safeNorm(du - nOut * nOut.dot(du));
  if (tOut.length() <= FRAME_EPS) {
    // Mirrors the provider's degenerate fallback (frames.cc final pass).
    float3 X = std::fabs(nOut[0]) < 0.9f ? float3(1, 0, 0) : float3(0, 1, 0);
    tOut = safeNorm(X - nOut * nOut.dot(X));
  }
}

/** Apply the level's composited displacement (Σ mix weight·channel) onto the
 * smoothed base, in the lattice frame evaluated AT the base (edit-independent).
 * `no`/`ta` are #Multires::parametricFrames output, dense by level vert id —
 * the same field storeDispFromPositions encodes against, which is what makes
 * the round trip exact. `pos` must NOT alias `base` — seam verts are visited
 * once per replica and must re-read the clean base. */
static void applyDisp(GridsStore &store,
                      Refiner &refiner,
                      int level,
                      const Vector<float3> &base,
                      const Vector<float3> &no,
                      const Vector<float3> &ta,
                      Vector<float3> &pos,
                      const Vector<Multires::ChannelMix> &mix)
{
  Assert(&base != &pos, "applyDisp base/pos must not alias");

  SubdivLevel &lvl = refiner.levels[level - 1];
  int S = lvl.gridSide, w = S + 1;

  pos.resize(base.size());
  // Every vert appears in >= 1 grid slot; replicas recompute the same value.
  for (int g = 0; g < store.gridCount(); g++) {
    const int *gv = &lvl.gridVerts[g * w * w];
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        int vid = gv[v * w + u];
        float3 D(0.0f, 0.0f, 0.0f);
        for (const Multires::ChannelMix &m : mix) {
          const float *d = store.elem(level, m.channel, g, u, v);
          D[0] += d[0] * m.weight;
          D[1] += d[1] * m.weight;
          D[2] += d[2] * m.weight;
        }
        float3 n = no[vid], t = ta[vid];
        float3 b = n.cross(t);
        float3 p = base[vid];
        p += t * D[0];
        p += b * D[1];
        p += n * D[2];
        pos[vid] = p;
      }
    }
  }
}

Vector<float3> &Multires::ensureChain(int level)
{
  Assert(level >= 1 && level <= maxLevel(), "level in refined range");

  int start = 1;
  while (start <= level && posCache_[start - 1].valid) {
    start++;
  }

  Vector<float3> cageCo;
  for (int l = start; l <= level; l++) {
    const Vector<float3> *prev;
    if (l == 1) {
      gatherVertCo(*cage_, cageCo);
      prev = &cageCo;
    } else {
      prev = &posCache_[l - 2].pos;
    }

    LevelPos &lp = posCache_[l - 1];
    Vector<float3> base;
    refiner.levels[l - 1].stencil.eval(*prev, base);

    if (dispNonZero(l)) {
      Vector<ChannelMix> mix;
      compositeMix(mix);
      // The decode frames are the cache's frames, so keep them — a later
      // writeback re-expression then pays nothing.
      parametricFrames(l, base, lp.frameNo, lp.frameTa);
      applyDisp(store, refiner, l, base, lp.frameNo, lp.frameTa, lp.pos, mix);
      lp.base = std::move(base);
      lp.framesValid = true;
      lp.posIsBase = false;
    } else {
      lp.pos = std::move(base);
      lp.posIsBase = true;
      lp.framesValid = false;
      lp.base.clear();
      lp.frameNo.clear();
      lp.frameTa.clear();
    }
    lp.valid = true;
  }
  return posCache_[level - 1].pos;
}

void Multires::ensureBaseAndFrames(int level)
{
  Assert(level >= 1 && level <= maxLevel(), "level in refined range");
  LevelPos &lp = posCache_[level - 1];
  Assert(lp.valid, "chain valid through level");

  // Materialize the base copy while pos still equals it (writeback mutates
  // the baseline pos afterwards).
  if (lp.posIsBase && lp.base.size() == 0) {
    lp.base = lp.pos;
    lp.posIsBase = false;
  }
  if (lp.framesValid) {
    return;
  }
  if (lp.base.size() == 0) {
    Vector<float3> cageCo;
    const Vector<float3> *prev;
    if (level == 1) {
      gatherVertCo(*cage_, cageCo);
      prev = &cageCo;
    } else {
      Assert(posCache_[level - 2].valid, "chain valid below level");
      prev = &posCache_[level - 2].pos;
    }
    refiner.levels[level - 1].stencil.eval(*prev, lp.base);
  }
  parametricFrames(level, lp.base, lp.frameNo, lp.frameTa);
  lp.framesValid = true;
}

MultiresSlot *Multires::materialize(int level)
{
  if (MultiresSlot *s = findSlot(level)) {
    s->lastUse = ++useCounter_;
    // A cage write-back reconciles its own level only, so a slot built before
    // one carries pre-edit derived attributes. Re-derive on the way in rather
    // than eagerly on every write-back: this runs on a level switch, and the
    // alternative is a whole-level rebuild per dab for levels nobody is
    // looking at.
    if (s->mesh && s->derivedGen != gridAttrs_.cageGeneration()) {
      assignDerivedAttrs(*s->mesh, level);
      s->derivedGen = gridAttrs_.cageGeneration();
    }
    return s;
  }

  Vector<float3> &pos = ensureChain(level);

  mesh::Mesh *m = buildLevelTopo(level);
  for (int i = 0; i < int(pos.size()); i++) {
    m->v.co[i] = pos[i];
  }
  m->recalc_normals();
  // Zero-disp materialization: pos IS the smooth base, so cache the base +
  // frames now — the level's first writeback then pays nothing.
  LevelPos &lp = posCache_[level - 1];
  if (lp.posIsBase && !lp.framesValid) {
    lp.base = lp.pos;
    parametricFrames(level, lp.base, lp.frameNo, lp.frameTa);
    lp.posIsBase = false;
    lp.framesValid = true;
  }
  assignGridUVs(*m, level);
  assignGridMaterials(*m, level);
  assignDerivedAttrs(*m, level);
  // Level topology is derived state — brushes must never remesh it, and the
  // VDM clamp is a true ceiling here (no promotion; plan X1).
  m->topoLocked = true;

  auto *tree = alloc::New<spatial::SpatialTree>("multires tree", m);
  /* Size-derived defaults (multires_tuning.h); an explicit app value still
   * wins, so an adopted level tree can be made to match app-built ones. */
  const MultiresTuning tuning =
      multiresAutoTune(m->v.count, store.gridCount(), GridsStore::sideForLevel(level));
  tree->leaf_limit = treeLeafLimit > 0 ? treeLeafLimit : tuning.slotLeafLimit;
  tree->depth_limit = treeDepthLimit > 0 ? treeDepthLimit : tuning.slotDepthLimit;
  tree->gpu_tri_target =
      treeGpuTriTarget > 0 ? treeGpuTriTarget : tuning.slotGpuTriTarget;
  tree->buildAll();
  for (auto *node : tree->leaves()) {
    tree->ensure_node_tris(node);
  }

  MultiresSlot s;
  s.level = level;
  s.mesh = m;
  s.tree = tree;
  s.lastUse = ++useCounter_;
  // Just derived from the cage as it stands.
  s.derivedGen = gridAttrs_.cageGeneration();
  slots_.append(s);
  // Built from the chain, which is store-current by construction.
  clearSlotStale(level);
  evictOverBudget();
  return findSlot(level);
}

void Multires::levelVertGridCoordsOut(int level, Vector<int> &out)
{
  out.clear();
  if (level < 1 || level > maxLevel()) {
    return;
  }
  buildVertGridCoords(refiner.levels[level - 1], refiner.gridCount(), out);
}

void Multires::parametricFrames(int level,
                                const Vector<float3> &base,
                                Vector<float3> &no,
                                Vector<float3> &ta)
{
  no.clear();
  ta.clear();
  if (level < 1 || level > maxLevel()) {
    return;
  }
  SubdivLevel &lvl = refiner.levels[level - 1];
  Vector<int> coords;
  buildVertGridCoords(lvl, refiner.gridCount(), coords);

  no.resize(lvl.vertCount);
  ta.resize(lvl.vertCount);
  // Each vert reads its own canonical grid and writes only its own slot, so
  // the field is order-independent — unlike the cross field it replaces.
  task::parallel_for(
      util::IndexRange(size_t(lvl.vertCount)), [&](util::IndexRange range) {
        for (size_t si : range) {
          int i = int(si);
          int g = coords[i * 3];
          if (g < 0) {
            no[i] = float3(0.0f, 0.0f, 1.0f);
            ta[i] = float3(1.0f, 0.0f, 0.0f);
            continue;
          }
          gridFrame(lvl, g, coords[i * 3 + 1], coords[i * 3 + 2], base, no[i], ta[i]);
        }
      });
}

} // namespace sculptcore::subdiv
