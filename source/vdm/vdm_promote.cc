#include "vdm_promote.h"

#include "displace/frames.h"
#include "mesh/attribute.h"
#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "mesh/mesh_callbacks.h"
#include "mesh/mesh_iter.h"
#include "mesh/mesh_proxy.h"
#include "mesh/ops/subdivide.h"
#include "mesh/utils/mesh_validate.h" // faceNewellNormal
#include "meshlog/meshlog.h"
#include "spatial/spatial.h"
#include "vdm_splat.h"
#include "vdm_undo.h"

#include "litestl/util/map.h"
#include "litestl/util/set.h"

#include <cmath>

namespace sculptcore::vdm {

using litestl::math::float2;
using litestl::util::Map;
using litestl::util::Set;
using litestl::util::Vector;
using mesh::AttrData;
using mesh::AttrType;
using mesh::Mesh;
using spatial::DetailCarrier;
using spatial::SpatialTree;

namespace {
constexpr float EPS = 1e-9f;

inline float3 safeNormalize(const float3 &v)
{
  float l = v.length();
  return l > EPS ? v * (1.0f / l) : float3(0.0f, 0.0f, 0.0f);
}

struct FrameAttrs {
  AttrData<float3> *normal = nullptr;
  AttrData<float3> *tangent = nullptr;
};

FrameAttrs frameAttrs(Mesh &m)
{
  FrameAttrs fa;
  mesh::AttrRef nref =
      m.v.attrs.find_attribute(AttrType::FLOAT3, displace::FRAME_NORMAL_ATTR);
  mesh::AttrRef tref =
      m.v.attrs.find_attribute(AttrType::FLOAT3, displace::FRAME_TANGENT_ATTR);
  fa.normal = nref.exists() ? static_cast<AttrData<float3> *>(nref.data) : nullptr;
  fa.tangent = tref.exists() ? static_cast<AttrData<float3> *>(tref.data) : nullptr;
  return fa;
}

/* One fan triangle of a promoted face, snapshotted BEFORE subdivision so the
 * seed pass can reconstruct the exact splat-time frame (barycentric interp of
 * the ORIGINAL verts' frames) at any UV inside the original footprint. */
struct RegionTri {
  float2 uv[3];
  float3 co[3], no[3], tan[3];
  float px[3], py[3]; // texel space
  float inv = 0.0f;   // 1 / signed 2·area (texel space)
};

/* Barycentrics of texel-space point (cx, cy) in T (unnormalized eps-tolerant). */
inline bool triBary(const RegionTri &T, float cx, float cy, float &w0, float &w1,
                    float &w2)
{
  w0 = ((T.px[1] - cx) * (T.py[2] - cy) - (T.py[1] - cy) * (T.px[2] - cx)) * T.inv;
  w1 = ((T.px[2] - cx) * (T.py[0] - cy) - (T.py[2] - cy) * (T.px[0] - cx)) * T.inv;
  w2 = 1.0f - w0 - w1;
  constexpr float kEps = -1e-4f;
  return w0 >= kEps && w1 >= kEps && w2 >= kEps;
}

/* Splat-identical displaced position at `uv` from a snapshot triangle. */
inline float3 displacedAt(const RegionTri &T, VdmStore &store, const float2 &uv,
                          float w0, float w1, float w2)
{
  float3 base = T.co[0] * w0 + T.co[1] * w1 + T.co[2] * w2;
  float3 n = safeNormalize(T.no[0] * w0 + T.no[1] * w1 + T.no[2] * w2);
  float3 t = T.tan[0] * w0 + T.tan[1] * w1 + T.tan[2] * w2;
  t = safeNormalize(t - n * n.dot(t));
  if (n.length() < EPS || t.length() < EPS) {
    return base;
  }
  float3 b = n.cross(t);
  float3 d = store.sample(0, uv[0], uv[1]);
  return base + t * d[0] + b * d[1] + n * d[2];
}

/* Gather a face's fan triangles (UVs + frames) into `out`. Returns false when
 * the face has no UVs / fewer than 3 corners. */
bool gatherFaceTris(Mesh &m,
                    AttrData<float2> *uv,
                    const FrameAttrs &frames,
                    int f,
                    float res,
                    Vector<RegionTri> &out)
{
  Vector<int> cVerts;
  Vector<float2> cUvs;
  mesh::FaceProxy face(&m, f);
  for (auto list : face.lists()) {
    for (auto c : list) {
      cVerts.append(c.v());
      cUvs.append(uv->safe_get(c.i));
    }
  }
  if (cVerts.size() < 3) {
    return false;
  }
  for (int k = 1; k + 1 < int(cVerts.size()); k++) {
    int tri[3] = {0, k, k + 1};
    RegionTri T;
    for (int j = 0; j < 3; j++) {
      int vert = cVerts[tri[j]];
      T.uv[j] = cUvs[tri[j]];
      T.px[j] = T.uv[j][0] * res;
      T.py[j] = T.uv[j][1] * res;
      T.co[j] = m.v.co[vert];
      T.no[j] = frames.normal->safe_get(vert);
      T.tan[j] = frames.tangent->safe_get(vert);
    }
    float area2 =
        (T.px[1] - T.px[0]) * (T.py[2] - T.py[0]) - (T.py[1] - T.py[0]) * (T.px[2] - T.px[0]);
    if (std::fabs(area2) < 1e-12f) {
      continue;
    }
    T.inv = 1.0f / area2;
    out.append(T);
  }
  return true;
}

} // namespace

void collectPromotionCandidates(Mesh &m,
                                SpatialTree &tree,
                                VdmStore &store,
                                std::span<const int> faces,
                                const VdmPromoteParams &params,
                                Vector<int> &out)
{
  AttrData<float2> *uv = findUvCornerLayer(m);
  FrameAttrs frames = frameAttrs(m);
  if (!uv || !frames.normal || !frames.tangent) {
    return;
  }
  store.updateBounds();
  float res = float(store.params.resolution);
  float cosThetaMax = params.theta_max_deg > 0.0f
                          ? std::cos(params.theta_max_deg * 3.14159265f / 180.0f)
                          : -2.0f;

  for (int f : faces) {
    if (f < 0 || size_t(f) >= m.f.capacity() || m.f.freemap[f]) {
      continue;
    }
    if (tree.treeMesh.f.carrier[f] != int(DetailCarrier::VDM)) {
      continue;
    }
    if (params.force) {
      out.append(f);
      continue;
    }

    Vector<RegionTri> tris;
    if (!gatherFaceTris(m, uv, frames, f, res, tris) || tris.size() == 0) {
      continue;
    }

    // Fold bound: the face's stored max|D| vs α_promote·ρ_min of its verts.
    bool candidate = false;
    if (params.alpha_promote > 0.0f) {
      float u0 = tris[0].uv[0][0], v0 = tris[0].uv[0][1], u1 = u0, v1 = v0;
      float rho = 1e30f;
      for (const RegionTri &T : tris) {
        for (int j = 0; j < 3; j++) {
          u0 = std::min(u0, T.uv[j][0]);
          u1 = std::max(u1, T.uv[j][0]);
          v0 = std::min(v0, T.uv[j][1]);
          v1 = std::max(v1, T.uv[j][1]);
        }
      }
      mesh::FaceProxy face(&m, f);
      for (auto list : face.lists()) {
        for (auto c : list) {
          float3 n = safeNormalize(frames.normal->safe_get(c.v()));
          rho = std::min(rho, vertexFoldRadius(m, c.v(), n));
        }
      }
      float bound = store.maxBoundInUvRect(u0, v0, u1, v1);
      if (bound > params.alpha_promote * rho) {
        candidate = true;
      }
    }

    // Overhang: displaced normal at the first triangle's centroid vs the base
    // normal, from ±one-texel central differences of the displaced surface.
    if (!candidate && cosThetaMax > -1.5f) {
      const RegionTri &T = tris[0];
      float2 c = (T.uv[0] + T.uv[1] + T.uv[2]) * (1.0f / 3.0f);
      float h = 1.0f / res;
      auto at = [&](float du, float dv) {
        float2 p(c[0] + du, c[1] + dv);
        float w0, w1, w2;
        float cx = p[0] * res, cy = p[1] * res;
        triBary(T, cx, cy, w0, w1, w2);
        return displacedAt(T, store, p, w0, w1, w2);
      };
      float3 qu = (at(h, 0.0f) - at(-h, 0.0f)) * 0.5f;
      float3 qv = (at(0.0f, h) - at(0.0f, -h)) * 0.5f;
      float3 nq = safeNormalize(qu.cross(qv));
      float3 nb = safeNormalize(mesh::faceNewellNormal(m, f));
      if (nq.length() > EPS && nb.length() > EPS) {
        float d = nq.dot(nb);
        d = d < 0.0f ? -d : d; // orientation-agnostic
        if (d < cosThetaMax) {
          candidate = true;
        }
      }
    }

    if (candidate) {
      out.append(f);
    }
  }
}

VdmPromoteStats promoteRegion(Mesh &m,
                              SpatialTree &tree,
                              VdmStore &store,
                              std::span<const int> faces,
                              const VdmPromoteParams &params,
                              mesh::MeshCallbacks *cb,
                              meshlog::MeshLog *log)
{
  VdmPromoteStats stats;
  AttrData<float2> *uv = findUvCornerLayer(m);
  FrameAttrs frames = frameAttrs(m);
  if (!uv || !frames.normal || !frames.tangent || faces.empty()) {
    return stats;
  }
  float res = float(store.params.resolution);

  // Snapshot the region's fan triangles (frames + UVs of the ORIGINAL verts)
  // before subdivision: the seed pass reconstructs the splat-time frame from
  // these, and the clear pass rasterizes the same footprint.
  Vector<RegionTri> regionTris;
  Vector<int> region;
  for (int f : faces) {
    if (f < 0 || size_t(f) >= m.f.capacity() || m.f.freemap[f]) {
      continue;
    }
    if (tree.treeMesh.f.carrier[f] != int(DetailCarrier::VDM)) {
      continue;
    }
    if (gatherFaceTris(m, uv, frames, f, res, regionTris)) {
      region.append(f);
    }
  }
  if (region.size() == 0) {
    return stats;
  }
  stats.promoted = int(region.size());

  // Select exactly the region faces (plain FACE domain), snapshotting the
  // caller's selection for restore.
  Vector<int> prevSelected;
  for (int f : m.f) {
    if (m.f.select.get_data()->get(f)) {
      prevSelected.append(f);
    }
    m.f.select.set(f, false);
  }
  for (int f : region) {
    m.f.select.set(f, true);
  }

  // Subdivide with a wrapping callback bundle that also records the created
  // faces (the promoted geometry) on top of the caller's meshlog+spatial cb.
  Vector<int> createdFaces;
  mesh::MeshCallbacks wrapped;
  if (cb) {
    wrapped = *cb;
  }
  {
    auto prevFaceCreate = wrapped.onFaceCreate;
    wrapped.onFaceCreate = [&createdFaces, prevFaceCreate](int f) {
      createdFaces.append(f);
      if (prevFaceCreate) {
        prevFaceCreate(f);
      }
    };
  }
  Vector<int> createdVerts;
  {
    auto prevVertCreate = wrapped.onVertCreate;
    wrapped.onVertCreate = [&createdVerts, prevVertCreate](int v) {
      createdVerts.append(v);
      if (prevVertCreate) {
        prevVertCreate(v);
      }
    };
  }
  Vector<int> cutVerts;
  mesh::ops::subdivideEdges(m, &wrapped, params.subdiv_cuts, cutVerts,
                            /*preferOpDomain=*/false);

  // Pattern subdivide restores only the ORIGINAL corners' attrs; corners on
  // new (cut/inner) verts keep default UVs. Recover them from 3D position
  // barycentrics against the snapshot triangles (positions are still base —
  // seeding hasn't run), so classification and seeding see real UVs.
  {
    Set<int> newVerts;
    for (int v : createdVerts) {
      newVerts.add(v);
    }
    auto posBary = [](const RegionTri &T, const float3 &p, float &w1, float &w2) {
      float3 e1 = T.co[1] - T.co[0], e2 = T.co[2] - T.co[0], d = p - T.co[0];
      float a = e1.dot(e1), b = e1.dot(e2), c2 = e2.dot(e2);
      float det = a * c2 - b * b;
      if (std::fabs(det) < 1e-20f) {
        return false;
      }
      float d1 = d.dot(e1), d2 = d.dot(e2);
      w1 = (c2 * d1 - b * d2) / det;
      w2 = (a * d2 - b * d1) / det;
      return true;
    };
    for (int f : createdFaces) {
      if (size_t(f) >= m.f.capacity() || m.f.freemap[f]) {
        continue;
      }
      mesh::FaceProxy face(&m, f);
      for (auto list : face.lists()) {
        for (auto c : list) {
          if (!newVerts.contains(c.v())) {
            continue;
          }
          const float3 &p = m.v.co[c.v()];
          const RegionTri *best = nullptr;
          float bw1 = 0, bw2 = 0, bestErr = 1e30f;
          for (const RegionTri &T : regionTris) {
            float w1, w2;
            if (!posBary(T, p, w1, w2)) {
              continue;
            }
            float w0 = 1.0f - w1 - w2;
            float err = 0.0f;
            err += w0 < 0.0f ? -w0 : 0.0f;
            err += w1 < 0.0f ? -w1 : 0.0f;
            err += w2 < 0.0f ? -w2 : 0.0f;
            if (err < bestErr) {
              bestErr = err;
              best = &T;
              bw1 = w1;
              bw2 = w2;
            }
          }
          if (best && bestErr < 0.1f) {
            float w0 = 1.0f - bw1 - bw2;
            float2 uvp = best->uv[0] * w0 + best->uv[1] * bw1 + best->uv[2] * bw2;
            uv->materialize(c.i);
            (*uv)[c.i] = uvp;
          }
        }
      }
    }
  }

  // Restore the caller's face selection (children inherited select=true).
  for (int f : m.f) {
    m.f.select.set(f, false);
  }
  for (int f : prevSelected) {
    if (size_t(f) < m.f.capacity() && !m.f.freemap[f]) {
      m.f.select.set(f, true);
    }
  }

  // Subdivision also rebuilds the unselected neighbours of every cut edge, so
  // createdFaces holds BOTH the promoted children and neighbour fans (which
  // inherit their VDM carrier and must keep it). Classify by UV footprint:
  // only faces whose UV centroid lies inside the original region flip to GEOM.
  Set<int> seedVerts;
  Vector<int> childFaces;
  for (int f : createdFaces) {
    if (size_t(f) >= m.f.capacity() || m.f.freemap[f]) {
      continue;
    }
    float2 cen(0.0f, 0.0f);
    int nc = 0;
    mesh::FaceProxy face(&m, f);
    for (auto list : face.lists()) {
      for (auto c : list) {
        cen += uv->safe_get(c.i);
        nc++;
      }
    }
    if (nc == 0) {
      continue;
    }
    cen *= 1.0f / float(nc);
    float cx = cen[0] * res, cy = cen[1] * res;
    bool inRegion = false;
    for (const RegionTri &T : regionTris) {
      float w0, w1, w2;
      if (triBary(T, cx, cy, w0, w1, w2)) {
        inRegion = true;
        break;
      }
    }
    if (!inRegion) {
      continue;
    }
    childFaces.append(f);
    tree.treeMesh.f.carrier.get_data()->materialize(f);
    tree.treeMesh.f.carrier[f] = int(DetailCarrier::GEOM);
    for (auto list : face.lists()) {
      for (auto c : list) {
        seedVerts.add(c.v());
      }
    }
  }

  // Seed every region vert to base + frame·D at its UV, using the snapshot
  // triangle containing that UV (exact splat-frame reconstruction). Verts on
  // the region boundary land on the displaced base surface the VDM neighbour
  // still renders — the C0 pin of hybrid-doc §5.
  Vector<int> seeded;
  for (int v : seedVerts) {
    // The vert's UV: average of its corners' UVs over created faces only
    // (a boundary vert's VDM-side corners can sit in another chart).
    float2 vuv(0.0f, 0.0f);
    int nuv = 0;
    if (m.v.e[v] == ELEM_NONE) {
      continue;
    }
    for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
      int c0 = m.e.c[e];
      if (c0 == ELEM_NONE) {
        continue;
      }
      int c = c0;
      do {
        int cf = m.l.f[m.c.l[c]];
        bool inRegion = false;
        for (int rf : childFaces) {
          if (rf == cf) {
            inRegion = true;
            break;
          }
        }
        if (inRegion) {
          int cc = c;
          // Find this face's corner at v.
          int guard = 0;
          while (m.c.v[cc] != v && guard++ < 64) {
            cc = m.c.next[cc];
          }
          if (m.c.v[cc] == v) {
            vuv += uv->safe_get(cc);
            nuv++;
          }
        }
        c = m.c.radial_next[c];
      } while (c != c0);
    }
    if (nuv == 0) {
      continue;
    }
    vuv *= 1.0f / float(nuv);

    float cx = vuv[0] * res, cy = vuv[1] * res;
    const RegionTri *best = nullptr;
    float bw0 = 0, bw1 = 0, bw2 = 0;
    float bestMin = -1e30f;
    for (const RegionTri &T : regionTris) {
      float w0, w1, w2;
      bool inside = triBary(T, cx, cy, w0, w1, w2);
      float mn = std::min(w0, std::min(w1, w2));
      if (inside) {
        best = &T;
        bw0 = w0;
        bw1 = w1;
        bw2 = w2;
        break;
      }
      if (mn > bestMin) {
        bestMin = mn;
        best = &T;
        bw0 = std::max(w0, 0.0f);
        bw1 = std::max(w1, 0.0f);
        bw2 = std::max(w2, 0.0f);
        float s = bw0 + bw1 + bw2;
        if (s > EPS) {
          bw0 /= s;
          bw1 /= s;
          bw2 /= s;
        }
      }
    }
    if (!best) {
      continue;
    }
    float3 q = displacedAt(*best, store, vuv, bw0, bw1, bw2);
    if (cb && cb->onVertChange) {
      cb->onVertChange(v);
    }
    m.v.co[v] = q;
    seeded.append(v);
  }
  stats.seededVerts = int(seeded.size());
  if (seeded.size() > 0) {
    tree.markVertsMoved(seeded);
  }

