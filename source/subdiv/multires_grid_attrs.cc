/** Grid-domain attributes on a level mesh: UVs, materials, face sets and the
 * other cage attributes derived per grid, plus the scatter of a level edit
 * back onto the cage. */

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

void Multires::assignGridUVs(mesh::Mesh &m, int level)
{
  SubdivLevel &lvl = refiner.levels[level - 1];
  int S = lvl.gridSide, w = S + 1;

  // This attribute is deliberately not tagged AttrUse::UV, and it is named
  // alongside its `.ptex.c.*` siblings, because it is an internal
  // parameterization rather than the mesh's UV map. The UV map belongs to the
  // cage and is subdivided by assignDerivedAttrs.
  AttrRef &uvRef =
      m.c.attrs.ensure(AttrType::FLOAT2, util::string(vdm::PTEX_ATLAS_ATTR), true);
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

  // Two possible sources of per-cell values, and never both: writing grid
  // elements is the Host storage class's privilege (the channel), the mesh
  // path is the Derived one's (the slot column).
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
  } else {
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
    } else {
      cage_->f.attrs.ensure(AttrType::INT, util::string(name), true);
    }
  }
  auto *cdata = cage_->f.attrs.find_attribute(AttrType::INT, name).get_data<int>();
  auto *sdata =
      slotMesh ? slotMesh->f.attrs.find_attribute(AttrType::INT, name).get_data<int>()
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
          } else {
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
  noteCageAttrEdit(level, changed != 0);
  return changed;
}

/** Book-keeping shared by the two cage write-backs: the cage changed, so every
 * derived copy of it is behind -- except this level's, which the caller has
 * just reconciled grid by grid. Only a slot that was current before the write
 * is carried forward; one already behind stays behind and re-derives on its
 * next materialize. */
void Multires::noteCageAttrEdit(int level, bool changed)
{
  if (!changed) {
    return;
  }
  const uint64_t prev = gridAttrs_.cageGeneration();
  gridAttrs_.noteCageEdit();
  if (MultiresSlot *s = findSlot(level)) {
    if (s->derivedGen == prev) {
      s->derivedGen = gridAttrs_.cageGeneration();
    }
  }
}

bool Multires::gridCageVerts(Vector<int> &out)
{
  out.clear();
  if (!cage_) {
    return false;
  }
  // gridCageFaces' walk, taking the corner's vertex instead of its face.
  out.ensure_capacity(size_t(refiner.gridCount()));
  for (int fi : cage_->f) {
    const int c0 = cage_->l.c[cage_->f.l[fi]];
    int cc = c0;
    do {
      out.append(cage_->c.v[cc]);
      cc = cage_->c.next[cc];
    } while (cc != c0);
  }
  if (int(out.size()) != refiner.gridCount()) {
    out.clear();
    return false; // enumeration drifted from the refiner's
  }
  return true;
}

/** Stamp `count` grids of a level slot's per-vertex FLOAT4 attribute from the
 * derived samples of the same name. The partial form of the colour half of
 * #assignDerivedAttrs, for a collapse that has just re-derived those grids. */
void Multires::stampSlotVertFloat4(int level,
                                   const char *name,
                                   const int *gridIds,
                                   int count)
{
  MultiresSlot *slot = findSlot(level);
  if (!slot || !slot->mesh || count <= 0) {
    return;
  }
  AttrRef ref = slot->mesh->v.attrs.find_attribute(AttrType::FLOAT4, name);
  if (!ref.exists()) {
    return;
  }
  int comps = 0;
  const float *samples = gridAttrs_.samples(level, util::string(name), &comps);
  if (!samples || comps != 4) {
    return;
  }
  auto *col = static_cast<mesh::AttrData<math::float4> *>(ref.data);
  const SubdivLevel &lvl = refiner.levels[level - 1];
  const int w = lvl.gridSide + 1, n = refiner.gridCount();
  for (int i = 0; i < count; i++) {
    const int g = gridIds[i];
    if (g < 0 || g >= n) {
      continue;
    }
    const int *gv = &lvl.gridVerts[size_t(g) * w * w];
    const float *src = &samples[size_t(g) * w * w * 4];
    for (int k = 0; k < w * w; k++) {
      col->materialize(gv[k]);
      (*col)[gv[k]] =
          math::float4(src[k * 4], src[k * 4 + 1], src[k * 4 + 2], src[k * 4 + 3]);
    }
  }
}

