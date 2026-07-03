#include "frames.h"

#include "mesh/attribute.h"
#include "mesh/boundary.h"
#include "mesh/mesh_iter.h"
#include "mesh/utils/mesh_validate.h" // faceNewellNormal

#include <cmath>

namespace sculptcore::displace {

using mesh::AttrData;
using mesh::AttrFlag;
using mesh::AttrRef;
using mesh::AttrType;
using mesh::BoolAttrView;
using mesh::Mesh;
using litestl::util::Vector;

// The geometric helpers below mirror brush/feature_field.cc (the FEATURE_ALIGN
// brush's incremental cross field); kept separate so displace never depends on
// the brush module. A later cleanup can migrate both onto one home.
namespace {
constexpr float EPS = 1e-9f;

inline float3 projTangent(const float3 &v, const float3 &n)
{
  return v - n * n.dot(v);
}

inline float3 safeNormalize(const float3 &v)
{
  float l = v.length();
  return l > EPS ? v * (1.0f / l) : float3(0.0f, 0.0f, 0.0f);
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

inline bool edgeIsFeature(Mesh &m,
                          int e,
                          BoolAttrView *sharp,
                          BoolAttrView *seam,
                          AttrData<int> *group)
{
  if (sharp && sharp->get(e)) {
    return true;
  }
  if (seam && seam->get(e)) {
    return true;
  }
  int f1 = ELEM_NONE, f2 = ELEM_NONE;
  int nf = edgeFaces(m, e, f1, f2);
  if (nf == 1) {
    return true;
  }
  if (nf == 2 && group) {
    if (group->safe_get(f1) != group->safe_get(f2)) {
      return true;
    }
  }
  return false;
}

float3 featureTangent(Mesh &m,
                      int v,
                      const float3 &n,
                      BoolAttrView *sharp,
                      BoolAttrView *seam,
                      AttrData<int> *group)
{
  if (m.v.e[v] == ELEM_NONE) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  float3 acc(0.0f, 0.0f, 0.0f);
  bool has = false;
  for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
    if (!edgeIsFeature(m, e, sharp, seam, group)) {
      continue;
    }
    float3 d = safeNormalize(projTangent(m.v.co[otherVert(m, e, v)] - m.v.co[v], n));
    if (d.length() < EPS) {
      continue;
    }
    if (!has) {
      acc = d;
      has = true;
    } else {
      acc += acc.dot(d) < 0.0f ? d * -1.0f : d;
    }
  }
  return safeNormalize(acc);
}

/** Transcendental-free by design: the frame provider is the cross-backend
 * synchronization anchor, and libm acos/atan2/cos/sin round differently
 * between emscripten and native (the X1 finding). Only +,-,*,/,sqrt appear
 * here — all IEEE-exact — so recomputes are bit-identical across backends.
 * The dihedral weight uses the normal chord |n1-n2| (= 2·sin(θ/2), monotone
 * in θ) and the eigen-direction comes from half-angle identities. */
float3 estimatePrincipalDir(Mesh &m, int v, const float3 &n)
{
  if (m.v.e[v] == ELEM_NONE) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  float3 X = std::fabs(n[0]) < 0.9f ? float3(1, 0, 0) : float3(0, 1, 0);
  X = safeNormalize(projTangent(X, n));
  if (X.length() < EPS) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  float3 Y = n.cross(X);

  double A00 = 0, A01 = 0, A11 = 0;
  for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
    int f1 = ELEM_NONE, f2 = ELEM_NONE;
    if (edgeFaces(m, e, f1, f2) != 2) {
      continue;
    }
    float3 n1 = safeNormalize(mesh::faceNewellNormal(m, f1));
    float3 n2 = safeNormalize(mesh::faceNewellNormal(m, f2));
    float chord = (n1 - n2).length(); // 2·sin(θ/2): the bit-stable θ proxy
    float3 evec = m.v.co[otherVert(m, e, v)] - m.v.co[v];
    float el = evec.length();
    if (el < EPS) {
      continue;
    }
    float3 eu = evec * (1.0f / el);
    float ex = eu.dot(X), ey = eu.dot(Y);
    double w = double(chord) * double(el);
    A00 += w * ex * ex;
    A01 += w * ex * ey;
    A11 += w * ey * ey;
  }
  if (A00 + A11 < 1e-7) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  // Principal axis of [[A00,A01],[A01,A11]]: (cos2θ, sin2θ) ∝ (A00−A11, 2A01),
  // halved via cosθ = √((1+cos2θ)/2), sinθ = sign(sin2θ)·√((1−cos2θ)/2).
  double c2 = A00 - A11, s2 = 2.0 * A01;
  double r = std::sqrt(c2 * c2 + s2 * s2);
  if (r < 1e-30) {
    return safeNormalize(X); // isotropic: any tangent direction is principal
  }
  double cos2 = c2 / r;
  double ct = std::sqrt(0.5 * (1.0 + cos2));
  double st = std::sqrt(0.5 * (1.0 - cos2));
  if (s2 < 0.0) {
    st = -st;
  }
  return safeNormalize(X * float(ct) + Y * float(st));
}

struct FrameAttrs {
  AttrData<float3> *normal = nullptr;
  AttrData<float3> *tangent = nullptr;
};

FrameAttrs frameAttrs(Mesh &m)
{
  FrameAttrs fa;
  AttrRef nref = m.v.attrs.find_attribute(AttrType::FLOAT3, FRAME_NORMAL_ATTR);
  AttrRef tref = m.v.attrs.find_attribute(AttrType::FLOAT3, FRAME_TANGENT_ATTR);
  fa.normal = nref.exists() ? static_cast<AttrData<float3> *>(nref.data) : nullptr;
  fa.tangent = tref.exists() ? static_cast<AttrData<float3> *>(tref.data) : nullptr;
  return fa;
}

} // namespace