  // Clear the promoted footprint's texels (the displacement is geometry now;
  // stale texels would double-apply in the fragment path). Interior plus the
  // same 1.5-texel dilation margin the splatter fills.
  constexpr float kSkirt = 1.5f;
  for (const RegionTri &T : regionTris) {
    int pad = int(std::ceil(kSkirt)) + 1;
    int x0 = int(std::floor(std::min(T.px[0], std::min(T.px[1], T.px[2])) - 0.5f)) - pad;
    int x1 = int(std::ceil(std::max(T.px[0], std::max(T.px[1], T.px[2])) + 0.5f)) + pad;
    int y0 = int(std::floor(std::min(T.py[0], std::min(T.py[1], T.py[2])) - 0.5f)) - pad;
    int y1 = int(std::ceil(std::max(T.py[0], std::max(T.py[1], T.py[2])) + 0.5f)) + pad;
    float absArea2 = std::fabs(1.0f / T.inv);
    float lenOpp[3];
    for (int j = 0; j < 3; j++) {
      int a = (j + 1) % 3, b2 = (j + 2) % 3;
      float ex = T.px[b2] - T.px[a], ey = T.py[b2] - T.py[a];
      lenOpp[j] = std::sqrt(ex * ex + ey * ey);
    }
    for (int y = y0; y <= y1; y++) {
      for (int x = x0; x <= x1; x++) {
        float cx = float(x) + 0.5f, cy = float(y) + 0.5f;
        float w0, w1, w2;
        triBary(T, cx, cy, w0, w1, w2);
        float d0 = w0 * absArea2 / std::max(lenOpp[0], EPS);
        float d1 = w1 * absArea2 / std::max(lenOpp[1], EPS);
        float d2 = w2 * absArea2 / std::max(lenOpp[2], EPS);
        if (d0 < -kSkirt || d1 < -kSkirt || d2 < -kSkirt) {
          continue;
        }
        float3 cur = store.texel(x, y);
        if (cur[0] != 0.0f || cur[1] != 0.0f || cur[2] != 0.0f) {
          store.writeTexel(x, y, float3(0.0f, 0.0f, 0.0f));
          stats.clearedTexels++;
        }
      }
    }
  }

