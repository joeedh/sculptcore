#include "remesh/preremesh.h"
#include "remesh/field/cross_field.h"

#include "dyntopo/dyntopo.h"
#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"

#include "litestl/math/vector.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cmath>

namespace sculptcore::remesh {

using namespace litestl;
using math::float3;
using mesh::Mesh;

void bkRemeshToTarget(Mesh &m, float L, uint32_t seed, const char *size_attr)
{
  m.thawTopo();

  bool have = false;
  math::float3 bmin{}, bmax{};
  for (int v : m.v) {
    math::float3 co = m.v.co[v];
    if (!have) {
      bmin = bmax = co;
      have = true;
      continue;
    }
    for (int i = 0; i < 3; i++) {
      if (co[i] < bmin[i]) bmin[i] = co[i];
      if (co[i] > bmax[i]) bmax[i] = co[i];
    }
  }
  if (!have) {
    return;
  }
  math::float3 center = (bmin + bmax) * 0.5f;
  float radius = (bmax - bmin).length(); // > half-diagonal: covers the whole mesh

  dyntopo::DynTopoParams dp;
  dp.l_max = L * (4.0f / 3.0f);
  dp.l_min = L * (4.0f / 5.0f);
  dp.mode = dyntopo::DynTopoMode::Both;
  dp.do_flips = true;
  dp.do_smooth = true;
  dp.preserve_features = false; // callers that need pinning set it up first
  dp.max_rounds = 100;
  dp.size_attr = size_attr; // null = uniform; set = per-vertex curvature sizing
  dyntopo::applyBrushDab(m, center, radius, dp, seed);
}

namespace {

// Axis-alignment emphasis for the field-aligned weighted centroid. Each ring
// neighbour keeps a unit base weight (so diagonal coupling — the shear damper on
// a structured triangulation — is never removed) plus an ADDITIVE boost for
// edges aligned with a cross arm: weight = 1 + boost·max(|cosφ|,|sinφ|)^pow. The
// boost straightens u/v rows without letting the grid shear.
constexpr float kAlignPow = 4.0f;
constexpr float kAlignBoost = 3.0f;

/* Lift the per-face cross field to a per-vertex cross frame (vU,vW orthonormal in
 * the vertex tangent plane). 4-RoSy period ambiguity is resolved by averaging the
 * incident faces' u-arms in the exp(i·4·φ) domain against a per-vertex reference
 * frame. A vertex with no usable field (attr absent, degenerate normal, no
 * contributing face) is left with zero arms so the caller falls back to isotropic
 * at that vertex. */
void liftFieldToVerts(Mesh &m, util::Vector<float3> &vU, util::Vector<float3> &vW)
{
  const int vcap = int(m.v.capacity());
  vU.resize(vcap);
  vW.resize(vcap);
  for (int i = 0; i < vcap; i++) {
    vU[i] = float3(0.0f);
    vW[i] = float3(0.0f);
  }

  if (!m.f.attrs.has(mesh::AttrType::FLOAT, util::string(".remesh.f.theta"))) {
    return; // no field → all-isotropic fallback
  }
  mesh::BuiltinAttr<float, ".remesh.f.theta"> theta;
  theta.ensure(m.f.attrs);

  const int fcap = int(m.f.capacity());
  util::Vector<float3> FX, FY, FN;
  FX.resize(fcap);
  FY.resize(fcap);
  FN.resize(fcap);

  // Per-vertex normal = unit sum of incident face normals; the frame the field
  // lift is expressed in. Face frames are the solver's faceFrame so the recovered
  // u-arm matches the field's θ convention.
  util::Vector<float3> vN, vT1, vT2;
  vN.resize(vcap);
  vT1.resize(vcap);
  vT2.resize(vcap);
  for (int i = 0; i < vcap; i++) {
    vN[i] = float3(0.0f);
  }
  for (int f : m.f) {
    faceFrame(m, f, FX[f], FY[f], FN[f]);
    int c0 = m.l.c[m.f.l[f]], cc = c0;
    do {
      vN[m.c.v[cc]] += FN[f];
      cc = m.c.next[cc];
    } while (cc != c0);
  }
  for (int v : m.v) {
    float3 n = vN[v];
    float nl = n.length();
    if (nl < 1e-20f) {
      vN[v] = float3(0.0f);
      continue;
    }
    n = n * (1.0f / nl);
    vN[v] = n;
    // A reference tangent (least-aligned world axis projected into the plane).
    float3 a = std::fabs(n[0]) < 0.9f ? float3(1.0f, 0.0f, 0.0f)
                                      : float3(0.0f, 1.0f, 0.0f);
    float3 t1 = a - n * a.dot(n);
    float t1l = t1.length();
    if (t1l < 1e-20f) {
      vN[v] = float3(0.0f);
      continue;
    }
    t1 = t1 * (1.0f / t1l);
    vT1[v] = t1;
    vT2[v] = n.cross(t1);
  }

  // Average each incident face's u-arm in exp(i·4·φ) form (period-invariant).
  util::Vector<math::float2> acc;
  acc.resize(vcap);
  for (int i = 0; i < vcap; i++) {
    acc[i] = math::float2(0.0f, 0.0f);
  }
  for (int f : m.f) {
    float t = theta[f];
    float3 d0 = FX[f] * std::cos(t) + FY[f] * std::sin(t); // a cross arm
    int c0 = m.l.c[m.f.l[f]], cc = c0;
    do {
      int v = m.c.v[cc];
      float3 n = vN[v];
      if (n.lengthSqr() > 0.25f) {
        float3 g = d0 - n * d0.dot(n); // arm projected into the vertex plane
        if (g.length() > 1e-12f) {
          float phi = std::atan2(g.dot(vT2[v]), g.dot(vT1[v]));
          acc[v][0] += std::cos(4.0f * phi);
          acc[v][1] += std::sin(4.0f * phi);
        }
      }
      cc = m.c.next[cc];
    } while (cc != c0);
  }

  for (int v : m.v) {
    float3 n = vN[v];
    if (n.lengthSqr() < 0.25f) {
      continue;
    }
    math::float2 c = acc[v];
    if (c[0] * c[0] + c[1] * c[1] < 1e-12f) {
      continue;
    }
    float phi = std::atan2(c[1], c[0]) * 0.25f;
    float3 u = vT1[v] * std::cos(phi) + vT2[v] * std::sin(phi);
    float ul = u.length();
    if (ul < 1e-20f) {
      continue;
    }
    u = u * (1.0f / ul);
    vU[v] = u;
    vW[v] = n.cross(u);
  }
}

} // namespace

void tangentialSmooth(Mesh &m, int iters, float lambda, float align)
{
  m.thawTopo();

  // Lift the field once per call (it is fixed across the inner iterations; the
  // driver recomputes it at its own outer cadence). align == 0 skips it entirely
  // so the isotropic path stays byte-identical to the classic relaxation.
  util::Vector<float3> vU, vW;
  if (align > 0.0f) {
    liftFieldToVerts(m, vU, vW);
  }

  util::Vector<float3> nco;
  nco.resize(int(m.v.capacity()));
  util::Vector<float3> ring;
  for (int it = 0; it < iters; it++) {
    for (int v : m.v) {
      float3 vco = m.v.co[v];
      int e0 = m.v.e[v];
      if (e0 == ELEM_NONE) {
        nco[v] = vco;
        continue;
      }
      ring.clear();
      int ec = e0, guard = 0;
      do {
        int ov = m.e.vs[ec][0] == v ? m.e.vs[ec][1] : m.e.vs[ec][0];
        ring.append(m.v.co[ov]);
        int side = m.e.vs[ec][0] == v ? 0 : 1;
        ec = m.e.disk[ec][side * 2 + 1];
      } while (ec != e0 && ++guard < 256);
      int k = int(ring.size());
      if (k < 3) {
        nco[v] = vco;
        continue;
      }
      math::float3 cen{};
      for (int i = 0; i < k; i++) cen += ring[i];
      cen = cen * (1.0f / float(k));

      // Tier 9a: bend the target centroid toward a cross-field-aligned one so the
      // smooth straightens u/v rows instead of washing flow out. Weighted ring
      // centroid favouring neighbours aligned with the local cross arms; lerp by
      // `align`. No field at this vertex ⇒ stays isotropic.
      if (align > 0.0f && vU[v].lengthSqr() > 0.25f) {
        float3 u = vU[v], w = vW[v];
        math::float3 facc{};
        float wsum = 0.0f;
        for (int i = 0; i < k; i++) {
          float3 e = ring[i] - vco;
          float el = e.length();
          if (el < 1e-20f) continue;
          float3 eh = e * (1.0f / el);
          float au = std::fabs(eh.dot(u)), aw = std::fabs(eh.dot(w));
          float wgt = 1.0f + kAlignBoost * std::pow(au > aw ? au : aw, kAlignPow);
          facc += e * wgt;
          wsum += wgt;
        }
        if (wsum > 1e-20f) {
          float3 cen_field = vco + facc * (1.0f / wsum);
          cen = cen * (1.0f - align) + cen_field * align;
        }
      }

      math::float3 nrm{}; // Newell normal of the ordered ring polygon about cen
      float minlen = 1e30f;
      for (int i = 0; i < k; i++) {
        math::float3 a = ring[i] - cen, b = ring[(i + 1) % k] - cen;
        nrm += a.cross(b);
        float el = (ring[i] - vco).length();
        if (el < minlen) minlen = el;
      }
      math::float3 d = cen - vco;
      float nl = nrm.length();
      if (nl > 1e-20f) {
        math::float3 un = nrm * (1.0f / nl);
        d = d - un * d.dot(un); // tangent-plane component only
      }
      float dl = d.length();
      if (dl > minlen && dl > 1e-20f) d = d * (minlen / dl); // clamp to edge scale
      nco[v] = vco + d * lambda;
    }
    for (int v : m.v) {
      m.v.co[v] = nco[v];
    }
  }
  m.recalc_normals();
}

} // namespace sculptcore::remesh
