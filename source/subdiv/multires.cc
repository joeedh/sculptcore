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

  // Deliberately NOT tagged AttrUse::UV, and named alongside its `.ptex.c.*`
  // siblings: this is an internal parameterization, not the mesh's UV map. The
  // UV map is the cage's, subdivided by assignDerivedAttrs.
  AttrRef &uvRef = m.c.attrs.ensure(AttrType::FLOAT2, util::string(vdm::PTEX_ATLAS_ATTR),
                                    true);
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

bool Multires::gridFaceInts(const char *name, Vector<int> &out)
{
  out.clear();
  if (!cage_ || !cage_->f.attrs.has(mesh::AttrType::INT, name)) {
    return false;
  }
  auto *mat = cage_->f.attrs.find_attribute(mesh::AttrType::INT, name).get_data<int>();

  // The refiner seeds one grid per cage corner, walking cage faces in id order
  // and each face's corners through c.next (subdiv.cc buildLevelTopo).
  out.ensure_capacity(size_t(refiner.gridCount()));
  for (int fi : cage_->f) {
    const int material = mat->safe_get(fi);
    const int c0 = cage_->l.c[cage_->f.l[fi]];
    int cc = c0;
    do {
      out.append(material);
      cc = cage_->c.next[cc];
    } while (cc != c0);
  }
  if (int(out.size()) != refiner.gridCount()) {
    out.clear();
    return false; // enumeration drifted from the refiner's
  }
  return true;
}

bool Multires::gridMaterials(Vector<int> &out)
{
  return gridFaceInts("material_index", out);
}

bool Multires::gridCageFaces(Vector<int> &out)
{
  out.clear();
  if (!cage_) {
    return false;
  }
  // Same walk gridFaceInts describes: cage faces in id order, corners via c.next.
  out.ensure_capacity(size_t(refiner.gridCount()));
  for (int fi : cage_->f) {
    const int c0 = cage_->l.c[cage_->f.l[fi]];
    int cc = c0;
    do {
      out.append(fi);
      cc = cage_->c.next[cc];
    } while (cc != c0);
  }
  if (int(out.size()) != refiner.gridCount()) {
    out.clear();
    return false; // enumeration drifted from the refiner's
  }
  return true;
}