void ensureFrameAttrs(Mesh &m)
{
  AttrRef &nref = m.v.attrs.ensure(AttrType::FLOAT3, FRAME_NORMAL_ATTR, false);
  nref.flag = AttrFlag::NOINTERP;
  AttrRef &tref = m.v.attrs.ensure(AttrType::FLOAT3, FRAME_TANGENT_ATTR, false);
  tref.flag = AttrFlag::NOINTERP;
}

void updateFramesRegion(Mesh &m,
                        const Vector<int> &verts,
                        const FrameProviderParams &params)
{
  if (verts.size() == 0) {
    return;
  }
  ensureFrameAttrs(m);
  FrameAttrs fa = frameAttrs(m);
  if (!fa.normal || !fa.tangent) {
    return;
  }

  BoolAttrView *sharp = params.use_features
                            ? mesh::boundary::findBoolEdgeView(&m, mesh::boundary::EDGE_SHARP)
                            : nullptr;
  BoolAttrView *seam = params.use_features
                           ? mesh::boundary::findBoolEdgeView(&m, mesh::boundary::EDGE_SEAM)
                           : nullptr;
  AttrData<int> *group = nullptr;
  if (params.use_features) {
    AttrRef gref = m.f.attrs.find_attribute(AttrType::INT, mesh::boundary::FACE_GROUP);
    group = static_cast<AttrData<int> *>(gref.data);
  }

  // Materialize the region + 1-ring (the diffusion reads neighbours through
  // operator[]) and seed the smoothed normal from the raw vertex normal.
  for (size_t i = 0; i < verts.size(); i++) {
    int v = verts[i];
    fa.normal->materialize(v);
    fa.tangent->materialize(v);
    if (m.v.e[v] != ELEM_NONE) {
      for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
        int ov = otherVert(m, e, v);
        fa.normal->materialize(ov);
        fa.tangent->materialize(ov);
      }
    }
    float3 n = safeNormalize(m.v.no[v]);
    (*fa.normal)[v] = n.length() > EPS ? n : float3(0.0f, 0.0f, 1.0f);
  }

  // Smoothed normal: Gauss-Seidel 1-ring averaging in the fixed region order.
  // Neighbours outside the region fall back to their raw vertex normal, so a
  // region-scoped update blends into the surrounding field.
  for (int it = 0; it < params.normal_smooth_iters; it++) {
    for (size_t i = 0; i < verts.size(); i++) {
      int v = verts[i];
      if (m.v.e[v] == ELEM_NONE) {
        continue;
      }
      float3 acc = (*fa.normal)[v];
      for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
        int ov = otherVert(m, e, v);
        float3 nn = (*fa.normal)[ov];
        if (nn.length() < EPS) {
          nn = safeNormalize(m.v.no[ov]);
        }
        acc += nn;
      }
      float3 nd = safeNormalize(acc);
      if (nd.length() > EPS) {
        (*fa.normal)[v] = nd;
      }
    }
  }

  // Tangent seed: hard-pin feature verts, soft-seed the rest from curvature —
  // all against the smoothed normal so frames and shading agree. Unlike the
  // brush cross field (which accumulates across dabs), the region is re-seeded
  // from geometry alone, so recomputes are bit-identical (the parity anchor).
  Vector<uint8_t> pinned;
  pinned.resize(verts.size());
  for (size_t i = 0; i < verts.size(); i++) {
    int v = verts[i];
    float3 n = (*fa.normal)[v];
    pinned[i] = 0;
    (*fa.tangent)[v] = float3(0.0f, 0.0f, 0.0f);
    if (params.use_features) {
      float3 t = featureTangent(m, v, n, sharp, seam, group);
      if (t.length() > EPS) {
        (*fa.tangent)[v] = t;
        pinned[i] = 1;
        continue;
      }
    }
    if (params.use_curvature) {
      float3 c = estimatePrincipalDir(m, v, n);
      if (c.length() > EPS) {
        (*fa.tangent)[v] = c;
      }
    }
  }

  // 4-fold-aware diffusion (see feature_field.cc): each neighbour tangent is
  // rotated to the nearest 90° image of the running accumulator before
  // summing. Serial, fixed order — the backend-parity anchor.
  for (int it = 0; it < params.diffuse_iters; it++) {
    for (size_t i = 0; i < verts.size(); i++) {
      if (pinned[i]) {
        continue;
      }
      int v = verts[i];
      if (m.v.e[v] == ELEM_NONE) {
        continue;
      }
      float3 n = (*fa.normal)[v];
      float3 acc = safeNormalize(projTangent((*fa.tangent)[v], n));
      bool has = acc.length() > EPS;
      for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
        float3 t = safeNormalize(projTangent((*fa.tangent)[otherVert(m, e, v)], n));
        if (t.length() < EPS) {
          continue;
        }
        if (!has) {
          acc = t;
          has = true;
          continue;
        }
        float3 rot = n.cross(t);
        float d1 = t.dot(acc), d2 = rot.dot(acc);
        float3 chosen = std::fabs(d1) >= std::fabs(d2) ? t : rot;
        float dd = std::fabs(d1) >= std::fabs(d2) ? d1 : d2;
        acc += dd < 0.0f ? chosen * -1.0f : chosen;
      }
      float3 nd = safeNormalize(acc);
      if (nd.length() > EPS) {
        (*fa.tangent)[v] = nd;
      }
    }
  }

  // Final orthonormalization: every region tangent exactly ⊥ its smoothed
  // normal and unit length (deterministic fallback for degenerate verts).
  for (size_t i = 0; i < verts.size(); i++) {
    int v = verts[i];
    float3 n = (*fa.normal)[v];
    float3 t = safeNormalize(projTangent((*fa.tangent)[v], n));
    if (t.length() < EPS) {
      float3 X = std::fabs(n[0]) < 0.9f ? float3(1, 0, 0) : float3(0, 1, 0);
      t = safeNormalize(projTangent(X, n));
    }
    (*fa.tangent)[v] = t;
  }
}

