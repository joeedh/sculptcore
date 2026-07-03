#include "vdm_splat.h"

#include "displace/frames.h"
#include "mesh/attribute.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/mesh_proxy.h"
#include "mesh/utils/mesh_validate.h" // faceNewellNormal
#include "spatial/spatial.h"

#include "litestl/util/map.h"
#include "litestl/util/set.h"

#include <cmath>

namespace sculptcore::vdm {

using litestl::math::float2;
using litestl::util::Map;
using litestl::util::Set;
using litestl::util::Vector;
using mesh::AttrData;
using mesh::AttrRef;
using mesh::AttrType;
using mesh::Mesh;
using spatial::DetailCarrier;
using spatial::SpatialNode;
using spatial::SpatialTree;

namespace {
constexpr float EPS = 1e-9f;

inline float3 safeNormalize(const float3 &v)
{
  float l = v.length();
  return l > EPS ? v * (1.0f / l) : float3(0.0f, 0.0f, 0.0f);
}

/* Smoothstep falloff over the dab sphere: 1 at the center, 0 at the radius. */
inline float falloff(float dist, float radius)
{
  if (dist >= radius) {
    return 0.0f;
  }
  float w = 1.0f - dist / radius;
  return w * w * (3.0f - 2.0f * w);
}

inline int otherVert(Mesh &m, int e, int v)
{
  return m.e.vs[e][0] == v ? m.e.vs[e][1] : m.e.vs[e][0];
}

inline int edgeFaces(Mesh &m, int e, int &f1, int &f2)
{
  int c0 = m.e.c[e];
  if (c0 == ELEM_NONE) {
    return 0;
  }
  int n = 0, c = c0;
  do {
    int f = m.l.f[m.c.l[c]];
    if (n == 0) {
      f1 = f;
    } else if (n == 1) {
      f2 = f;
    }
    n++;
    c = m.c.radial_next[c];
  } while (c != c0 && n < 64);
  return n;
}

/* Offset-fold radius ρ_min = 1/|κ_max| at `v` from the 1-ring shape operator
 * (the Cohen-Steiner tensor frames.cc seeds from, here reduced to eigenvalue
 * magnitudes and normalized by the barycentric ring area). Flat regions
 * return a large finite radius (clamp effectively off). */
float vertexFoldRadius(Mesh &m, int v, const float3 &n)
{
  constexpr float kFlatRadius = 1e6f;
  if (m.v.e[v] == ELEM_NONE) {
    return kFlatRadius;
  }
  float3 X = std::fabs(n[0]) < 0.9f ? float3(1, 0, 0) : float3(0, 1, 0);
  X = safeNormalize(X - n * n.dot(X));
  if (X.length() < EPS) {
    return kFlatRadius;
  }
  float3 Y = n.cross(X);

  double A00 = 0, A01 = 0, A11 = 0, ringArea = 0;
  for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
    int f1 = ELEM_NONE, f2 = ELEM_NONE;
    if (edgeFaces(m, e, f1, f2) != 2) {
      continue;
    }
    float3 n1 = safeNormalize(mesh::faceNewellNormal(m, f1));
    float3 n2 = safeNormalize(mesh::faceNewellNormal(m, f2));
    float d = n1.dot(n2);
    d = d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d);
    float theta = std::acos(d);
    float3 evec = m.v.co[otherVert(m, e, v)] - m.v.co[v];
    float el = evec.length();
    if (el < EPS) {
      continue;
    }
    float3 eu = evec * (1.0f / el);
    float ex = eu.dot(X), ey = eu.dot(Y);
    double w = double(theta) * double(el);
    A00 += w * ex * ex;
    A01 += w * ex * ey;
    A11 += w * ey * ey;
    ringArea += 0.5 * double(mesh::faceNewellNormal(m, f1).length()) / 3.0;
  }
  if (ringArea < 1e-12) {
    return kFlatRadius;
  }
  double tr = A00 + A11;
  double disc = std::sqrt((A00 - A11) * (A00 - A11) + 4.0 * A01 * A01);
  double l1 = 0.5 * (tr + disc), l2 = 0.5 * (tr - disc);
  double kmax = std::max(std::fabs(l1), std::fabs(l2)) / ringArea;
  if (kmax < 1e-6) {
    return kFlatRadius;
  }
  float r = float(1.0 / kmax);
  return r > kFlatRadius ? kFlatRadius : r;
}

