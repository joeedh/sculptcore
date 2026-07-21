#include "multires.h"

#include "vdm/vdm_store.h"

#include "displace/frames.h"
#include "mesh/mesh.h"
#include "mesh/mesh_proxy.h"
#include "spatial/spatial.h"
#include "spatial/spatial_base.h"

#include "litestl/util/alloc.h"
#include "litestl/util/assert.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace litestl;
using litestl::math::float2;
using litestl::math::float3;
using litestl::util::Assert;
using litestl::util::Vector;

namespace sculptcore::subdiv {

Multires::~Multires()
{
  for (int i = int(slots_.size()) - 1; i >= 0; i--) {
    evictSlot(i);
  }
}

void Multires::init(mesh::Mesh &cage, int maxLevel)
{
  for (int i = int(slots_.size()) - 1; i >= 0; i--) {
    evictSlot(i);
  }
  posCache_.clear();
  activeLevel_ = 0;
  cage_ = &cage;

  refiner.refine(cage, maxLevel);
  refiner.releaseMeshes();

  store.buildFromCage(cage);
  for (int i = 0; i < maxLevel; i++) {
    store.addLevel();
  }
  Assert(store.gridCount() == refiner.gridCount(), "store/refiner grid enumeration");

  posCache_.resize(maxLevel);
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
        int quad[4] = {gv[v * w + u], gv[v * w + u + 1], gv[(v + 1) * w + u + 1],
                       gv[(v + 1) * w + u]};
        m->make_face(std::span<int>(quad, 4));
      }
    }
  }
  return m;
}

void Multires::assignGridUVs(mesh::Mesh &m, int level)
{
  SubdivLevel &lvl = refiner.levels[level - 1];
  int S = lvl.gridSide, w = S + 1;

  AttrRef &uvRef = m.c.attrs.ensure(AttrType::FLOAT2, util::string("uv"), true);
  uvRef.use = uvRef.use | AttrUse::UV;
  auto *uv = static_cast<AttrData<float2> *>(uvRef.data);

  // Exact Ptex parameterization alongside the packed chart uv (X2): the
  // owning grid + the grid-local param, free of packing/inset arithmetic.
  AttrRef &pgRef = m.c.attrs.ensure(AttrType::INT, util::string(".ptex.c.grid"), true);
  AttrRef &puRef = m.c.attrs.ensure(AttrType::FLOAT2, util::string(".ptex.c.uv"), true);
  auto *pgrid = static_cast<AttrData<int> *>(pgRef.data);
  auto *puv = static_cast<AttrData<float2> *>(puRef.data);

  int cpr = 1;
  while (cpr * cpr < refiner.gridCount()) {
    cpr++;
  }
  float cell = 1.0f / float(cpr);
  // Gutter so bilinear reads + dilation skirts never bleed across charts.
  float inset = cell / 32.0f;
  float span = cell - 2.0f * inset;

  // Faces are grid-major in creation order, one quad per cell (buildLevelTopo);
  // corners are matched to the cell's lattice points by vert id, so no corner-
  // order assumption. Lattice j: (u,v)+(du,dv) with du/dv below.
  static const int du[4] = {0, 1, 1, 0};
  static const int dv[4] = {0, 0, 1, 1};
  int f = 0;
  for (int g = 0; g < refiner.gridCount(); g++) {
    const int *gv = &lvl.gridVerts[g * w * w];
    float ox = float(g % cpr) * cell + inset;
    float oy = float(g / cpr) * cell + inset;
    for (int v = 0; v < S; v++) {
      for (int u = 0; u < S; u++, f++) {
        int quad[4];
        for (int j = 0; j < 4; j++) {
          quad[j] = gv[(v + dv[j]) * w + (u + du[j])];
        }
        mesh::FaceProxy face(&m, f);
        for (auto list : face.lists()) {
          for (auto c : list) {
            int j = 0;
            while (j < 4 && quad[j] != c.v()) {
              j++;
            }
            Assert(j < 4, "level-face corner matches a cell lattice point");
            float lu = float(u + du[j]) / float(S);
            float lv = float(v + dv[j]) / float(S);
            (*uv)[c.i] = float2(ox + span * lu, oy + span * lv);
            (*pgrid)[c.i] = g;
            (*puv)[c.i] = float2(lu, lv);
          }
        }
      }
    }
  }
}