void updateFramesAll(Mesh &m, const FrameProviderParams &params)
{
  Vector<int> verts;
  for (int v : m.v) {
    verts.append(v);
  }
  updateFramesRegion(m, verts, params);
}

int crossFieldIndexSum(Mesh &m)
{
  FrameAttrs fa = frameAttrs(m);
  if (!fa.tangent) {
    return 0;
  }

  constexpr float HALF_PI = 1.5707963267948966f;
  int total = 0;
  for (int f : m.f) {
    float3 N = safeNormalize(mesh::faceNewellNormal(m, f));
    if (N.length() < EPS) {
      continue;
    }
    // In-plane reference basis for signed angles.
    float3 X = std::fabs(N[0]) < 0.9f ? float3(1, 0, 0) : float3(0, 1, 0);
    X = safeNormalize(projTangent(X, N));
    float3 Y = N.cross(X);

    // Corner tangents projected into the face plane, as angles mod 90°.
    mesh::FaceProxy face(&m, f);
    float acc = 0.0f;
    float first = 0.0f, prev = 0.0f;
    int nc = 0;
    bool bad = false;
    for (auto list : face.lists()) {
      for (auto c : list) {
        float3 t = safeNormalize(projTangent(fa.tangent->safe_get(c.v()), N));
        if (t.length() < EPS) {
          bad = true;
          break;
        }
        float ang = std::atan2(t.dot(Y), t.dot(X));
        if (nc == 0) {
          first = ang;
        } else {
          float d = ang - prev;
          // Nearest 90° image: fold the step into (-45°, 45°].
          while (d > HALF_PI * 0.5f) {
            d -= HALF_PI;
          }
          while (d <= -HALF_PI * 0.5f) {
            d += HALF_PI;
          }
          acc += d;
        }
        prev = ang;
        nc++;
      }
    }
    if (bad || nc < 3) {
      continue;
    }
    // Close the loop back to the first corner.
    float d = first - prev;
    while (d > HALF_PI * 0.5f) {
      d -= HALF_PI;
    }
    while (d <= -HALF_PI * 0.5f) {
      d += HALF_PI;
    }
    acc += d;
    total += int(std::lround(acc / HALF_PI));
  }
  return total;
}

} // namespace sculptcore::displace
