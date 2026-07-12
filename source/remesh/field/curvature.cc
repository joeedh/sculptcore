#include "remesh/field/curvature.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "mesh/utils/mesh_validate.h" // faceNewellNormal

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include "eigen/include/eigen5/Eigen/Core"
#include "eigen/include/eigen5/Eigen/Eigenvalues"

#include <cmath>

namespace sculptcore::remesh {

using litestl::math::float2;
using litestl::math::float3;
using litestl::util::Vector;
using sculptcore::mesh::AttrFlag;
using sculptcore::mesh::BuiltinAttr;
using sculptcore::mesh::Mesh;

/* Discrete principal-curvature estimation via the normal-cycle shape operator.
 *
 * Per vertex we accumulate, over its incident manifold edges, the rank-1
 * tensor  beta(e) * |e|/2 * (e_hat (x) e_hat), then divide by the barycentric
 * vertex area. beta(e) is the SIGNED dihedral angle across the edge; its sign
 * is taken winding-independently from whether the neighbour face's centroid
 * sits behind the first face's normal (convex => positive curvature).
 *
 * The tensor's tangent eigenpairs give the principal curvatures, but with the
 * directions CROSSED relative to the magnitudes: the most-bent edge direction
 * (large eigenvalue) is the principal direction of MINIMUM surface curvature.
 * Hence kmax_dir = eigenvector(small eigenvalue), kmin_dir = eigenvector(large)
 * — verified on a cylinder (kmax ~ 1/R circumferential, kmin ~ 0 axial). */
namespace {
// Symmetric 3x3 shape-operator tensor, stored per vertex for Tier 2 diffusion.
struct Tensor3 {
  double m[3][3];
};
} // namespace

void computeCurvature(Mesh &m, const CurvatureParams &params)
{
  m.recalc_normals(); // thaws topology, ensures m.v.no

  // Output layers (TEMP: intermediate field guidance, never serialized).
  BuiltinAttr<float3, ".remesh.v.kmin_dir", AttrFlag::TEMP> kmin_dir;
  BuiltinAttr<float3, ".remesh.v.kmax_dir", AttrFlag::TEMP> kmax_dir;
  BuiltinAttr<float2, ".remesh.v.k", AttrFlag::TEMP> kval;
  kmin_dir.ensure(m.v.attrs);
  kmax_dir.ensure(m.v.attrs);
  kval.ensure(m.v.attrs);

  const int vcap = int(m.v.capacity());
  const int fcap = int(m.f.capacity());

  // Per-face geometry: unit normal, area, centroid (from the Newell normal).
  Vector<float3> fn, fcent;
  Vector<float> farea;
  fn.resize(fcap);
  fcent.resize(fcap);
  farea.resize(fcap);

  for (int f : m.f) {
    float3 nn = mesh::faceNewellNormal(m, f);
    float len = nn.length();
    farea[f] = 0.5f * len;
    fn[f] = len > 1e-20f ? nn * (1.0f / len) : float3(0.0f, 0.0f, 0.0f);

    float3 c(0.0f, 0.0f, 0.0f);
    int n = 0;
    int li = m.f.l[f], c0 = m.l.c[li], cc = c0;
    do {
      c += m.v.co[m.c.v[cc]];
      n++;
      cc = m.c.next[cc];
    } while (cc != c0);
    fcent[f] = n > 0 ? c * (1.0f / float(n)) : c;
  }

  // Barycentric vertex area: each face shares its area equally to its verts.
  Vector<float> varea;
  varea.resize(vcap);
  for (int i = 0; i < vcap; i++) {
    varea[i] = 0.0f;
  }
  for (int f : m.f) {
    int li = m.f.l[f], n = m.l.size[li];
    float share = farea[f] / float(n > 0 ? n : 1);
    int c0 = m.l.c[li], cc = c0;
    do {
      varea[m.c.v[cc]] += share;
      cc = m.c.next[cc];
    } while (cc != c0);
  }

  // Accumulate the area-normalized shape operator per vertex into a tensor
  // field, so Tier 2 can diffuse it before the eigendecomposition. Stored as
  // M = T / varea[v] (the historical normalization), zero where the vertex has
  // no area so degenerate verts fall through to the tangent fallback as before.
  Vector<Tensor3> field, scratch;
  field.resize(vcap);
  scratch.resize(vcap);
  for (int i = 0; i < vcap; i++) {
    for (int a = 0; a < 3; a++) {
      for (int b = 0; b < 3; b++) {
        field[i].m[a][b] = 0.0;
      }
    }
  }

  for (int v : m.v) {
    // Accumulate the shape operator over incident manifold edges (disk walk).
    double T[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    int e0 = m.v.e[v];
    if (e0 != ELEM_NONE) {
      int ec = e0;
      do {
        int c1 = m.e.c[ec];
        if (c1 != ELEM_NONE) {
          int c2 = m.c.radial_next[c1];
          // Manifold edge: exactly two corners around it.
          if (c2 != c1 && m.c.radial_next[c2] == c1) {
            int f1 = m.l.f[m.c.l[c1]];
            int f2 = m.l.f[m.c.l[c2]];
            float3 n1 = fn[f1], n2 = fn[f2];
            float d = n1.dot(n2);
            d = d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d);
            float theta = std::acos(d);
            // Winding-independent sign: convex (f2 centroid behind n1) => +.
            float signC = n1.dot(fcent[f2] - fcent[f1]) < 0.0f ? 1.0f : -1.0f;
            float beta = signC * theta;

            float3 evec = m.v.co[m.e.vs[ec][1]] - m.v.co[m.e.vs[ec][0]];
            float elen = evec.length();
            if (elen > 1e-20f) {
              float3 eu = evec * (1.0f / elen);
              double w = double(beta) * double(elen) * 0.5;
              for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                  T[i][j] += w * double(eu[i]) * double(eu[j]);
                }
              }
            }
          }
        }
        int side = m.e.vs[ec][0] == v ? 0 : 1;
        ec = mesh::diskEdge(m.e.disk[ec][side * 2 + 1]);
      } while (ec != e0);
    }