struct FrameAttrs {
  AttrData<float3> *normal = nullptr;
  AttrData<float3> *tangent = nullptr;
};

FrameAttrs frameAttrs(Mesh &m)
{
  FrameAttrs fa;
  AttrRef nref =
      m.v.attrs.find_attribute(AttrType::FLOAT3, displace::FRAME_NORMAL_ATTR);
  AttrRef tref =
      m.v.attrs.find_attribute(AttrType::FLOAT3, displace::FRAME_TANGENT_ATTR);
  fa.normal = nref.exists() ? static_cast<AttrData<float3> *>(nref.data) : nullptr;
  fa.tangent = tref.exists() ? static_cast<AttrData<float3> *>(tref.data) : nullptr;
  return fa;
}

} // namespace

VdmSplatStats splatDab(Mesh &m,
                       SpatialTree &tree,
                       VdmStore &store,
                       const VdmSplatParams &params)
{
  VdmSplatStats stats;

  AttrData<float2> *uv = findUvCornerLayer(m);
  FrameAttrs frames = frameAttrs(m);
  if (!uv || !frames.normal || !frames.tangent || params.radius <= 0.0f) {
    return stats;
  }

  Vector<SpatialNode *> nodes;
  tree.filterNodes(params.center, params.radius, nodes);

  float res = float(store.params.resolution);
  float dir = params.invert ? -1.0f : 1.0f;
  // Matches the draw kernel's scaling: strength · falloff · radius · 0.5.
  float amp = params.strength * params.radius * 0.5f * dir;

  Set<uint64_t> visited; // texels splatted this dab (shared UV edges/verts)
  Map<int, float> foldRadius; // per-vert ρ_min cache
  Vector<int> touchedFaces;

  // Gathered UV triangles (fan-triangulated faces under the dab), rasterized
  // in two passes: interiors first, then the one-texel dilation skirts — so a
  // skirt write can never pre-empt a neighbouring face's interior texel.
  struct SplatTri {
    int face;
    float3 co[3], no[3], tan[3];
    float px[3], py[3];
    float inv;       // 1 / signed 2-area (texel space)
    float absArea2;  // |signed 2-area|
    float lenOpp[3]; // edge length opposite each corner
    float rhoMin;
  };
  Vector<SplatTri> tris;

  for (SpatialNode *node : nodes) {
    for (int f : node->data->unique_faces) {
      if (tree.treeMesh.f.carrier[f] != int(DetailCarrier::VDM)) {
        continue;
      }

      // Gather the face's corners (verts + UVs) once, then fan-triangulate.
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
        continue;
      }

      for (int k = 1; k + 1 < int(cVerts.size()); k++) {
        int tri[3] = {0, k, k + 1};
        SplatTri T;
        T.face = f;
        float trho[3];
        for (int j = 0; j < 3; j++) {
          int vert = cVerts[tri[j]];
          float2 tuv = cUvs[tri[j]];
          T.px[j] = tuv[0] * res;
          T.py[j] = tuv[1] * res;
          T.co[j] = m.v.co[vert];
          T.no[j] = frames.normal->safe_get(vert);
          T.tan[j] = frames.tangent->safe_get(vert);
          float *rho = foldRadius.lookup_ptr(vert);
          if (!rho) {
            float r = vertexFoldRadius(m, vert, safeNormalize(T.no[j]));
            foldRadius.insert(vert, float(r));
            trho[j] = r;
          } else {
            trho[j] = *rho;
          }
        }
        // Conservative per-triangle fold radius: the tightest vert.
        T.rhoMin = std::min(trho[0], std::min(trho[1], trho[2]));

        float area2 = (T.px[1] - T.px[0]) * (T.py[2] - T.py[0]) -
                      (T.py[1] - T.py[0]) * (T.px[2] - T.px[0]);
        if (std::fabs(area2) < 1e-12f) {
          continue;
        }
        T.inv = 1.0f / area2;
        T.absArea2 = std::fabs(area2);
        for (int j = 0; j < 3; j++) {
          int a = (j + 1) % 3, b2 = (j + 2) % 3;
          float ex = T.px[b2] - T.px[a], ey = T.py[b2] - T.py[a];
          T.lenOpp[j] = std::sqrt(ex * ex + ey * ey);
        }
        tris.append(T);
      }
    }
  }

  Map<int, uint8_t> faceTouched;

  // Evaluate + write one texel from (possibly clamped) barycentrics. Returns
  // whether a texel was written (falloff or a degenerate frame can decline).
  auto splatTexel = [&](const SplatTri &T, int x, int y, float w0, float w1,
                        float w2, uint64_t key) -> bool {
    float3 base = T.co[0] * w0 + T.co[1] * w1 + T.co[2] * w2;
    float3 n = safeNormalize(T.no[0] * w0 + T.no[1] * w1 + T.no[2] * w2);
    float3 t = T.tan[0] * w0 + T.tan[1] * w1 + T.tan[2] * w2;
    t = safeNormalize(t - n * n.dot(t));
    if (n.length() < EPS || t.length() < EPS) {
      return false;
    }
    float3 b = n.cross(t);

    // World falloff from the *displaced* point (base + frame·texel).
    float3 tex = store.texel(x, y);
    float3 disp = t * tex[0] + b * tex[1] + n * tex[2];
    float d = (base + disp - params.center).length();
    float s = falloff(d, params.radius);
    if (s == 0.0f) {
      return false;
    }
    visited.add(key);

    // Tangent inversion: apply the world delta, re-express in-frame.
    float3 world = disp + params.normal * (s * amp);
    float3 newTex(world.dot(t), world.dot(b), world.dot(n));

    if (params.alpha > 0.0f) {
      float lim = params.alpha * T.rhoMin;
      float len = newTex.length();
      if (len > lim) {
        newTex *= lim / len;
        stats.texelsClamped++;
      }
    }
    store.writeTexel(x, y, newTex);
    stats.texelsTouched++;
    if (!faceTouched.contains(T.face)) {
      faceTouched.insert(T.face, uint8_t(1));
    }
    return true;
  };

  constexpr float kBaryEps = -1e-5f;
  // Pass 1: triangle interiors.
  for (const SplatTri &T : tris) {
    int x0 = int(std::floor(std::min(T.px[0], std::min(T.px[1], T.px[2])) - 0.5f));
    int x1 = int(std::ceil(std::max(T.px[0], std::max(T.px[1], T.px[2])) + 0.5f));
    int y0 = int(std::floor(std::min(T.py[0], std::min(T.py[1], T.py[2])) - 0.5f));
    int y1 = int(std::ceil(std::max(T.py[0], std::max(T.py[1], T.py[2])) + 0.5f));
    for (int y = y0; y <= y1; y++) {
      for (int x = x0; x <= x1; x++) {
        float cx = float(x) + 0.5f, cy = float(y) + 0.5f;
        float w0 = ((T.px[1] - cx) * (T.py[2] - cy) - (T.py[1] - cy) * (T.px[2] - cx)) *
                   T.inv;
        float w1 = ((T.px[2] - cx) * (T.py[0] - cy) - (T.py[2] - cy) * (T.px[0] - cx)) *
                   T.inv;
        float w2 = 1.0f - w0 - w1;
        if (w0 < kBaryEps || w1 < kBaryEps || w2 < kBaryEps) {
          continue;
        }
        // Same int-pair packing as tileKey, used here as a texel key.
        uint64_t key = (uint64_t(uint32_t(x)) << 32) | uint64_t(uint32_t(y));
        if (visited.contains(key)) {
          continue;
        }
        splatTexel(T, x, y, w0, w1, w2, key);
      }
    }
  }

  // Pass 2: one-texel dilation skirts. Gutter texels within kSkirt texels of a
  // triangle edge get the nearest on-triangle value (clamped barycentrics), so
  // bilinear reads at a UV-chart boundary don't blend toward zero. This fills
  // each chart's own gutter; cross-chart value matching is the Ptex backend's
  // job (X2).
  constexpr float kSkirt = 1.5f;
  for (const SplatTri &T : tris) {
    int pad = int(std::ceil(kSkirt)) + 1;
    int x0 =
        int(std::floor(std::min(T.px[0], std::min(T.px[1], T.px[2])) - 0.5f)) - pad;
    int x1 =
        int(std::ceil(std::max(T.px[0], std::max(T.px[1], T.px[2])) + 0.5f)) + pad;
    int y0 =
        int(std::floor(std::min(T.py[0], std::min(T.py[1], T.py[2])) - 0.5f)) - pad;
    int y1 =
        int(std::ceil(std::max(T.py[0], std::max(T.py[1], T.py[2])) + 0.5f)) + pad;
    for (int y = y0; y <= y1; y++) {
      for (int x = x0; x <= x1; x++) {
        uint64_t key = (uint64_t(uint32_t(x)) << 32) | uint64_t(uint32_t(y));
        if (visited.contains(key)) {
          continue;
        }
        float cx = float(x) + 0.5f, cy = float(y) + 0.5f;
        float w0 = ((T.px[1] - cx) * (T.py[2] - cy) - (T.py[1] - cy) * (T.px[2] - cx)) *
                   T.inv;
        float w1 = ((T.px[2] - cx) * (T.py[0] - cy) - (T.py[2] - cy) * (T.px[0] - cx)) *
                   T.inv;
        float w2 = 1.0f - w0 - w1;
        if (w0 >= kBaryEps && w1 >= kBaryEps && w2 >= kBaryEps) {
          continue; // interior texel pass 1 declined (falloff) — leave it
        }
        // Signed texel-space distance to each edge: w_i * |2A| / lenOpp_i.
        float d0 = w0 * T.absArea2 / T.lenOpp[0];
        float d1 = w1 * T.absArea2 / T.lenOpp[1];
        float d2 = w2 * T.absArea2 / T.lenOpp[2];
        if (d0 < -kSkirt || d1 < -kSkirt || d2 < -kSkirt) {
          continue;
        }
        float c0 = w0 < 0.0f ? 0.0f : w0;
        float c1 = w1 < 0.0f ? 0.0f : w1;
        float c2 = w2 < 0.0f ? 0.0f : w2;
        float sum = c0 + c1 + c2;
        if (sum < EPS) {
          continue;
        }
        splatTexel(T, x, y, c0 / sum, c1 / sum, c2 / sum, key);
      }
    }
  }

  for (const auto &pair : faceTouched) {
    touchedFaces.append(pair.key);
    stats.facesTouched++;
  }

  // Refresh the touched faces' displacement-bound pads (bounds-only dirty).
  if (touchedFaces.size() > 0) {
    Vector<float> bounds;
    exportFaceBounds(store, m,
                     std::span<const int>(touchedFaces.data(), touchedFaces.size()),
                     bounds);
    tree.setFaceDisplacementBounds(touchedFaces.data(), bounds.data(),
                                   int(touchedFaces.size()));
  }
  return stats;
}

} // namespace sculptcore::vdm