int Multires::scatterFaceIntToCage(int level, const char *name, Vector<int> &r_grids)
{
  r_grids.clear();
  if (!cage_ || level < 1 || level > maxLevel()) {
    return 0;
  }
  const int S = refiner.levels[level - 1].gridSide;
  const int cells = S * S, gridCount = refiner.gridCount();

  // Two possible sources of per-cell values, and the store wins: a
  // grids-native face stroke writes a Face-domain authored channel and never
  // materializes a slot mesh, so a slot-only read would see nothing at all.
  int ch = store.findChannel(util::string(name));
  if (ch >= 0 && (!store.channelAuthored(ch) || store.channelElemSize(ch) != 1 ||
                  store.channelDomain(ch) != GridElemDomain::Face ||
                  !store.channelLevelAllocated(level, ch)))
  {
    ch = -1;
  }
  mesh::Mesh *slotMesh = nullptr;
  if (ch >= 0) {
    store.ensureLevelResident(level);
  }
  else {
    MultiresSlot *slot = findSlot(level);
    if (!slot || !slot->mesh || !slot->mesh->f.attrs.has(AttrType::INT, name)) {
      return 0; // nothing materialized to read back from
    }
    slotMesh = slot->mesh;
    if (slotMesh->f.count != gridCount * cells) {
      return 0; // not this level's grid mesh (holes, or a foreign topology)
    }
  }
  Vector<int> gridFace;
  if (!gridCageFaces(gridFace)) {
    return 0;
  }
  const bool isGroup = strcmp(name, "group") == 0;
  if (!cage_->f.attrs.has(AttrType::INT, name)) {
    if (isGroup) {
      cage_->ensureFaceGroups();
    }
    else {
      cage_->f.attrs.ensure(AttrType::INT, util::string(name), true);
    }
  }
  auto *cdata = cage_->f.attrs.find_attribute(AttrType::INT, name).get_data<int>();
  auto *sdata = slotMesh ? slotMesh->f.attrs.find_attribute(AttrType::INT, name).get_data<int>()
                         : nullptr;
  if (!cdata || (!sdata && ch < 0)) {
    return 0;
  }
  // One grid's cells are contiguous in both layouts, so the source is a base
  // pointer per grid on the store side and an offset per grid on the slot side.
  auto gridCells = [&](int g) -> int * {
    return ch >= 0 ? reinterpret_cast<int *>(store.elem(level, ch, g, 0, 0)) : nullptr;
  };

  // Pass 1: per grid, the first cell disagreeing with its cage face's value.
  Vector<int> propVal, propHas;
  propVal.resize(size_t(gridCount));
  propHas.resize(size_t(gridCount));
  task::parallel_for(util::IndexRange(size_t(gridCount)), [&](util::IndexRange range) {
    for (size_t gi : range) {
      const int g = int(gi);
      const int cur = cdata->safe_get(gridFace[g]);
      const int base = g * cells;
      const int *src = gridCells(g);
      propVal[g] = 0;
      propHas[g] = 0;
      for (int k = 0; k < cells; k++) {
        const int v = src ? src[k] : sdata->safe_get(base + k);
        if (v != cur) {
          propVal[g] = v;
          propHas[g] = 1;
          break;
        }
      }
    }
  });

  // Pass 2: the grids of one cage face are contiguous, so walk them as runs
  // and let the lowest-indexed proposal decide the whole face.
  int changed = 0;
  for (int g = 0; g < gridCount;) {
    const int face = gridFace[g];
    int g1 = g;
    while (g1 < gridCount && gridFace[g1] == face) {
      g1++;
    }
    int val = 0;
    bool has = false;
    for (int k = g; k < g1; k++) {
      if (propHas[k]) {
        val = propVal[k];
        has = true;
        break;
      }
    }
    if (has) {
      cdata->materialize(face);
      (*cdata)[face] = val;
      changed++;
      for (int k = g; k < g1; k++) {
        r_grids.append(k);
        const int base = k * cells;
        // Re-stamp the source too: a cell still holding the pre-stroke value
        // would disagree with the cage face just set and propose reverting it
        // on the next fold. Per-cell detail therefore collapses to per-base-face
        // at push time, which is the rule the mesh path already follows.
        int *dst = gridCells(k);
        for (int c = 0; c < cells; c++) {
          if (dst) {
            dst[c] = val;
          }
          else {
            sdata->materialize(base + c);
            (*sdata)[base + c] = val;
          }
        }
      }
    }
    g = g1;
  }
  if (changed && isGroup) {
    gridAttrs_.refreshFaceSetColors(r_grids.data(), int(r_grids.size()));
    if (ch >= 0) {
      gridAttrs_.refreshFaceSetSampleColors(level, r_grids.data(), int(r_grids.size()));
    }
  }
  return changed;
}

void Multires::assignGridMaterials(mesh::Mesh &m, int level)
{
  Vector<int> gridMat;
  if (!gridMaterials(gridMat)) {
    return;
  }

  AttrRef &ref = m.f.attrs.ensure(AttrType::INT, util::string("material_index"), true);
  auto *data = static_cast<AttrData<int> *>(ref.data);

  // Faces are grid-major, one quad per cell, S*S cells per grid (buildLevelTopo).
  const int S = refiner.levels[level - 1].gridSide;
  int f = 0;
  for (int g = 0; g < int(gridMat.size()); g++) {
    for (int k = 0; k < S * S; k++, f++) {
      data->materialize(f);
      (*data)[f] = gridMat[g];
    }
  }
}

