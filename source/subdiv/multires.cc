#include "multires.h"

#include "displace/frames.h"
#include "mesh/mesh.h"
#include "mesh/mesh_proxy.h"
#include "spatial/spatial.h"
#include "spatial/spatial_base.h"

#include "litestl/util/alloc.h"
#include "litestl/util/assert.h"

#include <cmath>
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

  /* One quad per grid cell; each level face is exactly one cell, and the
   * (u,v)->(u+1,v)->... cell order matches the refiner's child-quad winding. */
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
            (*uv)[c.i] = float2(ox + span * (float(u + du[j]) / float(S)),
                                oy + span * (float(v + dv[j]) / float(S)));
          }
        }
      }
    }
  }
}

bool Multires::dispNonZero(int level)
{
  int S = GridsStore::sideForLevel(level), w = S + 1;
  for (int g = 0; g < store.gridCount(); g++) {
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        const float *d = store.elem(level, 0, g, u, v);
        if (d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f) {
          return true;
        }
      }
    }
  }
  return false;
}

/* Apply the level's stored displacement onto the smoothed base, in the F3
 * frame evaluated AT the base (edit-independent). `pos` must NOT alias `base`
 * — seam verts are visited once per replica and must re-read the clean base. */
static void applyDisp(GridsStore &store,
                      Refiner &refiner,
                      int level,
                      mesh::Mesh *baseMesh,
                      const Vector<float3> &base,
                      Vector<float3> &pos)
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
  /* Every vert appears in >= 1 grid slot; replicas recompute the same value. */
  for (int g = 0; g < store.gridCount(); g++) {
    const int *gv = &lvl.gridVerts[g * w * w];
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        int vid = gv[v * w + u];
        const float *d = store.elem(level, 0, g, u, v);
        float3 n = (*no)[vid], t = (*ta)[vid];
        float3 b = n.cross(t);
        float3 p = base[vid];
        p += t * d[0];
        p += b * d[1];
        p += n * d[2];
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
      mesh::Mesh *tm = buildLevelTopo(l);
      for (int i = 0; i < int(base.size()); i++) {
        tm->v.co[i] = base[i];
      }
      tm->recalc_normals();
      applyDisp(store, refiner, l, tm, base, lp.pos);
      alloc::Delete(tm);
    } else {
      lp.pos = std::move(base);
    }
    lp.valid = true;
  }
  return posCache_[level - 1].pos;
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
                                      const Vector<bool> *mask)
{
  SubdivLevel &lvl = refiner.levels[level - 1];

  // Recompute the smoothed base + frames this level's disp is relative to.
  Vector<float3> cageCo, base;
  const Vector<float3> *prev;
  if (level == 1) {
    gatherVertCo(*cage_, cageCo);
    prev = &cageCo;
  } else {
    prev = &posCache_[level - 2].pos;
  }
  lvl.stencil.eval(*prev, base);

  mesh::Mesh *tm = buildLevelTopo(level);
  for (int i = 0; i < int(base.size()); i++) {
    tm->v.co[i] = base[i];
  }
  tm->recalc_normals();
  displace::FrameProviderParams params;
  displace::updateFramesAll(*tm, params);
  auto attr = [&](const char *name) {
    AttrRef ref = tm->v.attrs.find_attribute(AttrType::FLOAT3, name);
    return ref.exists() ? static_cast<AttrData<float3> *>(ref.data) : nullptr;
  };
  AttrData<float3> *no = attr(displace::FRAME_NORMAL_ATTR);
  AttrData<float3> *ta = attr(displace::FRAME_TANGENT_ATTR);
  Assert(no && ta, "frame provider attrs present");

  int S = lvl.gridSide, w = S + 1;
  for (int g = 0; g < store.gridCount(); g++) {
    const int *gv = &lvl.gridVerts[g * w * w];
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        int vid = gv[v * w + u];
        if (mask && !(*mask)[vid]) {
          continue;
        }
        float3 n = (*no)[vid], t = (*ta)[vid];
        float3 b = n.cross(t);
        float3 dp = pos[vid] - base[vid];
        float *d = store.elem(level, 0, g, u, v);
        d[0] = dp.dot(t);
        d[1] = dp.dot(b);
        d[2] = dp.dot(n);
      }
    }
  }
  alloc::Delete(tm);
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

  storeDispFromPositions(level, pos, &changed);

  /* The edited mesh is the new baseline for this level; everything finer is
   * derived from it and must re-evaluate. */
  for (int i = 0; i < lvl.vertCount; i++) {
    if (changed[i]) {
      baseline[i] = pos[i];
    }
  }
  invalidateAbove(level);
  return nChanged;
}

