/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "grid_attrs.h"

#include "multires.h"

#include "mesh/attribute.h"
#include "mesh/attribute_bool.h"
#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"

#include "litestl/util/alloc.h"

#include <cmath>

using namespace litestl;
using litestl::math::float2;
using litestl::math::float3;
using litestl::math::float4;
using litestl::util::Vector;

namespace sculptcore::subdiv {

using mesh::AttrData;
using mesh::AttrRef;
using mesh::AttrType;
using mesh::AttrUse;

namespace {

/** Ptex-face corner positions of a quad cage face, by loop order — Blender's
 * `subdiv_foreach_corner_vertices_regular_do` weights, which is what
 * `quad_weights_from_uv` interpolates against. */
constexpr float kQuadPtex[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};

/** Which cage face + corner a grid belongs to. Grids are enumerated one per
 * cage corner, cage faces in id order and corners through `c.next` — the
 * refiner's own seeding order (subdiv.cc), so index == grid id. */
struct GridRef {
  int face = ELEM_NONE;
  int corner = ELEM_NONE; // corner id in the cage
  int size = 0;           // corners on the face
  int index = 0;          // this corner's position in the face's loop order
};

void buildGridRefs(mesh::Mesh &cage, Vector<GridRef> &out)
{
  out.clear();
  for (int fi : cage.f) {
    const int c0 = cage.l.c[cage.f.l[fi]];
    int size = 0, cc = c0;
    do {
      size++;
      cc = cage.c.next[cc];
    } while (cc != c0);
    int k = 0;
    cc = c0;
    do {
      out.append(GridRef{fi, cc, size, k});
      k++;
      cc = cage.c.next[cc];
    } while (cc != c0);
  }
}

int attrComps(AttrType type)
{
  switch (type) {
    case AttrType::FLOAT:
      return 1;
    case AttrType::FLOAT2:
      return 2;
    case AttrType::FLOAT3:
      return 3;
    case AttrType::FLOAT4:
      return 4;
    default:
      return 0;
  }
}

/** Flatten a float-family attribute into `comps` floats per element, dense by
 * element id (free slots zeroed) — everything downstream is component-generic. */
bool gatherAttr(AttrRef &ref, size_t capacity, int comps, Vector<float> &out)
{
  out.resize(capacity * size_t(comps));
  for (size_t i = 0; i < out.size(); i++) {
    out[i] = 0.0f;
  }
  switch (ref.type) {
    case AttrType::FLOAT: {
      auto *d = ref.get_data<float>();
      for (size_t i = 0; i < capacity; i++) {
        out[i] = d->safe_get(int(i));
      }
      return true;
    }
    case AttrType::FLOAT2: {
      auto *d = ref.get_data<float2>();
      for (size_t i = 0; i < capacity; i++) {
        const float2 v = d->safe_get(int(i));
        out[i * 2 + 0] = v[0];
        out[i * 2 + 1] = v[1];
      }
      return true;
    }
    case AttrType::FLOAT3: {
      auto *d = ref.get_data<float3>();
      for (size_t i = 0; i < capacity; i++) {
        const float3 v = d->safe_get(int(i));
        for (int k = 0; k < 3; k++) {
          out[i * 3 + k] = v[k];
        }
      }
      return true;
    }
    case AttrType::FLOAT4: {
      auto *d = ref.get_data<float4>();
      for (size_t i = 0; i < capacity; i++) {
        const float4 v = d->safe_get(int(i));
        for (int k = 0; k < 4; k++) {
          out[i * 4 + k] = v[k];
        }
      }
      return true;
    }
    default:
      return false;
  }
}

/** Signed area of a face in UV space — Blender's UV weld refuses to merge
 * corners whose faces wind opposite ways (BKE_mesh_uv_vert_map_create). */
float uvWinding(mesh::Mesh &cage, int fi, const Vector<float> &cornerUv)
{
  const int c0 = cage.l.c[cage.f.l[fi]];
  float area = 0.0f;
  int cc = c0;
  do {
    const int cn = cage.c.next[cc];
    const float *a = &cornerUv[size_t(cc) * 2];
    const float *b = &cornerUv[size_t(cn) * 2];
    area += a[0] * b[1] - b[0] * a[1];
    cc = cn;
  } while (cc != c0);
  return area;
}

/** Corners of face `f` sitting at vert `v`, appended to `out`. */
void cornersOfFaceVert(mesh::Mesh &m, int f, int v, Vector<int> &out)
{
  const int c0 = m.l.c[m.f.l[f]];
  int cc = c0;
  do {
    if (m.c.v[cc] == v) {
      out.append(cc);
    }
    cc = m.c.next[cc];
  } while (cc != c0);
}

/** Unique faces around `v`, with one representative corner each. Appends
 * {face, corner} pairs. */
void facesAroundVert(mesh::Mesh &m, int v, Vector<int> &faces, Vector<int> &corners)
{
  for (int e : m.e_of_v(v)) {
    const int c0 = m.e.c[e];
    if (c0 == ELEM_NONE) {
      continue;
    }
    int cc = c0;
    do {
      if (m.c.v[cc] == v) {
        const int f = m.l.f[m.c.l[cc]];
        if (!faces.contains(f)) {
          faces.append(f);
          corners.append(cc);
        }
      }
      cc = m.c.radial_next[cc];
    } while (cc != c0);
  }
}

/** Project a refined level mesh onto the Catmull-Clark limit surface in place.
 *
 * The limit mask is exact at a vertex's own parametric point regardless of the
 * level it was produced at, which is what makes a discrete refinement agree
 * with OpenSubdiv's limit evaluation — the rule Blender's UV subdivision uses.
 * Interior valence-n: (n²V + 4ΣE + ΣF)/(n(n+5)) over the edge neighbours and
 * the quads' opposite corners; a smooth crease/boundary curve: (4V + e₀ + e₁)/6;
 * a corner (≥3 creases, or any boundary vert under `linearBoundary`): held. */
void applyLimitMask(mesh::Mesh &m, bool linearBoundary)
{
  using mesh::BoolAttrView;
  m.thawTopo();
  BoolAttrView *sharp = mesh::boundary::findBoolEdgeView(&m, mesh::boundary::EDGE_SHARP);

  auto edgeFaces = [&](int e) {
    const int c0 = m.e.c[e];
    if (c0 == ELEM_NONE) {
      return 0;
    }
    int n = 0, cc = c0;
    do {
      n++;
      cc = m.c.radial_next[cc];
    } while (cc != c0);
    return n;
  };

  Vector<float3> limit;
  limit.resize(m.v.capacity());
  Vector<int> faces, corners;
  for (int vi : m.v) {
    const float3 co = m.v.co[vi];
    int valence = 0, nCrease = 0;
    bool onBoundary = false;
    int creaseOpp[2] = {ELEM_NONE, ELEM_NONE};
    for (int e : m.e_of_v(vi)) {
      valence++;
      const bool boundary = edgeFaces(e) != 2;
      onBoundary |= boundary;
      if (boundary || (sharp && (*sharp)[e])) {
        const int opp = m.e.vs[e][0] == vi ? m.e.vs[e][1] : m.e.vs[e][0];
        if (nCrease < 2) {
          creaseOpp[nCrease] = opp;
        }
        nCrease++;
      }
    }

    if (valence == 0 || nCrease >= 3 || (onBoundary && linearBoundary)) {
      limit[vi] = co;
    }
    else if (nCrease == 2) {
      limit[vi] = (co * 4.0f + m.v.co[creaseOpp[0]] + m.v.co[creaseOpp[1]]) * (1.0f / 6.0f);
    }
    else {
      float3 sumE, sumF;
      for (int e : m.e_of_v(vi)) {
        sumE += m.v.co[m.e.vs[e][0] == vi ? m.e.vs[e][1] : m.e.vs[e][0]];
      }
      faces.clear();
      corners.clear();
      facesAroundVert(m, vi, faces, corners);
      for (int c : corners) {
        // Level meshes are all-quad, so the opposite corner is two steps on.
        sumF += m.v.co[m.c.v[m.c.next[m.c.next[c]]]];
      }
      const float n = float(valence);
      limit[vi] = (co * (n * n) + sumE * 4.0f + sumF) * (1.0f / (n * (n + 5.0f)));
    }
  }
  for (int vi : m.v) {
    m.v.co[vi] = limit[vi];
  }
}

float3 faceSetColor(int group, int defaultGroup)
{
  if (group == 0 || group == defaultGroup) {
    return float3(1.0f, 1.0f, 1.0f);
  }
  // The mesh draw path's palette (spatial_gpu.cc polyGroupColor), so both
  // paths show a face set in the same colour.
  const uint32_t h = uint32_t(group) * 2654435761u;
  const float r = float((h >> 0) & 0xFFu) / 255.0f;
  const float g = float((h >> 8) & 0xFFu) / 255.0f;
  const float b = float((h >> 16) & 0xFFu) / 255.0f;
  return float3(0.25f + 0.7f * r, 0.25f + 0.7f * g, 0.25f + 0.7f * b);
}

} // namespace

litestl::util::string activeUvName(mesh::Mesh &m)
{
  for (AttrRef &attr : m.c.attrs.attrs) {
    if (attr.type == AttrType::FLOAT2 && attr.data && bool(attr.use & AttrUse::UV)) {
      return attr.name;
    }
  }
  return litestl::util::string();
}

void MultiresAttrs::declareHostAttr(const string &name, AttrType type)
{
  for (HostAttr &h : hostAttrs_) {
    if (h.name == name && h.type == type) {
      return;
    }
  }
  hostAttrs_.append(HostAttr{name, type});
}

void MultiresAttrs::clearHostAttrs()
{
  hostAttrs_.clear();
}

GridAttrStorage MultiresAttrs::storageFor(const string &name,
                                          AttrType type,
                                          mesh::AttrFlag flags) const
{
  if (attrComps(type) == 0 && type != AttrType::INT) {
    return GridAttrStorage::None;
  }
  // Scratch the host never stores, so per-grid-element writes cost it nothing.
  if (bool(flags & mesh::AttrFlag::TEMP)) {
    return GridAttrStorage::Temp;
  }
  for (const HostAttr &h : hostAttrs_) {
    if (h.name == name && h.type == type) {
      return GridAttrStorage::Host;
    }
  }
  return GridAttrStorage::Derived;
}

void MultiresAttrs::setUvSmooth(UvSmooth mode)
{
  if (mode == uvSmooth_) {
    return;
  }
  uvSmooth_ = mode;
  for (GridAttrLayer &l : layers_) {
    if (l.uvRule) {
      l.valid = false;
    }
  }
  generation_++;
}

void MultiresAttrs::invalidate(const string &name)
{
  for (GridAttrLayer &l : layers_) {
    if (l.name == name) {
      l.valid = false;
    }
  }
  fsetValid_ = false;
  generation_++;
}

void MultiresAttrs::invalidateAll()
{
  for (GridAttrLayer &l : layers_) {
    l.valid = false;
  }
  fsetValid_ = false;
  generation_++;
}

GridAttrLayer *MultiresAttrs::findLayer(const string &name)
{
  for (GridAttrLayer &l : layers_) {
    if (l.name == name) {
      return &l;
    }
  }
  return nullptr;
}

const float *MultiresAttrs::samples(int level, const string &name, int *r_comps)
{
  if (r_comps) {
    *r_comps = 0;
  }
  mesh::Mesh *cage = mr_ ? mr_->cage() : nullptr;
  if (!cage || name.size() == 0 || level < 1 || level > mr_->maxLevel()) {
    return nullptr;
  }

  GridAttrLayer *layer = findLayer(name);
  if (!layer) {
    // Resolve the cage attribute: corner domain first (UV maps live there),
    // then point. Only float-family layers subdivide.
    GridAttrLayer fresh;
    fresh.name = name;
    for (AttrRef &attr : cage->c.attrs.attrs) {
      if (attr.name == name && attr.data && attrComps(attr.type) > 0) {
        fresh.type = attr.type;
        fresh.corner = true;
        fresh.uvRule = bool(attr.use & AttrUse::UV) && attr.type == AttrType::FLOAT2;
        break;
      }
    }
    if (fresh.type == AttrType::NONE) {
      for (AttrRef &attr : cage->v.attrs.attrs) {
        if (attr.name == name && attr.data && attrComps(attr.type) > 0) {
          fresh.type = attr.type;
          fresh.corner = false;
          break;
        }
      }
    }
    if (fresh.type == AttrType::NONE) {
      return nullptr;
    }
    fresh.comps = attrComps(fresh.type);
    layers_.append(std::move(fresh));
    layer = &layers_.last();
  }

  if (!buildLayer(*layer, level)) {
    return nullptr;
  }
  if (r_comps) {
    *r_comps = layer->comps;
  }
  return layer->data.data();
}

bool MultiresAttrs::buildLayer(GridAttrLayer &layer, int level)
{
  if (layer.valid && layer.level == level) {
    return true;
  }
  mesh::Mesh *cage = mr_->cage();
  if (!cage) {
    return false;
  }
  layer.level = level;
  layer.valid = false;
  // FVAR_LINEAR_ALL is the generic bilinear rule, so UVs take the cheap path.
  const bool fvar = layer.uvRule && uvSmooth_ != UvSmooth::None;
  if (fvar && buildFaceVarying(layer, level, *cage)) {
    layer.valid = true;
    return true;
  }
  buildBilinear(layer, level, *cage);
  layer.valid = true;
  return true;
}

void MultiresAttrs::buildBilinear(GridAttrLayer &layer, int level, mesh::Mesh &cage)
{
  const int comps = layer.comps;
  const int S = mr_->refiner.levels[level - 1].gridSide;
  const int w = S + 1;
  const int gridCount = mr_->refiner.gridCount();
  layer.data.resize(size_t(gridCount) * size_t(w * w) * size_t(comps));

  auto &group = layer.corner ? cage.c.attrs : cage.v.attrs;
  AttrRef ref = group.find_attribute(layer.type, layer.name);
  Vector<float> src;
  const size_t capacity = layer.corner ? cage.c.capacity() : cage.v.capacity();
  if (!ref.exists() || !gatherAttr(ref, capacity, comps, src)) {
    for (size_t i = 0; i < layer.data.size(); i++) {
      layer.data[i] = 0.0f;
    }
    return;
  }

  Vector<GridRef> grids;
  buildGridRefs(cage, grids);
  const int n = std::min(int(grids.size()), gridCount);

  // Ptex-corner values, `comps` floats each (Blender's storage_spans).
  Vector<float> ptex;
  ptex.resize(size_t(comps) * 4);
  Vector<int> faceElems;

  for (int g = 0; g < n; g++) {
    const GridRef &gr = grids[g];
    // Face elements in loop order: the vert or the corner itself, per domain.
    faceElems.clear();
    {
      const int c0 = cage.l.c[cage.f.l[gr.face]];
      int cc = c0;
      do {
        faceElems.append(layer.corner ? cc : cage.c.v[cc]);
        cc = cage.c.next[cc];
      } while (cc != c0);
    }
    const int size = int(faceElems.size());
    auto elemVal = [&](int i, int k) { return src[size_t(faceElems[i]) * comps + k]; };

    // Where in the ptex square this grid's lattice lives: origin + the axes the
    // engine's +u (toward the next corner) and +v (toward the previous) run
    // along. An n-gon corner owns a whole ptex face; a quad's four corners
    // share one, a quadrant each.
    float org[2], du[2], dv[2];
    if (size == 4) {
      const int k = gr.index;
      const float *A = kQuadPtex[k];
      const float *B = kQuadPtex[(k + 1) % 4];
      const float *D = kQuadPtex[(k + 3) % 4];
      for (int i = 0; i < 2; i++) {
        org[i] = A[i];
        du[i] = (B[i] - A[i]) * 0.5f;
        dv[i] = (D[i] - A[i]) * 0.5f;
      }
      for (int j = 0; j < 4; j++) {
        for (int k2 = 0; k2 < comps; k2++) {
          ptex[size_t(j) * comps + k2] = elemVal(j, k2);
        }
      }
    }
    else {
      org[0] = org[1] = 0.0f;
      du[0] = 1.0f;
      du[1] = 0.0f;
      dv[0] = 0.0f;
      dv[1] = 1.0f;
      const int k = gr.index;
      const int nx = (k + 1) % size;
      const int pv = (k + size - 1) % size;
      for (int k2 = 0; k2 < comps; k2++) {
        // {corner, mid-to-next, face average, mid-to-prev} — Blender's
        // vert_/loop_interpolation_from_corner.
        ptex[size_t(0) * comps + k2] = elemVal(k, k2);
        ptex[size_t(1) * comps + k2] = 0.5f * (elemVal(k, k2) + elemVal(nx, k2));
        float avg = 0.0f;
        for (int i = 0; i < size; i++) {
          avg += elemVal(i, k2);
        }
        ptex[size_t(2) * comps + k2] = avg / float(size);
        ptex[size_t(3) * comps + k2] = 0.5f * (elemVal(k, k2) + elemVal(pv, k2));
      }
    }

    float *dst = &layer.data[size_t(g) * size_t(w * w) * size_t(comps)];
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        const float fu = float(u) / float(S), fv = float(v) / float(S);
        const float pu = org[0] + du[0] * fu + dv[0] * fv;
        const float pv2 = org[1] + du[1] * fu + dv[1] * fv;
        // quad_weights_from_uv (subdiv_mesh.cc).
        const float wts[4] = {(1.0f - pu) * (1.0f - pv2),
                              pu * (1.0f - pv2),
                              pu * pv2,
                              (1.0f - pu) * pv2};
        float *o = dst + (size_t(v) * w + u) * comps;
        for (int k = 0; k < comps; k++) {
          float acc = 0.0f;
          for (int j = 0; j < 4; j++) {
            acc += wts[j] * ptex[size_t(j) * comps + k];
          }
          o[k] = acc;
        }
      }
    }
  }
}

