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

      bool touched = false;
      for (int k = 1; k + 1 < int(cVerts.size()); k++) {
        int tri[3] = {0, k, k + 1};
        float2 tuv[3];
        float3 tco[3], tno[3], ttan[3];
        float trho[3];
        for (int j = 0; j < 3; j++) {
          int vert = cVerts[tri[j]];
          tuv[j] = cUvs[tri[j]];
          tco[j] = m.v.co[vert];
          tno[j] = frames.normal->safe_get(vert);
          ttan[j] = frames.tangent->safe_get(vert);
          float *rho = foldRadius.lookup_ptr(vert);
          if (!rho) {
            float r = vertexFoldRadius(m, vert, safeNormalize(tno[j]));
            foldRadius.insert(vert, float(r));
            trho[j] = r;
          } else {
            trho[j] = *rho;
          }
        }
        // Conservative per-triangle fold radius: the tightest vert.
        float rhoMin = std::min(trho[0], std::min(trho[1], trho[2]));

        // Rasterize the UV triangle over texel centers (texel space).
        float px[3], py[3];
        for (int j = 0; j < 3; j++) {
          px[j] = tuv[j][0] * res;
          py[j] = tuv[j][1] * res;
        }
        float area2 = (px[1] - px[0]) * (py[2] - py[0]) -
                      (py[1] - py[0]) * (px[2] - px[0]);
        if (std::fabs(area2) < 1e-12f) {
          continue;
        }
        float inv = 1.0f / area2;
        int x0 = int(std::floor(std::min(px[0], std::min(px[1], px[2])) - 0.5f));
        int x1 = int(std::ceil(std::max(px[0], std::max(px[1], px[2])) + 0.5f));
        int y0 = int(std::floor(std::min(py[0], std::min(py[1], py[2])) - 0.5f));
        int y1 = int(std::ceil(std::max(py[0], std::max(py[1], py[2])) + 0.5f));

        for (int y = y0; y <= y1; y++) {
          for (int x = x0; x <= x1; x++) {
            float cx = float(x) + 0.5f, cy = float(y) + 0.5f;
            float w0 = ((px[1] - cx) * (py[2] - cy) - (py[1] - cy) * (px[2] - cx)) * inv;
            float w1 = ((px[2] - cx) * (py[0] - cy) - (py[2] - cy) * (px[0] - cx)) * inv;
            float w2 = 1.0f - w0 - w1;
            constexpr float kBaryEps = -1e-5f;
            if (w0 < kBaryEps || w1 < kBaryEps || w2 < kBaryEps) {
              continue;
            }
            // Same int-pair packing as tileKey, used here as a texel key.
            uint64_t key = (uint64_t(uint32_t(x)) << 32) | uint64_t(uint32_t(y));
            if (visited.contains(key)) {
              continue;
            }

            // Interpolated base point + orthonormalized frame at the texel.
            float3 base = tco[0] * w0 + tco[1] * w1 + tco[2] * w2;
            float3 n = safeNormalize(tno[0] * w0 + tno[1] * w1 + tno[2] * w2);
            float3 t = ttan[0] * w0 + ttan[1] * w1 + ttan[2] * w2;
            t = safeNormalize(t - n * n.dot(t));
            if (n.length() < EPS || t.length() < EPS) {
              continue;
            }
            float3 b = n.cross(t);

            // World falloff from the *displaced* point (base + frame·texel).
            float3 tex = store.texel(x, y);
            float3 disp = t * tex[0] + b * tex[1] + n * tex[2];
            float d = (base + disp - params.center).length();
            float s = falloff(d, params.radius);
            if (s == 0.0f) {
              continue;
            }
            visited.add(key);

            // Tangent inversion: apply the world delta, re-express in-frame.
            float3 world = disp + params.normal * (s * amp);
            float3 newTex(world.dot(t), world.dot(b), world.dot(n));

            if (params.alpha > 0.0f) {
              float lim = params.alpha * rhoMin;
              float len = newTex.length();
              if (len > lim) {
                newTex *= lim / len;
                stats.texelsClamped++;
              }
            }
            store.writeTexel(x, y, newTex);
            stats.texelsTouched++;
            touched = true;
          }
        }
      }
      if (touched) {
        touchedFaces.append(f);
        stats.facesTouched++;
      }
    }
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
