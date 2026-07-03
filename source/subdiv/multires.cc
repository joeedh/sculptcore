#include "multires.h"

#include "displace/frames.h"
#include "mesh/mesh.h"
#include "spatial/spatial.h"
#include "spatial/spatial_base.h"

#include "litestl/util/alloc.h"
#include "litestl/util/assert.h"

#include <cstring>

using namespace litestl;
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

  auto *tree = alloc::New<spatial::SpatialTree>("multires tree", m);
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
  Vector<bool> changed;
  changed.resize(lvl.vertCount);
  int nChanged = 0;
  for (int i = 0; i < lvl.vertCount; i++) {
    float3 p = lm.v.co[i];
    changed[i] = std::memcmp(&p, &baseline[i], sizeof(float3)) != 0;
    nChanged += changed[i] ? 1 : 0;
  }
  if (nChanged == 0) {
    return 0;
  }

  /* Recompute the smoothed base + frames this level's disp is relative to. */
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
        if (!changed[vid]) {
          continue;
        }
        float3 n = (*no)[vid], t = (*ta)[vid];
        float3 b = n.cross(t);
        float3 dp = lm.v.co[vid] - base[vid];
        float *d = store.elem(level, 0, g, u, v);
        d[0] = dp.dot(t);
        d[1] = dp.dot(b);
        d[2] = dp.dot(n);
      }
    }
  }
  alloc::Delete(tm);

  /* The edited mesh is the new baseline for this level; everything finer is
   * derived from it and must re-evaluate. */
  for (int i = 0; i < lvl.vertCount; i++) {
    if (changed[i]) {
      baseline[i] = lm.v.co[i];
    }
  }
  invalidateAbove(level);
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