/* z = Aᵀ·y over the stencil (scatter form of eval), same fma chain per term. */
static void applyStencilT(const StencilTable &st,
                          const Vector<float3> &y,
                          Vector<float3> &z)
{
  z.resize(st.coarseCount);
  for (int j = 0; j < st.coarseCount; j++) {
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
 * double accumulators). Returns iterations used. */
static int solveStencilLeastSquares(const StencilTable &st,
                                    const Vector<float3> &target,
                                    Vector<float3> &x)
{
  Vector<float3> b, fineTmp, q, r, p, z;
  applyStencilT(st, target, b);

  // Jacobi preconditioner: diag(AᵀA)_j = Σ_i w_ij².
  Vector<float> dinv;
  dinv.resize(st.coarseCount);
  for (int j = 0; j < st.coarseCount; j++) {
    dinv[j] = 0.0f;
  }
  for (int k = 0; k < int(st.weights.size()); k++) {
    dinv[st.indices[k]] += st.weights[k] * st.weights[k];
  }
  for (int j = 0; j < st.coarseCount; j++) {
    dinv[j] = dinv[j] > 1e-20f ? 1.0f / dinv[j] : 0.0f;
  }

  auto applyM = [&](const Vector<float3> &in, Vector<float3> &out) {
    st.eval(in, fineTmp);
    applyStencilT(st, fineTmp, out);
  };

  applyM(x, q);
  r.resize(st.coarseCount);
  z.resize(st.coarseCount);
  p.resize(st.coarseCount);
  for (int j = 0; j < st.coarseCount; j++) {
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
    for (int j = 0; j < st.coarseCount; j++) {
      x[j] += p[j] * alpha;
      r[j] += q[j] * -alpha;
    }
    for (int j = 0; j < st.coarseCount; j++) {
      z[j] = r[j] * dinv[j];
    }
    double rzNew = vecDot(r, z);
    float beta = float(rzNew / rz);
    rz = rzNew;
    for (int j = 0; j < st.coarseCount; j++) {
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

  storeDispFromPositions(coarseLevel, coarse, &changed);
  posCache_[coarseLevel - 1].pos = coarse;

  // Re-express this level against the new base (reads the coarse chain just
  // stored above); its surface — and any resident slot mesh — is preserved.
  storeDispFromPositions(level, target, nullptr);
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

void Multires::invalidateAbove(int level)
{
  for (int l = level + 1; l <= maxLevel(); l++) {
    posCache_[l - 1].valid = false;
    posCache_[l - 1].pos.clear();
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
    posCache_[l - 1].valid = false;
    posCache_[l - 1].pos.clear();
  }
  for (int i = int(slots_.size()) - 1; i >= 0; i--) {
    evictSlot(i);
  }
  activeLevel_ = 0;
}

litestl::binding::types::Struct<Multires> *Multires::defineBindings()
{
  using namespace litestl::binding;
  types::Struct<Multires> *st =
      new types::Struct<Multires>("sculptcore::subdiv::Multires", sizeof(Multires));
  BIND_STRUCT_METHOD(st, maxLevel, MARGS());
  BIND_STRUCT_METHOD(st, activeLevel, MARGS());
  return st;
}

MultiresSlot *Multires::setActiveLevel(int level)
{
  if (activeLevel_ >= 1 && activeLevel_ != level) {
    writeback(activeLevel_);
  }
  /* Mark active BEFORE materializing so eviction protects the incoming level
   * (not the one being switched away from) when the budget is tight. */
  activeLevel_ = level;
  return materialize(level);
}

} // namespace sculptcore::subdiv