  // Mark the carrier boundary: edges whose two radial faces disagree on
  // carrier become EDGE_LAYER_REGION feature edges (dyntopo respects them;
  // UV seams inside VDM regions are already covered by EDGE_UVCHART). The
  // marks ride their own external chunk so one undo press clears them.
  auto *flagChunk = litestl::alloc::New<VdmEdgeFlagLogChunk>("VdmEdgeFlagLogChunk");
  mesh::BoolAttrView *prevFlags =
      mesh::boundary::findBoolEdgeView(&m, mesh::boundary::EDGE_LAYER_REGION);
  Set<int> marked;
  for (int v : seedVerts) {
    if (m.v.e[v] == ELEM_NONE) {
      continue;
    }
    for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
      if (marked.contains(e)) {
        continue;
      }
      int c0 = m.e.c[e];
      if (c0 == ELEM_NONE) {
        continue;
      }
      int carriers[2] = {-1, -1};
      int nf = 0, c = c0;
      do {
        int cf = m.l.f[m.c.l[c]];
        if (nf < 2) {
          carriers[nf] = tree.treeMesh.f.carrier[cf];
        }
        nf++;
        c = m.c.radial_next[c];
      } while (c != c0 && nf < 8);
      if (nf == 2 && carriers[0] != carriers[1]) {
        marked.add(e);
        VdmEdgeFlagLogChunk::Mark mark;
        mark.edge = e;
        mark.before = prevFlags && prevFlags->get(e);
        flagChunk->marks.append(mark);
        mesh::boundary::setEdgeFlag(&m, mesh::boundary::EDGE_LAYER_REGION, e, true);
        stats.regionEdges++;
      }
    }
  }
  if (log && flagChunk->marks.size() > 0) {
    log->appendChunk(flagChunk);
  } else {
    litestl::alloc::Delete(flagChunk);
  }

  return stats;
}

void VdmEdgeFlagLogChunk::undo(mesh::Mesh *m, spatial::SpatialTree * /*tree*/)
{
  for (const Mark &mark : marks) {
    if (size_t(mark.edge) < m->e.capacity() && !m->e.freemap[mark.edge]) {
      mesh::boundary::setEdgeFlag(m, mesh::boundary::EDGE_LAYER_REGION, mark.edge,
                                  mark.before);
    }
  }
}

void VdmEdgeFlagLogChunk::redo(mesh::Mesh *m, spatial::SpatialTree * /*tree*/)
{
  for (const Mark &mark : marks) {
    if (size_t(mark.edge) < m->e.capacity() && !m->e.freemap[mark.edge]) {
      mesh::boundary::setEdgeFlag(m, mesh::boundary::EDGE_LAYER_REGION, mark.edge,
                                  true);
    }
  }
}

} // namespace sculptcore::vdm
