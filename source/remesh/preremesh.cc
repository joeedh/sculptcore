#include "remesh/preremesh.h"
#include "remesh/field/cross_field.h"
#include "remesh/field/density.h"

#include "dyntopo/dyntopo.h"
#include "mesh/attribute_builtin.h"
#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "mesh/utils/mesh_validate.h" // faceNewellNormal

#include "litestl/math/vector.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cmath>

namespace sculptcore::remesh {

using namespace litestl;
using math::float3;
using mesh::Mesh;

void classifyFeatures(Mesh &m, float sharp_angle)
{
  m.thawTopo();
  namespace bnd = mesh::boundary;

  const float cos_thresh = std::cos(sharp_angle);
  for (int e : m.e) {
    int c1 = m.e.c[e];
    bool feature = false;
    if (c1 == ELEM_NONE) {
      feature = true; // wire edge (no incident face)
    } else {
      int c2 = m.c.radial_next[c1];
      if (c2 == c1 || m.c.radial_next[c2] != c1) {
        feature = true; // open boundary (1 face) or non-manifold (>2)
      } else {
        float3 n1 = mesh::faceNewellNormal(m, m.l.f[m.c.l[c1]]);
        float3 n2 = mesh::faceNewellNormal(m, m.l.f[m.c.l[c2]]);
        float l1 = n1.length(), l2 = n2.length();
        if (l1 > 1e-20f && l2 > 1e-20f) {
          float d = n1.dot(n2) / (l1 * l2);
          d = d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d);
          feature = d < cos_thresh; // dihedral exceeds the threshold
        }
      }
    }
    // Tag boundary + dihedral-sharp creases into the same overlay so the feature
    // views treat them uniformly (a topological boundary is geometrically sharp).
    bnd::setEdgeFlag(&m, bnd::EDGE_SHARP, e, feature);
  }
  bnd::recomputeDirty(&m); // build the per-vertex class the smooth/collapse pin on
}

void bkRemeshToTarget(Mesh &m, float L, uint32_t seed, const char *size_attr,
                      bool preserve_features, dyntopo::DynTopoTrace *trace)
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
  /* l_min = l_max/2, not BK's classic 4/5·L: a split's children land at exactly
   * l_max/2, so any l_min above that puts fresh children inside the collapse band
   * and the pass churns split↔collapse forever instead of converging. */
  dp.l_min = L * (2.0f / 3.0f);
  dp.mode = dyntopo::DynTopoMode::Both;
  dp.do_flips = true;
  // Pinned mode hands relaxation to the caller's feature-aware smooth: BK's own
  // smooth can't pin freshly-split midpoints (unclassified until recomputeDirty),
  // so it would drift them off a crease. Geometry-only mode keeps it on.
  dp.do_smooth = !preserve_features;
  dp.preserve_features = preserve_features; // caller ran classifyFeatures first
  dp.max_rounds = 100;
  dp.size_attr = size_attr; // null = uniform; set = per-vertex curvature sizing
  dp.trace = trace;         // null = no tracing; set = append this dab's rounds
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