int Multires::scatterVertFloat4ToCage(
    int level, const char *name, const float *dabs, int dabCount, Vector<int> &r_grids)
{
  if (!cage_ || level < 1 || level > maxLevel()) {
    return 0;
  }
  const int S = refiner.levels[level - 1].gridSide, w = S + 1;
  const int gridCount = refiner.gridCount();

  // Same two sources as the face twin, under the same rule: the channel is
  // written only by a Host-class attribute, the slot column only by a
  // Derived one's mesh path.
  int ch = store.findChannel(util::string(name));
  if (ch >= 0 && (!store.channelAuthored(ch) || store.channelElemSize(ch) != 4 ||
                  store.channelDomain(ch) != GridElemDomain::Vertex ||
                  !store.channelLevelAllocated(level, ch)))
  {
    ch = -1;
  }
  mesh::Mesh *slotMesh = nullptr;
  if (ch >= 0) {
    store.ensureLevelResident(level);
  } else {
    MultiresSlot *slot = findSlot(level);
    if (!slot || !slot->mesh || !slot->mesh->v.attrs.has(AttrType::FLOAT4, name)) {
      return 0; // nothing materialized to read back from
    }
    slotMesh = slot->mesh;
  }
  Vector<int> gridVert;
  if (!gridCageVerts(gridVert)) {
    return 0;
  }
  const bool fresh = !cage_->v.attrs.has(AttrType::FLOAT4, name);
  AttrRef &ref = cage_->v.attrs.ensure(AttrType::FLOAT4, util::string(name), true);
  auto *cdata = static_cast<mesh::AttrData<math::float4> *>(ref.data);
  auto *sdata = slotMesh
                    ? static_cast<mesh::AttrData<math::float4> *>(
                          slotMesh->v.attrs.find_attribute(AttrType::FLOAT4, name).data)
                    : nullptr;
  if (!cdata || (!sdata && ch < 0)) {
    return 0;
  }
  if (fresh) {
    // White, not the attribute system's zero: an unpainted vert has to read as
    // Blender's own default vertex colour, the way ensureFaceGroups floods a
    // first-ever group layer with the host's default id rather than 0.
    for (int vi : cage_->v) {
      cdata->materialize(vi);
      (*cdata)[vi] = math::float4(1.0f, 1.0f, 1.0f, 1.0f);
    }
  }
  const SubdivLevel &lvl = refiner.levels[level - 1];
  Vector<uint8_t> moved;
  moved.resize(cage_->v.capacity());
  for (size_t i = 0; i < moved.size(); i++) {
    moved[i] = 0;
  }
  int changed = 0;
  for (int g = 0; g < gridCount; g++) {
    const int vert = gridVert[g];
    math::float4 val;
    if (ch >= 0) {
      const float *src = store.elem(level, ch, g, 0, 0);
      val = math::float4(src[0], src[1], src[2], src[3]);
    } else {
      val = sdata->safe_get(lvl.gridVerts[size_t(g) * w * w]);
    }
    const math::float4 cur = cdata->safe_get(vert);
    if (val[0] == cur[0] && val[1] == cur[1] && val[2] == cur[2] && val[3] == cur[3]) {
      continue; // includes every later grid of a vert an earlier one adopted
    }
    cdata->materialize(vert);
    (*cdata)[vert] = val;
    moved[vert] = 1;
    changed++;
  }
  // A grid's samples interpolate every corner of its cage face, so what has to
  // re-derive is all grids of every face incident to a vert that moved, not
  // just the grids whose own corner did. Same walk as gridCageVerts.
  if (changed) {
    r_grids.ensure_capacity(r_grids.size() + size_t(gridCount));
    int g0 = 0;
    for (int fi : cage_->f) {
      const int c0 = cage_->l.c[cage_->f.l[fi]];
      int cc = c0, n = 0;
      bool hit = false;
      do {
        hit = hit || moved[cage_->c.v[cc]] != 0;
        cc = cage_->c.next[cc];
        n++;
      } while (cc != c0);
      if (hit) {
        for (int k = 0; k < n; k++) {
          r_grids.append(g0 + k);
        }
      }
      g0 += n;
    }
  }
  // Then the grids the dab itself painted, moved cage vert or not. A dab finer
  // than a base face moves nothing, and its paint has to go now rather than sit
  // on screen until a later dab happens to move a neighbouring cage vert.
  dabGrids(level, dabs, dabCount, r_grids);
  if (r_grids.size() == 0) {
    return 0;
  }
  // The collapse: every non-corner sample of a touched grid becomes a pure
  // function of the cage again, in the derived layer, in the store's session
  // channel and in the level slot -- so no copy still shows grid-resolution
  // paint the cage cannot reproduce.
  gridAttrs_.refreshFromCage(
      level, util::string(name), r_grids.data(), int(r_grids.size()));
  stampSlotVertFloat4(level, name, r_grids.data(), int(r_grids.size()));
  noteCageAttrEdit(level, changed != 0);
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

} // namespace sculptcore::subdiv
