#include "brush/feature_field.h"

#include "mesh/attribute.h"
#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/utils/mesh_validate.h" // faceNewellNormal

#include "litestl/math/vector.h"

#include <cmath>

namespace sculptcore::brush {

using litestl::math::float3;
using litestl::util::Vector;
using sculptcore::mesh::AttrData;
using sculptcore::mesh::AttrFlag;
using sculptcore::mesh::AttrRef;
using sculptcore::mesh::AttrType;
using sculptcore::mesh::BoolAttrView;
using sculptcore::mesh::Mesh;

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

/* Faces around edge `e` via its radial cycle. Returns the face count (0 = wire,
 * 1 = mesh border, 2 = manifold interior); fills f1/f2 for the first two. */
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

/* An edge is a feature if it is flagged sharp/seam, separates two face-set
 * groups, or is a mesh border (a single incident face). */
inline bool edgeIsFeature(
    Mesh &m, int e, BoolAttrView *sharp, BoolAttrView *seam, AttrData<int> *group)
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
    return true; // open mesh border
  }
  if (nf == 2 && group) {
    if (group->safe_get(f1) != group->safe_get(f2)) {
      return true; // face-set boundary
    }
  }
  return false;
}

/* Tangent along the feature curve through `v`: the (4-fold-consistent) sum of
 * the unit directions to `v`'s feature-edge neighbours, projected into the
 * tangent plane. Zero if `v` touches no feature edge. */
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
      // The two boundary edges leave v in roughly opposite directions; flip so
      // they reinforce instead of cancelling (line-field, 180° equivalence).
      acc += acc.dot(d) < 0.0f ? d * -1.0f : d;
    }
  }
  return safeNormalize(acc);
}

/* Local max-principal-curvature direction at `v` from the 1-ring shape operator
 * (Cohen-Steiner normal cycle, |dihedral| weights — direction only, so the sign
 * is irrelevant). Zero in flat regions, leaving such verts to plain smoothing /
 * boundary diffusion. */
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
    float d = n1.dot(n2);
    d = d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d);
    float theta = std::acos(d); // dihedral magnitude (>= 0)
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
  }
  if (A00 + A11 < 1e-7) {
    return float3(0.0f, 0.0f, 0.0f); // flat: no curvature guidance
  }
  // Principal axis of the symmetric 2×2 [[A00,A01],[A01,A11]] (larger eigenvalue).
  double ang = 0.5 * std::atan2(2.0 * A01, A00 - A11);
  return safeNormalize(X * float(std::cos(ang)) + Y * float(std::sin(ang)));
}

inline float3 vertNormal(Mesh &m, int v)
{
  float3 n = safeNormalize(m.v.no[v]);
  return n.length() > EPS ? n : float3(0.0f, 0.0f, 1.0f);
}

} // namespace

void ensureCrossField(Mesh &m)
{
  AttrRef &ref = m.v.attrs.ensure(AttrType::FLOAT3, CROSS_FIELD_ATTR, false);
  // Persistent (no TEMP) so it saves; NOINTERP so a dyntopo split clears the new
  // vert instead of averaging two cross directions (which can cancel).
  ref.flag = AttrFlag::NOINTERP;
}

void updateCrossFieldRegion(Mesh &m,
                            const Vector<int> &verts,
                            const FeatureFieldParams &params)
{
  if (verts.size() == 0) {
    return;
  }
  ensureCrossField(m);
  AttrRef ref = m.v.attrs.find_attribute(AttrType::FLOAT3, CROSS_FIELD_ATTR);
  auto *field = static_cast<AttrData<float3> *>(ref.data);
  if (!field) {
    return;
  }

  BoolAttrView *sharp =
      params.use_features
          ? mesh::boundary::findBoolEdgeView(&m, mesh::boundary::EDGE_SHARP)
          : nullptr;
  BoolAttrView *seam =
      params.use_features
          ? mesh::boundary::findBoolEdgeView(&m, mesh::boundary::EDGE_SEAM)
          : nullptr;
  AttrData<int> *group = nullptr;
  if (params.use_features) {
    AttrRef gref = m.f.attrs.find_attribute(AttrType::INT, mesh::boundary::FACE_GROUP);
    group = static_cast<AttrData<int> *>(gref.data);
  }

  Vector<uint8_t> pinned;
  pinned.resize(verts.size());

  // Seed pass: materialize each region vert + its 1-ring (the kernel and the
  // diffuse below read both via operator[]); hard-seed feature verts, soft-seed
  // still-unset interior verts from curvature.
  for (size_t i = 0; i < verts.size(); i++) {
    int v = verts[i];
    field->materialize(v);
    if (m.v.e[v] != ELEM_NONE) {
      for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
        field->materialize(otherVert(m, e, v));
      }
    }
    float3 n = vertNormal(m, v);
    pinned[i] = 0;
    if (params.use_features) {
      float3 t = featureTangent(m, v, n, sharp, seam, group);
      if (t.length() > EPS) {
        (*field)[v] = t;
        pinned[i] = 1;
        continue;
      }
    }
    if (params.use_curvature && projTangent((*field)[v], n).length() < EPS) {
      float3 c = estimatePrincipalDir(m, v, n);
      if (c.length() > EPS) {
        (*field)[v] = c;
      }
    }
  }

  // Diffuse pass: propagate directions inward with the 4-fold-aware step. Each
  // neighbour's direction is rotated to the nearest 90° image of the running
  // accumulator before summing, so a cross direction and its quarter-turns
  // reinforce instead of cancelling. Gauss-Seidel in the fixed (deterministic)
  // region order — identical on both backends.
  for (int it = 0; it < params.diffuse_iters; it++) {
    for (size_t i = 0; i < verts.size(); i++) {
      if (pinned[i]) {
        continue;
      }
      int v = verts[i];
      if (m.v.e[v] == ELEM_NONE) {
        continue;
      }
      float3 n = vertNormal(m, v);
      float3 acc = safeNormalize(projTangent((*field)[v], n));
      bool has = acc.length() > EPS;
      for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
        float3 t = safeNormalize(projTangent((*field)[otherVert(m, e, v)], n));
        if (t.length() < EPS) {
          continue;
        }
        if (!has) {
          acc = t;
          has = true;
          continue;
        }
        float3 rot = n.cross(t); // t rotated 90° about the normal
        float d1 = t.dot(acc), d2 = rot.dot(acc);
        float3 chosen = std::fabs(d1) >= std::fabs(d2) ? t : rot;
        float dd = std::fabs(d1) >= std::fabs(d2) ? d1 : d2;
        acc += dd < 0.0f ? chosen * -1.0f : chosen;
      }
      float3 nd = safeNormalize(acc);
      if (nd.length() > EPS) {
        (*field)[v] = nd;
      }
    }
  }
}

} // namespace sculptcore::brush
