#include "remesh/field/constraints.h"
#include "remesh/field/cross_field.h"
#include "remesh/field/curvature.h"
#include "remesh/field/feature_tag.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"

#include "litestl/math/vector.h"
#include "litestl/util/string.h"

#include <algorithm>
#include <cmath>

namespace sculptcore::remesh {

using litestl::math::float2;
using litestl::math::float3;
using litestl::util::Vector;
using sculptcore::mesh::AttrFlag;
using sculptcore::mesh::AttrType;
using sculptcore::mesh::BuiltinAttr;
using sculptcore::mesh::Mesh;

namespace {
// World axis least aligned with N (deterministic tie-break X<Y<Z); the ambient
// comb direction for a flat region. A coplanar region picks one axis, so it is
// combed coherently.
float3 ambientAxis(const float3 &N)
{
  float ax = std::fabs(N[0]), ay = std::fabs(N[1]), az = std::fabs(N[2]);
  if (ax <= ay && ax <= az)
    return float3(1.0f, 0.0f, 0.0f);
  if (ay <= az)
    return float3(0.0f, 1.0f, 0.0f);
  return float3(0.0f, 0.0f, 1.0f);
}

// Below this curvature anisotropy a face counts as "flat": its crease is not
// hard-pinned (that would cone a capped disk) and it is combed by ambient
// guidance instead.
constexpr float FLAT_ANISO = 0.35f;
// ...and only if its curvature MAGNITUDE is low (radius of curvature large vs the
// mesh). Anisotropy alone calls a uniformly-curved sphere "flat" (kmin==kmax), so
// it would be ambient-combed and wrecked; gating on kmax*bbox_diag separates a
// genuine plane/cap (≈0) from an isotropically curved surface.
constexpr float FLAT_CURV = 0.5f;
// Ambient comb weight (soft). Comparable to a curvature pin so a flat cap grids,
// but it only applies on flat faces so it never fights a curved region.
constexpr float AMBIENT_W = 1.0f;
} // namespace

void gatherConstraints(Mesh &m, const CrossFieldParams &params,
                       Vector<FaceConstraint> &out)
{
  m.recalc_normals();

  const int fcap = int(m.f.capacity());
  out.resize(fcap);
  for (int i = 0; i < fcap; i++) {
    out[i] = FaceConstraint{};
  }

  // M1 inputs, computed on demand.
  if (params.use_curvature) {
    computeCurvature(m);
  }
  if (params.use_sharp_features) {
    computeFeatureTags(m, params.sharp_angle);
  }

  BuiltinAttr<float3, ".remesh.v.kmax_dir"> kmax_dir;
  BuiltinAttr<float2, ".remesh.v.k"> kval;
  BuiltinAttr<bool, ".remesh.e.is_sharp"> is_sharp;
  BuiltinAttr<bool, ".remesh.e.is_boundary"> is_boundary;
  if (params.use_curvature) {
    kmax_dir.ensure(m.v.attrs);
    kval.ensure(m.v.attrs);
  }
  if (params.use_sharp_features) {
    is_sharp.ensure(m.e.attrs);
    is_boundary.ensure(m.e.attrs);
  }

  // Optional user stroke direction (per face); absent on most meshes.
  bool have_stroke =
      m.f.attrs.has(AttrType::FLOAT3, litestl::util::string(".remesh.f.stroke_dir"));
  BuiltinAttr<float3, ".remesh.f.stroke_dir"> stroke;
  if (have_stroke) {
    stroke.ensure(m.f.attrs);
  }

  auto cross4 = [](float ang, float &cx, float &cy) {
    cx = std::cos(4.0f * ang);
    cy = std::sin(4.0f * ang);
  };

  float ambient_w = AMBIENT_W, flat_aniso = FLAT_ANISO, flat_curv = FLAT_CURV;

  // Mesh scale: bbox diagonal, so the flatness curvature test is scale-free
  // (flat ⇔ radius of curvature ≫ object size ⇔ kmax * diag small).
  float bbmin[3], bbmax[3];
  bool bbinit = false;
  for (int v : m.v) {
    float3 p = m.v.co[v];
    for (int i = 0; i < 3; i++) {
      if (!bbinit || p[i] < bbmin[i]) bbmin[i] = p[i];
      if (!bbinit || p[i] > bbmax[i]) bbmax[i] = p[i];
    }
    bbinit = true;
  }
  float bbdiag = bbinit ? std::sqrt((bbmax[0] - bbmin[0]) * (bbmax[0] - bbmin[0]) +
                                    (bbmax[1] - bbmin[1]) * (bbmax[1] - bbmin[1]) +
                                    (bbmax[2] - bbmin[2]) * (bbmax[2] - bbmin[2]))
                         : 1.0f;
  if (bbdiag < 1e-8f) bbdiag = 1.0f;

  for (int f : m.f) {
    float3 X, Y, N;
    faceFrame(m, f, X, Y, N);

    // Curvature gather (soft direction + the flatness signal). Flatness uses the
    // MIN vertex anisotropy: a fan-cap face always contains the flat apex vertex,
    // whereas a body face's corners are all curved — averaging would be polluted
    // by the rim vertices the cap shares with the curved body.
    float sx = 0.0f, sy = 0.0f, aniso_min = 1.0f, kmag_at_min = 0.0f;
    int nverts = 0;
    if (params.use_curvature) {
      int c0 = m.l.c[m.f.l[f]], cc = c0;
      do {
        int v = m.c.v[cc];
        float kmin = kval[v][0], kmax = kval[v][1];
        float aniso =
            std::fabs(kmax - kmin) / (std::fabs(kmax) + std::fabs(kmin) + 1e-6f);
        float3 kd = kmax_dir[v];
        float ang = std::atan2(kd.dot(Y), kd.dot(X));
        float cx, cy;
        cross4(ang, cx, cy);
        sx += aniso * cx;
        sy += aniso * cy;
        // Track curvature magnitude at the most-isotropic (apex) vertex — the same
        // vertex aniso_min keys on, so a fan-cap face is judged by its flat apex
        // rather than the curved rim verts it shares with the body.
        if (aniso < aniso_min) {
          aniso_min = aniso;
          kmag_at_min = std::max(std::fabs(kmin), std::fabs(kmax));
        }
        nverts++;
        cc = m.c.next[cc];
      } while (cc != c0);
    }
    // Flat ⇔ near-isotropic AND low absolute curvature (vs mesh scale). The
    // magnitude gate keeps a uniformly-curved sphere out of the flat/ambient path.
    bool flat = params.use_curvature && nverts > 0 && aniso_min < flat_aniso &&
                kmag_at_min * bbdiag < flat_curv;

    // Hard constraints: user stroke (always) + sharp/boundary creases (only on
    // non-flat faces — pinning a flat crease-bounded disk circumferentially
    // makes a non-quad-meshable +1 cone; flat faces are combed by ambient).
    float hx = 0.0f, hy = 0.0f;
    bool has_hard = false;
    if (have_stroke) {
      float3 s = stroke[f];
      if (s.length() > 1e-8f) {
        float ang = std::atan2(s.dot(Y), s.dot(X));
        float cx, cy;
        cross4(ang, cx, cy);
        hx += cx;
        hy += cy;
        has_hard = true;
      }
    }
    // Hard constraints: sharp/boundary creases, but only on non-flat faces.
    // Pinning a flat crease-bounded cap circumferentially cones it at its center,
    // and a +1 cone on the cap axis makes a circumferential body loop wind 360°,
    // collapsing the cylinder-body param. Flat caps are combed by ambient instead
    // (cones move to the rim, keeping the body griddable).
    if (params.use_sharp_features && !flat) {
      int c0 = m.l.c[m.f.l[f]], cc = c0;
      do {
        int e = m.c.e[cc];
        if (e != ELEM_NONE && (is_sharp[e] || is_boundary[e])) {
          float3 d = m.v.co[m.e.vs[e][1]] - m.v.co[m.e.vs[e][0]];
          if (d.length() > 1e-12f) {
            float ang = std::atan2(d.dot(Y), d.dot(X));
            float cx, cy;
            cross4(ang, cx, cy);
            hx += cx;
            hy += cy;
            has_hard = true;
          }
        }
        cc = m.c.next[cc];
      } while (cc != c0);
    }
    if (has_hard) {
      float len = std::sqrt(hx * hx + hy * hy);
      if (len > 1e-8f)
        out[f] = FaceConstraint{hx / len, hy / len, 1.0f, true};
      continue;
    }

    // Flat face, no hard pin: ambient comb toward a coherent world axis so a cap
    // grids instead of swirling into a +1 cone (the index moves to the rim).
    if (flat) {
      float3 g = ambientAxis(N);
      float ang = std::atan2(g.dot(Y), g.dot(X));
      float cx, cy;
      cross4(ang, cx, cy);
      out[f] = FaceConstraint{cx, cy, ambient_w, false};
      continue;
    }

    // Curved face: soft curvature constraint, anisotropy-weighted so a soft pull
    // never overpowers the per-edge smoothness term (≈ 1 per interior edge).
    if (params.use_curvature) {
      float len = std::sqrt(sx * sx + sy * sy);
      if (len > 1e-8f && nverts > 0) {
        out[f] = FaceConstraint{sx / len, sy / len,
                                params.curvature_weight * len / float(nverts),
                                false};
      }
    }
  }
}

} // namespace sculptcore::remesh