bool MultiresAttrs::buildFaceVarying(GridAttrLayer &layer, int level, mesh::Mesh &cage)
{
  AttrRef ref = cage.c.attrs.find_attribute(layer.type, layer.name);
  Vector<float> uv;
  if (!ref.exists() || !gatherAttr(ref, cage.c.capacity(), 2, uv)) {
    return false;
  }

  // --- weld corners into fvar values, per Blender's UV vertex map ---
  // Corners at one vertex merge when their UVs agree within STD_UV_CONNECT_LIMIT
  // and their faces wind the same way in UV space.
  constexpr float kLimit = 0.0001f;
  Vector<bool> winding;
  winding.resize(cage.f.capacity());
  for (int fi : cage.f) {
    winding[fi] = uvWinding(cage, fi, uv) < 0.0f;
  }

  Vector<int> valueOfCorner;
  valueOfCorner.resize(cage.c.capacity());
  for (size_t i = 0; i < valueOfCorner.size(); i++) {
    valueOfCorner[i] = ELEM_NONE;
  }
  Vector<float2> valueUv;
  Vector<int> vertFaces, vertCorners, cluster, clusterOf;
  for (int vi : cage.v) {
    vertFaces.clear();
    vertCorners.clear();
    facesAroundVert(cage, vi, vertFaces, vertCorners);
    cluster.clear();
    for (int f : vertFaces) {
      cornersOfFaceVert(cage, f, vi, cluster);
    }
    clusterOf.clear();
    for (int c : cluster) {
      const float2 cu(uv[size_t(c) * 2], uv[size_t(c) * 2 + 1]);
      const bool wind = winding[cage.l.f[cage.c.l[c]]];
      int found = ELEM_NONE;
      for (size_t k = 0; k < clusterOf.size(); k++) {
        const float2 &ru = valueUv[clusterOf[k]];
        if (std::fabs(ru[0] - cu[0]) < kLimit && std::fabs(ru[1] - cu[1]) < kLimit &&
            winding[cage.l.f[cage.c.l[cluster[k]]]] == wind)
        {
          found = clusterOf[k];
          break;
        }
      }
      if (found == ELEM_NONE) {
        found = int(valueUv.size());
        valueUv.append(cu);
      }
      clusterOf.append(found);
      valueOfCorner[c] = found;
    }
  }

  // --- the UV cage: one vertex per fvar value, the cage's faces verbatim ---
  mesh::Mesh *uvCage = alloc::New<mesh::Mesh>("uv cage");
  for (const float2 &p : valueUv) {
    uvCage->make_vertex(float3(p[0], p[1], 0.0f));
  }
  Vector<int> quad;
  bool ok = true;
  for (int fi : cage.f) {
    quad.clear();
    const int c0 = cage.l.c[cage.f.l[fi]];
    int cc = c0;
    do {
      const int val = valueOfCorner[cc];
      if (val == ELEM_NONE) {
        ok = false;
        break;
      }
      quad.append(val);
      cc = cage.c.next[cc];
    } while (cc != c0);
    if (!ok || uvCage->make_face(std::span<int>(quad.data(), quad.size())) == ELEM_NONE) {
      ok = false;
      break;
    }
  }
  // Creases apply to every channel, so carry the cage's sharp edges across.
  if (ok) {
    if (mesh::BoolAttrView *sharp = mesh::boundary::findBoolEdgeView(
            &cage, mesh::boundary::EDGE_SHARP))
    {
      for (int fi : cage.f) {
        const int c0 = cage.l.c[cage.f.l[fi]];
        int cc = c0;
        do {
          const int cn = cage.c.next[cc];
          if ((*sharp)[cage.c.e[cc]]) {
            const int ue = uvCage->find_edge(valueOfCorner[cc], valueOfCorner[cn]);
            if (ue != ELEM_NONE) {
              mesh::boundary::setEdgeFlag(
                  uvCage, mesh::boundary::EDGE_SHARP, ue, true);
            }
          }
          cc = cn;
        } while (cc != c0);
      }
    }
  }

  if (!ok) {
    alloc::Delete(uvCage);
    return false; // degenerate weld — the caller falls back to bilinear
  }

  RefineOptions opts;
  opts.linearBoundary = uvSmooth_ == UvSmooth::PreserveBoundaries;
  Refiner uvRefiner;
  uvRefiner.refine(*uvCage, level, opts);

  const int gridCount = mr_->refiner.gridCount();
  const int S = mr_->refiner.levels[level - 1].gridSide;
  const int w = S + 1;
  if (uvRefiner.gridCount() != gridCount ||
      uvRefiner.levels[level - 1].gridSide != S)
  {
    alloc::Delete(uvCage);
    return false; // enumeration drifted; bilinear is still a valid answer
  }

  // OpenSubdiv evaluates the fvar LIMIT surface; the limit mask is exact at a
  // vertex's own parametric point, so this closes the discrete/limit gap.
  mesh::Mesh *fine = uvRefiner.levels[level - 1].mesh;
  applyLimitMask(*fine, opts.linearBoundary);

  layer.data.resize(size_t(gridCount) * size_t(w * w) * 2);
  const Vector<int> &gv = uvRefiner.levels[level - 1].gridVerts;
  for (int g = 0; g < gridCount; g++) {
    for (int i = 0; i < w * w; i++) {
      const float3 &p = fine->v.co[gv[size_t(g) * w * w + i]];
      float *o = &layer.data[(size_t(g) * (w * w) + i) * 2];
      o[0] = p[0];
      o[1] = p[1];
    }
  }

  alloc::Delete(uvCage);
  return true;
}