void Multires::assignDerivedAttrs(mesh::Mesh &m, int level)
{
  SubdivLevel &lvl = refiner.levels[level - 1];
  const int S = lvl.gridSide, w = S + 1;

  // A slot is a derived copy of the cage, so it inherits the host's "no face
  // set" id — left at 0, ensureFaceGroups fills the level with zeros the cage
  // disagrees with and scatterFaceIntToCage reads the whole mesh as changed.
  if (cage_) {
    m.default_group_id = cage_->default_group_id;
  }

  if (const float4 *samples = gridAttrs_.colorSamples(level)) {
    AttrRef &ref = m.v.attrs.ensure(AttrType::FLOAT4, util::string("color"), true);
    ref.use = ref.use | AttrUse::COLOR;
    auto *col = static_cast<AttrData<float4> *>(ref.data);
    // Replicated lattice points (grid seams) carry the same value, so which
    // grid writes a shared vert last does not matter.
    for (int g = 0; g < refiner.gridCount(); g++) {
      const int *gv = &lvl.gridVerts[g * w * w];
      for (int i = 0; i < w * w; i++) {
        (*col)[gv[i]] = samples[g * w * w + i];
      }
    }
  }

  Vector<int> groups;
  if (gridFaceInts("group", groups)) {
    AttrRef &ref = m.f.attrs.ensure(AttrType::INT, util::string("group"), true);
    ref.use = ref.use | AttrUse::POLYGROUP;
    auto *data = static_cast<AttrData<int> *>(ref.data);
    int f = 0;
    for (int g = 0; g < int(groups.size()); g++) {
      for (int k = 0; k < S * S; k++, f++) {
        data->materialize(f);
        (*data)[f] = groups[g];
      }
    }
  }

  const float2 *uvSamples = gridAttrs_.uvSamples(level);
  if (!uvSamples) {
    return;
  }
  AttrRef &uvRef = m.c.attrs.ensure(AttrType::FLOAT2, util::string("uv"), true);
  uvRef.use = uvRef.use | AttrUse::UV;
  auto *uv = static_cast<AttrData<float2> *>(uvRef.data);

  // The face-major walk of assignGridUVs, over the same lattice: face f is
  // grid g's cell (u,v), and each corner is matched to its lattice point by
  // vert id rather than by an assumed corner order.
  static const int du[4] = {0, 1, 1, 0};
  static const int dv[4] = {0, 0, 1, 1};
  int f = 0;
  for (int g = 0; g < refiner.gridCount(); g++) {
    const int *gv = &lvl.gridVerts[g * w * w];
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
            (*uv)[c.i] = uvSamples[g * w * w + (v + dv[j]) * w + (u + du[j])];
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
      // Every consumer of the mix weights and subtracts these, so this is the
      // chokepoint for the store's "no float math on a typed channel" rule.
      Assert(store.channelDomain(ch) == GridElemDomain::Vertex &&
                 store.channelInterpolatable(ch),
             "composited grid channel takes float math");
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
static float3 cellNewellNormal(const SubdivLevel &lvl, const int *gv, int u, int v,
                               const Vector<float3> &base)
{
  int S = lvl.gridSide, w = S + 1;
  float3 n(0.0f, 0.0f, 0.0f);
  for (int cv = v - 1; cv <= v; cv++) {
    for (int cu = u - 1; cu <= u; cu++) {
      if (cu < 0 || cv < 0 || cu >= S || cv >= S) {
        continue;
      }
      // buildLevelTopo's quad winding for cell (cu,cv).
      int quad[4] = {gv[cv * w + cu], gv[cv * w + cu + 1], gv[(cv + 1) * w + cu + 1],
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
  const MultiresTuning tuning = multiresAutoTune(
      m->v.count, store.gridCount(), GridsStore::sideForLevel(level));
  tree->leaf_limit = treeLeafLimit > 0 ? treeLeafLimit : tuning.slotLeafLimit;
  tree->depth_limit = treeDepthLimit > 0 ? treeDepthLimit : tuning.slotDepthLimit;
  tree->gpu_tri_target = treeGpuTriTarget > 0 ? treeGpuTriTarget :
                                                tuning.slotGpuTriTarget;
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
  // Built from the chain, which is store-current by construction.
  clearSlotStale(level);
  evictOverBudget();
  return findSlot(level);
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
  storeDispFromPositions(level, posCache_[level - 1].pos, &changed,
                         /*toEditTarget=*/true, &grids);
  // The store moved past the slot; until the host mirrors (or writeback
  // heals), the slot must not be diffed as an edit source.
  slotStaleMask_ |= 1u << level;
  invalidateAbove(level);
  if (level >= 2 && level < int(downPropPending_.size())) {
    downPropPending_[level] = true;
  }
}

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
         p && !(p->flag & spatial::Spatial_RegenBounds); p = p->parent)
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
  task::parallel_for(util::IndexRange(size_t(lvl.vertCount)), [&](util::IndexRange range) {
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
  task::parallel_for(util::IndexRange(size_t(lvl.vertCount)), [&](util::IndexRange range) {
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
  posCache_.resize(n);
  // The dropped level's detail is gone with its displacement; so is its debt.
  downPropPending_.resize(n + 1);
  activeLevel_ = 0;
  setActiveLevel(prevActive > n ? n : prevActive);
  return maxLevel();
}

void Multires::refreshAfterLayerChange()
{
  dropDomains(0);
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
  // Delta, like "disp": a sculpt layer holds a per-level displacement
  // correction, so a new finest level starts at zero and a dropped one takes
  // its correction with it.
  store.addChannel(name,
                   3,
                   GridElemDomain::Vertex,
                   mesh::AttrType::FLOAT,
                   /*persist=*/true,
                   GridLevelRule::Delta);
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
  task::parallel_for(util::IndexRange(size_t(lvl.vertCount)), [&](util::IndexRange range) {
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