void Multires::compositeMix(Vector<ChannelMix> &out) const
{
  out.clear();
  out.append({0, 1.0f});
  if (!cage_) {
    return;
  }
  for (int i = 0; i < int(cage_->sculptLayers.size()); i++) {
    const mesh::SculptLayerSettings &st = cage_->sculptLayers[i];
    if (!st.enabled || st.weight == 0.0f) {
      continue;
    }
    int ch = store.findChannel(st.name);
    if (ch > 0) {
      out.append({ch, st.weight});
    }
  }
}

int Multires::channelForLayer(int li) const
{
  if (!cage_ || li < 0 || li >= int(cage_->sculptLayers.size())) {
    return -1;
  }
  int ch = store.findChannel(cage_->sculptLayers[li].name);
  return ch > 0 ? ch : -1;
}

bool Multires::dispNonZero(int level)
{
  Vector<ChannelMix> mix;
  compositeMix(mix);
  int S = GridsStore::sideForLevel(level), w = S + 1;
  for (const ChannelMix &m : mix) {
    for (int g = 0; g < store.gridCount(); g++) {
      for (int v = 0; v < w; v++) {
        for (int u = 0; u < w; u++) {
          const float *d = store.elem(level, m.channel, g, u, v);
          if (d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

/** Copy the F3 frame-provider attrs off `m` (co == smooth base) into dense
 * per-vert vectors — the cache writeback re-expression reads from. */
static void extractFrameAttrs(mesh::Mesh &m,
                              int vertCount,
                              Vector<float3> &no,
                              Vector<float3> &ta)
{
  auto attr = [&](const char *name) {
    AttrRef ref = m.v.attrs.find_attribute(AttrType::FLOAT3, name);
    return ref.exists() ? static_cast<AttrData<float3> *>(ref.data) : nullptr;
  };
  AttrData<float3> *fn = attr(displace::FRAME_NORMAL_ATTR);
  AttrData<float3> *ft = attr(displace::FRAME_TANGENT_ATTR);
  Assert(fn && ft, "frame provider attrs present");
  no.resize(vertCount);
  ta.resize(vertCount);
  for (int i = 0; i < vertCount; i++) {
    no[i] = (*fn)[i];
    ta[i] = (*ft)[i];
  }
}

/** Apply the level's composited displacement (Σ mix weight·channel) onto the
 * smoothed base, in the F3 frame evaluated AT the base (edit-independent).
 * `pos` must NOT alias `base` — seam verts are visited once per replica and
 * must re-read the clean base. */
static void applyDisp(GridsStore &store,
                      Refiner &refiner,
                      int level,
                      mesh::Mesh *baseMesh,
                      const Vector<float3> &base,
                      Vector<float3> &pos,
                      const Vector<Multires::ChannelMix> &mix)
{
  Assert(&base != &pos, "applyDisp base/pos must not alias");
  displace::FrameProviderParams params;
  displace::updateFramesAll(*baseMesh, params);

  auto attr = [&](const char *name) {
    AttrRef ref = baseMesh->v.attrs.find_attribute(AttrType::FLOAT3, name);
    return ref.exists() ? static_cast<AttrData<float3> *>(ref.data) : nullptr;
  };
  AttrData<float3> *no = attr(displace::FRAME_NORMAL_ATTR);
  AttrData<float3> *ta = attr(displace::FRAME_TANGENT_ATTR);
  Assert(no && ta, "frame provider attrs present");

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
        float3 n = (*no)[vid], t = (*ta)[vid];
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
      mesh::Mesh *tm = buildLevelTopo(l);
      for (int i = 0; i < int(base.size()); i++) {
        tm->v.co[i] = base[i];
      }
      tm->recalc_normals();
      // applyDisp runs the frame provider on tm — cache base + frames off it
      // so writeback re-expression skips the whole rebuild.
      applyDisp(store, refiner, l, tm, base, lp.pos, mix);
      extractFrameAttrs(*tm, refiner.levels[l - 1].vertCount, lp.frameNo, lp.frameTa);
      lp.base = std::move(base);
      lp.framesValid = true;
      lp.posIsBase = false;
      alloc::Delete(tm);
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
  mesh::Mesh *tm = buildLevelTopo(level);
  for (int i = 0; i < int(lp.base.size()); i++) {
    tm->v.co[i] = lp.base[i];
  }
  tm->recalc_normals();
  displace::FrameProviderParams params;
  displace::updateFramesAll(*tm, params);
  extractFrameAttrs(*tm, refiner.levels[level - 1].vertCount, lp.frameNo, lp.frameTa);
  alloc::Delete(tm);
  lp.framesValid = true;
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

MultiresSlot *Multires::materialize(int level)
{
  if (MultiresSlot *s = findSlot(level)) {
    s->lastUse = ++useCounter_;
    return s;
  }

  Vector<float3> &pos = ensureChain(level);

  mesh::Mesh *m = buildLevelTopo(level);
  for (int i = 0; i < int(pos.size()); i++) {
    m->v.co[i] = pos[i];
  }
  m->recalc_normals();
  // Zero-disp materialization: this mesh's co IS the smooth base, so cache the
  // base + frames off it now — the level's first writeback then pays nothing.
  LevelPos &lp = posCache_[level - 1];
  if (lp.posIsBase && !lp.framesValid) {
    displace::FrameProviderParams fparams;
    displace::updateFramesAll(*m, fparams);
    extractFrameAttrs(*m, refiner.levels[level - 1].vertCount, lp.frameNo, lp.frameTa);
    lp.base = lp.pos;
    lp.posIsBase = false;
    lp.framesValid = true;
  }
  assignGridUVs(*m, level);
  // Level topology is derived state — brushes must never remesh it, and the
  // VDM clamp is a true ceiling here (no promotion; plan X1).
  m->topoLocked = true;

  auto *tree = alloc::New<spatial::SpatialTree>("multires tree", m);
  if (treeLeafLimit > 0) {
    tree->leaf_limit = treeLeafLimit;
  }
  if (treeDepthLimit > 0) {
    tree->depth_limit = treeDepthLimit;
  }
  if (treeGpuTriTarget > 0) {
    tree->gpu_tri_target = treeGpuTriTarget;
  }
  tree->buildAll();
  for (auto *node : tree->leaves()) {
    tree->ensure_node_tris(node);
  }

  MultiresSlot s;
  s.level = level;
  s.mesh = m;
  s.tree = tree;
  s.lastUse = ++useCounter_;
  slots_.append(s);
  evictOverBudget();
  return findSlot(level);
}

void Multires::storeDispFromPositions(int level,
                                      const Vector<float3> &pos,
                                      const Vector<bool> *mask,
                                      bool toEditTarget)
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

  int S = lvl.gridSide, w = S + 1;
  for (int g = 0; g < store.gridCount(); g++) {
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

int Multires::writeback(int level)
{
  MultiresSlot *slot = findSlot(level);
  if (!slot) {
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
  int nChanged = 0;
  for (int i = 0; i < lvl.vertCount; i++) {
    pos[i] = lm.v.co[i];
    changed[i] = std::memcmp(&pos[i], &baseline[i], sizeof(float3)) != 0;
    nChanged += changed[i] ? 1 : 0;
  }
  if (nChanged == 0) {
    return 0;
  }

  storeDispFromPositions(level, pos, &changed, /*toEditTarget=*/true);

  // The edited mesh is the new baseline for this level; everything finer is
  // derived from it and must re-evaluate.
  for (int i = 0; i < lvl.vertCount; i++) {
    if (changed[i]) {
      baseline[i] = pos[i];
    }
  }
  invalidateAbove(level);
  return nChanged;
}

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
    s += double(a[i][0]) * b[i][0] + double(a[i][1]) * b[i][1] +
         double(a[i][2]) * b[i][2];
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
  // refine() rebuilds all levels, but the stencil/grid tables are a pure
  // function of cage topology + level index, so levels 1..n-1 re-emit
  // bit-identically. Keep the existing cached chains + resident slots (they
  // stay valid) so the grow is lossless — only the fresh finest level is
  // derived, as stencil(level n-1) + zero disp.
  refiner.refine(*cage_, n);
  refiner.releaseMeshes();
  store.addLevel(); // zero-disp finest level for every channel (disp + layers)
  posCache_.resize(n);
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
  posCache_.resize(n);
  activeLevel_ = 0;
  setActiveLevel(prevActive > n ? n : prevActive);
  return maxLevel();
}

void Multires::refreshAfterLayerChange()
{
  for (int l = 1; l <= maxLevel(); l++) {
    posCache_[l - 1].reset();
  }
  for (int i = int(slots_.size()) - 1; i >= 0; i--) {
    evictSlot(i);
  }
  if (activeLevel_ >= 1) {
    materialize(activeLevel_);
  }
}

int Multires::layerAdd()
{
  if (!cage_) {
    return -1;
  }
  // Unique against both settings rows and store channels.
  util::string name;
  for (int n = 0;; n++) {
    char buf[32];
    if (n == 0) {
      std::snprintf(buf, sizeof(buf), "slayer");
    } else {
      std::snprintf(buf, sizeof(buf), "slayer.%03d", n);
    }
    name = util::string(buf);
    if (cage_->findSculptLayer(name) < 0 && store.findChannel(name) < 0) {
      break;
    }
  }
  mesh::SculptLayerSettings st;
  st.name = name;
  cage_->sculptLayers.append(std::move(st));
  store.addChannel(name, 3);
  // A fresh zero channel at weight 1 changes no level positions: no refresh.
  return int(cage_->sculptLayers.size()) - 1;
}

void Multires::layerRemove(int li)
{
  if (!cage_ || li < 0 || li >= int(cage_->sculptLayers.size())) {
    return;
  }
  if (li == cage_->activeEditLayer) {
    setEditTarget(-1); // folds pending edits into the layer first
  } else if (activeLevel_ >= 1) {
    writeback(activeLevel_); // pending edits keep their old attribution
  }
  int ch = store.findChannel(cage_->sculptLayers[li].name);
  if (ch > 0) {
    store.removeChannel(ch);
  }
  cage_->sculptLayers.remove_at(li, /*swap_end_only=*/false);
  if (li < cage_->activeEditLayer) {
    cage_->activeEditLayer--;
  }
  refreshAfterLayerChange();
}

void Multires::layerSetWeight(int li, float weight)
{
  if (!cage_ || li < 0 || li >= int(cage_->sculptLayers.size())) {
    return;
  }
  if (li == cage_->activeEditLayer) {
    // The target's weight is pinned to 1 — re-weighting it ends the edit.
    setEditTarget(-1);
  } else if (activeLevel_ >= 1) {
    writeback(activeLevel_);
  }
  mesh::SculptLayerSettings &st = cage_->sculptLayers[li];
  if (st.weight == weight) {
    return;
  }
  st.weight = weight;
  if (st.enabled) {
    refreshAfterLayerChange();
  }
}

void Multires::layerSetEnabled(int li, int enabled)
{
  if (!cage_ || li < 0 || li >= int(cage_->sculptLayers.size())) {
    return;
  }
  if (!enabled && li == cage_->activeEditLayer) {
    setEditTarget(-1);
  } else if (activeLevel_ >= 1) {
    writeback(activeLevel_);
  }
  mesh::SculptLayerSettings &st = cage_->sculptLayers[li];
  if (st.enabled == (enabled != 0)) {
    return;
  }
  st.enabled = enabled != 0;
  refreshAfterLayerChange();
}

void Multires::layerSetFrozen(int li, int frozen)
{
  if (!cage_ || li < 0 || li >= int(cage_->sculptLayers.size())) {
    return;
  }
  if (frozen && li == cage_->activeEditLayer) {
    // A frozen layer cannot be the edit target.
    setEditTarget(-1);
  }
  cage_->sculptLayers[li].frozen = frozen != 0;
}

int Multires::setEditTarget(int li)
{
  if (!cage_) {
    return -1;
  }
  if (li == cage_->activeEditLayer) {
    return li;
  }
  // Pending level edits belong to the OLD target: fold them first.
  if (activeLevel_ >= 1) {
    writeback(activeLevel_);
  }
  cage_->activeEditLayer = -1;
  if (li < 0 || li >= int(cage_->sculptLayers.size())) {
    return -1;
  }
  mesh::SculptLayerSettings &st = cage_->sculptLayers[li];
  if (st.frozen || channelForLayer(li) < 0) {
    return -1;
  }
  bool changed = !st.enabled || st.weight != 1.0f;
  st.enabled = true;
  st.weight = 1.0f; // pin: writeback must never divide by the target weight
  cage_->activeEditLayer = li;
  if (changed) {
    refreshAfterLayerChange();
  }
  return li;
}

int Multires::editTarget() const
{
  return cage_ ? cage_->activeEditLayer : -1;
}

int Multires::layerCount() const
{
  return cage_ ? int(cage_->sculptLayers.size()) : 0;
}

float Multires::layerWeight(int li) const
{
  return cage_ ? cage_->sculptLayerWeight(li) : 0.0f;
}

int Multires::layerEnabled(int li) const
{
  return cage_ ? cage_->sculptLayerEnabled(li) : 0;
}

int Multires::layerFrozen(int li) const
{
  return cage_ ? cage_->sculptLayerFrozen(li) : 0;
}

void Multires::layerTableOut(Vector<float> &out)
{
  out.clear();
  if (!cage_) {
    return;
  }
  for (const mesh::SculptLayerSettings &st : cage_->sculptLayers) {
    out.append(st.weight);
    out.append(st.enabled ? 1.0f : 0.0f);
    out.append(st.frozen ? 1.0f : 0.0f);
  }
}

void Multires::layerTableRestore(Vector<float> &table)
{
  if (!cage_) {
    return;
  }
  cage_->activeEditLayer = -1;
  cage_->sculptLayers.clear();
  for (int ch = 1; ch < store.channelCount(); ch++) {
    mesh::SculptLayerSettings st;
    st.name = store.channelName(ch);
    int k = (ch - 1) * 3;
    if (k + 2 < int(table.size())) {
      st.weight = table[k];
      st.enabled = table[k + 1] != 0.0f;
      st.frozen = table[k + 2] != 0.0f;
    }
    cage_->sculptLayers.append(std::move(st));
  }
  refreshAfterLayerChange();
}

void Multires::invalidateAbove(int level)
{
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
  for (int l = 1; l <= maxLevel(); l++) {
    posCache_[l - 1].reset();
  }
  for (int i = int(slots_.size()) - 1; i >= 0; i--) {
    evictSlot(i);
  }
  activeLevel_ = 0;
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

void Multires::levelVertGridCoordsOut(int level, Vector<int> &out)
{
  out.clear();
  if (level < 1 || level > maxLevel()) {
    return;
  }
  SubdivLevel &lvl = refiner.levels[level - 1];
  int S = lvl.gridSide, w = S + 1;
  out.resize(size_t(lvl.vertCount) * 3);
  for (int i = 0; i < lvl.vertCount * 3; i += 3) {
    out[i] = -1;
  }
  for (int g = 0; g < refiner.gridCount(); g++) {
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

MultiresSlot *Multires::setActiveLevel(int level)
{
  if (activeLevel_ >= 1 && activeLevel_ != level) {
    writeback(activeLevel_);
  }
  // Mark active BEFORE materializing so eviction protects the incoming level
  // (not the one being switched away from) when the budget is tight.
  activeLevel_ = level;
  MultiresSlot *slot = materialize(level);
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