    if (varea[v] > 1e-20f) {
      for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
          field[v].m[i][j] = T[i][j] / double(varea[v]);
        }
      }
    }
  }

  // Tier 2a: Jacobi-diffuse the tensor field over the one-ring to denoise it.
  // Uniform neighbour weights (w_vn = 1; documented choice — mass / cotangent
  // weights over-smooth or re-inject noise). Averaging is linear so tensors stay
  // symmetric; they are NOT PSD (beta is a signed dihedral), but the solver
  // below only needs symmetry. iters = 0 leaves the field untouched (today).
  const int iters = params.smooth_iters > 0 ? params.smooth_iters : 0;
  const double lambda = double(params.smooth_lambda);
  for (int it = 0; it < iters; it++) {
    for (int v : m.v) {
      double acc[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
      int cnt = 0;
      int e0 = m.v.e[v];
      if (e0 != ELEM_NONE) {
        int ec = e0;
        do {
          int vn2 = m.e.vs[ec][0] == v ? m.e.vs[ec][1] : m.e.vs[ec][0];
          for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
              acc[i][j] += field[vn2].m[i][j];
            }
          }
          cnt++;
          int side = m.e.vs[ec][0] == v ? 0 : 1;
          ec = mesh::diskEdge(m.e.disk[ec][side * 2 + 1]);
        } while (ec != e0);
      }
      if (cnt > 0) {
        double inv = 1.0 / double(cnt);
        for (int i = 0; i < 3; i++) {
          for (int j = 0; j < 3; j++) {
            scratch[v].m[i][j] =
                (1.0 - lambda) * field[v].m[i][j] + lambda * acc[i][j] * inv;
          }
        }
      } else {
        scratch[v] = field[v];
      }
    }
    for (int v : m.v) {
      field[v] = scratch[v];
    }
  }

  for (int v : m.v) {
    float3 vn = m.v.no[v];
    float3 dmin(0.0f, 0.0f, 0.0f), dmax(0.0f, 0.0f, 0.0f);
    float kmin = 0.0f, kmax = 0.0f;

    if (varea[v] > 1e-20f) {
      Eigen::Matrix3d M;
      for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
          M(i, j) = field[v].m[i][j];
        }
      }
      Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(M);
      Eigen::Vector3d ev = es.eigenvalues();    // ascending
      Eigen::Matrix3d V = es.eigenvectors();    // columns

      // The eigenvector nearest the surface normal is the (discarded) normal
      // direction; the other two span the tangent plane.
      int idxN = 0;
      double best = -1.0;
      for (int k = 0; k < 3; k++) {
        double dn = std::fabs(V(0, k) * vn[0] + V(1, k) * vn[1] + V(2, k) * vn[2]);
        if (dn > best) {
          best = dn;
          idxN = k;
        }
      }
      int small = -1, large = -1;
      for (int k = 0; k < 3; k++) {
        if (k == idxN) {
          continue;
        }
        if (small < 0) {
          small = k;
        } else {
          large = k;
        }
      }
      if (ev[small] > ev[large]) {
        int tmp = small;
        small = large;
        large = tmp;
      }

      kmin = float(ev[small]);
      kmax = float(ev[large]);
      // THE SWAP: kmax's direction is the small-eigenvalue eigenvector.
      dmax = float3(float(V(0, small)), float(V(1, small)), float(V(2, small)));
      dmin = float3(float(V(0, large)), float(V(1, large)), float(V(2, large)));
    }

    // Project onto the tangent plane + normalize; fall back to an arbitrary
    // tangent frame when the estimate is degenerate (flat / isolated).
    auto tangentize = [&](float3 d) -> float3 {
      d = d - vn * vn.dot(d);
      float l = d.length();
      if (l > 1e-12f) {
        return d * (1.0f / l);
      }
      float3 t = std::fabs(vn[0]) < 0.9f ? float3(1, 0, 0) : float3(0, 1, 0);
      t = t - vn * vn.dot(t);
      float tl = t.length();
      return tl > 1e-12f ? t * (1.0f / tl) : float3(1, 0, 0);
    };
    dmax = tangentize(dmax);
    dmin = tangentize(dmin);
    // Re-orthogonalize dmin against dmax within the tangent plane.
    float3 o = dmin - dmax * dmax.dot(dmin);
    float ol = o.length();
    if (ol > 1e-12f) {
      dmin = o * (1.0f / ol);
    } else {
      float3 cr = vn.cross(dmax);
      float cl = cr.length();
      dmin = cl > 1e-12f ? cr * (1.0f / cl) : dmin;
    }

    kmin_dir[v] = dmin;
    kmax_dir[v] = dmax;
    kval[v] = float2(kmin, kmax);
  }
}

} // namespace sculptcore::remesh