const float2 *MultiresAttrs::uvSamples(int level)
{
  mesh::Mesh *cage = mr_ ? mr_->cage() : nullptr;
  if (!cage) {
    return nullptr;
  }
  const string name = activeUvName(*cage);
  if (name.size() == 0) {
    return nullptr;
  }
  int comps = 0;
  const float *d = samples(level, name, &comps);
  return comps == 2 ? reinterpret_cast<const float2 *>(d) : nullptr;
}

const float4 *MultiresAttrs::colorSamples(int level)
{
  mesh::Mesh *cage = mr_ ? mr_->cage() : nullptr;
  if (!cage || !cage->v.attrs.has(AttrType::FLOAT4, "color")) {
    return nullptr;
  }
  int comps = 0;
  const float *d = samples(level, string("color"), &comps);
  return comps == 4 ? reinterpret_cast<const float4 *>(d) : nullptr;
}

const float3 *MultiresAttrs::gridFaceSetColors()
{
  mesh::Mesh *cage = mr_ ? mr_->cage() : nullptr;
  if (!cage || !cage->f.attrs.has(AttrType::INT, "group")) {
    return nullptr;
  }
  if (fsetValid_ && int(fsetColors_.size()) == mr_->refiner.gridCount()) {
    return fsetColors_.data();
  }
  auto *gdata = cage->f.attrs.find_attribute(AttrType::INT, "group").get_data<int>();
  Vector<GridRef> grids;
  buildGridRefs(*cage, grids);
  if (int(grids.size()) != mr_->refiner.gridCount()) {
    return nullptr;
  }
  fsetColors_.resize(grids.size());
  for (size_t g = 0; g < grids.size(); g++) {
    fsetColors_[g] = faceSetColor(gdata->safe_get(grids[g].face), cage->default_group_id);
  }
  fsetValid_ = true;
  return fsetColors_.data();
}

void MultiresAttrs::refreshFaceSetColors(const int *gridIds, int count)
{
  mesh::Mesh *cage = mr_ ? mr_->cage() : nullptr;
  if (!cage || !fsetValid_ || !cage->f.attrs.has(AttrType::INT, "group")) {
    return; // nothing built yet; the next gridFaceSetColors() builds it fresh
  }
  auto *gdata = cage->f.attrs.find_attribute(AttrType::INT, "group").get_data<int>();
  Vector<GridRef> grids;
  buildGridRefs(*cage, grids);
  if (grids.size() != fsetColors_.size()) {
    // The cage moved under the cache; fall back to a full rebuild.
    fsetValid_ = false;
    generation_++;
    return;
  }
  for (int i = 0; i < count; i++) {
    const int g = gridIds[i];
    if (g >= 0 && g < int(fsetColors_.size())) {
      fsetColors_[g] = faceSetColor(gdata->safe_get(grids[g].face), cage->default_group_id);
    }
  }
}

} // namespace sculptcore::subdiv