void tangentialSmooth(Mesh &m, int iters, float lambda, float align, bool fold_guard)
{
  m.thawTopo();

  // Lift the field once per call (it is fixed across the inner iterations; the
  // driver recomputes it at its own outer cadence). align == 0 skips it entirely
  // so the isotropic path stays byte-identical to the classic relaxation.
  util::Vector<float3> vU, vW;
  if (align > 0.0f) {
    liftFieldToVerts(m, vU, vW);
  }

  // Tier 9c: pin feature verts (boundary/sharp class != 0). Resolved once — absent
  // overlay (geometry-only callers like --solve decimation) ⇒ no pinning, so the
  // isotropic path stays byte-identical.
  bool have_feat =
      m.v.attrs.has(mesh::AttrType::INT, util::string(mesh::boundary::VERT_CLASS));
  mesh::BuiltinAttr<int, ".boundary.vert.class"> vclass;
  if (have_feat) {
    vclass.ensure(m.v.attrs);
  }

  util::Vector<float3> nco;
  nco.resize(int(m.v.capacity()));
  util::Vector<float3> ring;
  util::Vector<float3> fan_nb, fan_na; // per-fan-tri Newell normals (fold guard)
  for (int it = 0; it < iters; it++) {
    for (int v : m.v) {
      float3 vco = m.v.co[v];
      if (have_feat && vclass[v] != 0) {
        nco[v] = vco; // pinned feature vert — don't slide it off its curve
        continue;
      }
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
      const bool field_here = align > 0.0f && vU[v].lengthSqr() > 0.25f;
      if (field_here) {
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
      math::float3 fan{}; // incident-triangle normal about v (field path, see below)
      float minlen = 1e30f;
      for (int i = 0; i < k; i++) {
        math::float3 a = ring[i] - cen, b = ring[(i + 1) % k] - cen;
        nrm += a.cross(b);
        fan += (ring[i] - vco).cross(ring[(i + 1) % k] - vco);
        float el = (ring[i] - vco).length();
        if (el < minlen) minlen = el;
      }
      // Project out the off-surface component. The field path uses the fan normal
      // (sum of v's incident-triangle normals): for a flat-face vert whose 1-ring
      // reaches across a crease the ring-polygon Newell tents diagonally and would
      // let the aligned target pull v off-surface (a coarse cube then bulges),
      // whereas the fan normal stays on the face. Field-less verts (incl. all of
      // align==0) keep the ring Newell so the isotropic path stays byte-identical
      // to the classic relaxation — and no-field align>0 degrades to exactly it.
      math::float3 d = cen - vco;
      const math::float3 &pnrm = field_here ? fan : nrm;
      float nl = pnrm.length();
      if (nl > 1e-20f) {
        math::float3 un = pnrm * (1.0f / nl);
        d = d - un * d.dot(un); // tangent-plane component only
      }
      float dl = d.length();
      // Clamp to half the shortest incident edge (matching dyntopo's
      // smoothTangent): a full-edge move can land past a neighbor and fold the fan.
      float dmax = 0.5f * minlen;
      if (dl > dmax && dl > 1e-20f) d = d * (dmax / dl);
      math::float3 np = vco + d * lambda;
      // Fold guard (opt-in): cancel a move that NEWLY folds the fan — a tri that
      // agreed with the fan normal flipping against it, or a previously-unfolded
      // pair of consecutive fan tris creasing past 90°. Only good→bad transitions
      // are blocked, so already-folded fans stay free to relax themselves flat.
      if (fold_guard) {
        bool flips = false;
        fan_nb.clear();
        fan_na.clear();
        math::float3 fan_after{};
        for (int i = 0; i < k; i++) {
          math::float3 r0 = ring[i], r1 = ring[(i + 1) % k];
          fan_nb.append((r0 - vco).cross(r1 - vco));
          fan_na.append((r0 - np).cross(r1 - np));
          fan_after += fan_na[i];
        }
        for (int i = 0; i < k && !flips; i++) {
          int j = (i + 1) % k;
          flips = (fan_nb[i].dot(fan) >= 0.0f && fan_na[i].dot(fan_after) < 0.0f) ||
                  (fan_nb[i].dot(fan_nb[j]) >= 0.0f &&
                   fan_na[i].dot(fan_na[j]) < 0.0f);
        }
        if (flips) {
          np = vco;
        }
      }
      nco[v] = np;
    }
    // The per-vertex guard is predictive against FIXED neighbors, but the update
    // is Jacobi — combined moves can fold a fan no single move would. Revert any
    // vert whose fan goes good→bad in the all-moved state (plus its ring — one of
    // them caused it) and re-sweep; reverting only shrinks the moved set, so a
    // fixed point exists (at worst the fold-free before-state).
    if (fold_guard) {
      util::Vector<int> ringv;
      auto fanFolded = [&](int v, auto &&co) -> bool {
        int e0 = m.v.e[v];
        if (e0 == ELEM_NONE) {
          return false;
        }
        ringv.clear();
        int ec = e0, guard = 0;
        do {
          int ov = m.e.vs[ec][0] == v ? m.e.vs[ec][1] : m.e.vs[ec][0];
          ringv.append(ov);
          int side = m.e.vs[ec][0] == v ? 0 : 1;
          ec = m.e.disk[ec][side * 2 + 1];
        } while (ec != e0 && ++guard < 256);
        int k = int(ringv.size());
        if (k < 3) {
          return false;
        }
        float3 vc = co(v);
        fan_nb.clear();
        math::float3 fsum{};
        for (int i = 0; i < k; i++) {
          math::float3 n = (co(ringv[i]) - vc).cross(co(ringv[(i + 1) % k]) - vc);
          fan_nb.append(n);
          fsum += n;
        }
        for (int i = 0; i < k; i++) {
          if (fan_nb[i].dot(fsum) < 0.0f ||
              fan_nb[i].dot(fan_nb[(i + 1) % k]) < 0.0f) {
            return true;
          }
        }
        return false;
      };
      auto before = [&](int v) -> float3 { return m.v.co[v]; };
      auto after = [&](int v) -> float3 { return nco[v]; };
      bool any = true;
      for (int pass = 0; pass < 100 && any; pass++) {
        any = false;
        for (int v : m.v) {
          if (!fanFolded(v, after) || fanFolded(v, before)) {
            continue;
          }
          // ringv still holds v's ring from the `before` call above
          nco[v] = m.v.co[v];
          for (int ov : ringv) {
            nco[ov] = m.v.co[ov];
          }
          any = true;
        }
      }
    }
    for (int v : m.v) {
      m.v.co[v] = nco[v];
    }
  }
  m.recalc_normals();
}

namespace {

// Internal per-vertex size-scale layer the driver feeds to bkRemeshToTarget. Maps
// Tier 3's .remesh.v.density d(v) to the dimensionless scale s(v) = 1/sqrt(d) the
// BK band consumes (d > 1 → s < 1 → refine high-curvature; d < 1 → coarsen flat).
constexpr const char *kSizeAttr = ".remesh.v.presize";

void writeSizeScale(Mesh &m, float dmin, float dmax)
{
  if (!m.v.attrs.has(mesh::AttrType::FLOAT, util::string(".remesh.v.density"))) {
    return;
  }
  mesh::BuiltinAttr<float, ".remesh.v.density"> density;
  mesh::BuiltinAttr<float, ".remesh.v.presize"> size;
  density.ensure(m.v.attrs);
  size.ensure(m.v.attrs);
  for (int v : m.v) {
    float d = density[v];
    if (d < dmin) d = dmin;
    if (d > dmax) d = dmax;
    size[v] = d > 1e-12f ? 1.0f / std::sqrt(d) : 1.0f;
  }
}

} // namespace

void preRemesh(Mesh &m, const PreRemeshParams &p, PreRemeshStats *stats)
{
  m.thawTopo();
  const float L = p.target;
  if (L <= 0.0f || p.iters <= 0) {
    return; // no-op guard (the pipeline resolves target == 0 → target_edge_length)
  }

  // 1. Bootstrap: isotropic denoise sweeps before the field is trusted — a
  //    field-aligned smooth over a still-noisy field over-regularizes to its noise.
  //    9c: features are classified AFTER this, inside the loop, so the bootstrap
  //    removes high-frequency noise before it can be mistaken for sharp features
  //    (on a noisy input a 45° dihedral test would otherwise pin the noise). Clean
  //    input that should keep crisp features from the start sets bootstrap_iters=0.
  if (p.bootstrap_iters > 0) {
    tangentialSmooth(m, p.bootstrap_iters, p.smooth_lambda, 0.0f,
                     /*fold_guard=*/true);
  }

  const int cadence = p.field_cadence > 0 ? p.field_cadence : 1;
  util::Vector<float3> preSmooth;

  for (int it = 0; it < p.iters; it++) {
    // 2. Rough cross field (cheap SimplicialLDLT; cadenced — the field is stable
    //    once the geometry settles, so it need not be resolved every iter).
    if (p.align > 0.0f && (it % cadence) == 0) {
      CrossFieldParams cp;
      cp.use_curvature = true;
      cp.use_sharp_features = true;
      cp.seed = p.seed;
      computeCrossField(m, cp);
    }

    // Size field: regenerate Tier-3 density on the current triangulation and map it
    // to the scale layer the BK band reads. null size_attr ⇒ uniform target L.
    const char *size_attr = nullptr;
    if (p.density) {
      DensityParams dpa;
      dpa.target_edge_length = L;
      dpa.density_min = p.density_min;
      dpa.density_max = p.density_max;
      generateAutoDensity(m, dpa);
      // Tier 3b gradation limit: bound the size field's growth rate so the BK
      // band never steps sharply across an edge (a size cliff makes the
      // split/collapse loop churn pathologically at the boundary).
      limitDensityGradation(m, L, p.gradation, p.gradation_iters, p.density_min,
                            p.density_max);
      writeSizeScale(m, p.density_min, p.density_max);
      size_attr = kSizeAttr;
    }

    // 9c: reclassify on the current geometry so BK pins the features the smoothed
    //     mesh actually has (a crease the field-smooth softened drops out; the BK
    //     pass then sees the up-to-date overlay).
    if (p.preserve_features) {
      classifyFeatures(m, p.sharp_angle);
    }

    // 3. Botsch-Kobbelt to the size field (split long / collapse short / flip).
    //    With a trace attached, stamp this dab's rounds with the outer iter so the
    //    accumulated series reads as one continuous multi-iter time-line.
    size_t trace0 = p.trace ? p.trace->rounds.size() : 0;
    bkRemeshToTarget(m, L, p.seed + uint32_t(it) + 1u, size_attr,
                     p.preserve_features, p.trace);
    if (p.trace) {
      for (size_t i = trace0; i < p.trace->rounds.size(); i++) {
        p.trace->rounds[i].iter = it;
      }
    }

    // 9c: BK split/collapse marked the geometry it created boundary-dirty but did
    //     not reclassify — refresh the per-vertex class so the smooth below pins
    //     the fresh split midpoints sitting on a crease.
    if (p.preserve_features) {
      mesh::boundary::recomputeDirty(&m);
    }

    // 4. Field-aligned smooth (9a). The smooth preserves topology, so capturing
    //    positions across it gives a well-defined convergence measure.
    const bool measure = p.converge_eps > 0.0f;
    if (measure) {
      preSmooth.resize(int(m.v.capacity()));
      for (int v : m.v) {
        preSmooth[v] = m.v.co[v];
      }
    }
    tangentialSmooth(m, p.smooth_iters, p.smooth_lambda, p.align,
                     /*fold_guard=*/true);
    if (stats) {
      stats->iters_run = it + 1;
    }
    if (measure) {
      double mv = 0.0;
      for (int v : m.v) {
        mv = std::fmax(mv, double((m.v.co[v] - preSmooth[v]).length()));
      }
      if (mv < double(p.converge_eps) * double(L)) {
        if (stats) {
          stats->converged = true;
        }
        break; // the relaxation has settled
      }
    }
  }
}

} // namespace sculptcore::remesh
