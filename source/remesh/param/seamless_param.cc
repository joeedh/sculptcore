#include "remesh/param/seamless_param.h"
#include "remesh/param/seamless_internal.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"

#include "litestl/math/vector.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include "eigen/include/eigen3/Eigen/Sparse"
#include "eigen/include/eigen3/Eigen/SparseCholesky"

#include <cmath>
#include <vector>

namespace sculptcore::remesh {

using litestl::math::float2;
using litestl::math::float3;
using litestl::util::Vector;
using sculptcore::mesh::AttrFlag;
using sculptcore::mesh::AttrType;
using sculptcore::mesh::BuiltinAttr;
using sculptcore::mesh::Mesh;

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double HALF_PI = PI * 0.5;

inline double reduceQuarter(double x)
{
  return x - HALF_PI * std::round(x / HALF_PI);
}

// CCW rotation of a 2D vector by angle a.
inline float2 rot(double a, float2 p)
{
  double c = std::cos(a), s = std::sin(a);
  return float2(float(c * p[0] - s * p[1]), float(s * p[0] + c * p[1]));
}
} // namespace

SeamlessParamStats computeSeamlessParam(Mesh &m, const SeamlessParamParams &params)
{
  SeamlessParamStats stats;

  SeamlessSystem sys;
  if (!buildSeamlessSystem(m, params, sys)) {
    return stats;
  }
  const int M = sys.M;
  stats.num_corners = sys.num_corners;
  stats.num_classes = M;

  BuiltinAttr<float, ".remesh.f.theta"> theta;
  theta.ensure(m.f.attrs);
  BuiltinAttr<bool, ".remesh.e.is_cut", AttrFlag::TEMP> is_cut;
  is_cut.ensure(m.e.attrs);

  // L = base FEM stiffness + per-component pin (kills the constant null space) +
  // a Tikhonov shift for conditioning. u and v decouple onto the same L.
  std::vector<Eigen::Triplet<double>> trips = sys.baseTrips;
  for (int i = 0; i < int(sys.pinClass.size()); i++) {
    int c = sys.pinClass[i];
    trips.emplace_back(c, c, 1.0e6);
  }
  const double eps = double(params.gauge_eps);
  for (int c = 0; c < M; c++) {
    trips.emplace_back(c, c, eps);
  }

  Eigen::SparseMatrix<double> L(M, M);
  L.setFromTriplets(trips.begin(), trips.end());
  L.makeCompressed();

  Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver;
  solver.compute(L);
  Eigen::VectorXd U = Eigen::VectorXd::Zero(M);
  Eigen::VectorXd V = Eigen::VectorXd::Zero(M);
  if (solver.info() == Eigen::Success) {
    U = solver.solve(sys.bu);
    V = solver.solve(sys.bv);
    stats.solved = (solver.info() == Eigen::Success);
  }

  // Un-gauge to a per-corner (u, v): rotate the gauged class value back by
  // -R_f*90 so non-cut transitions become pure rotations with zero translation.
  BuiltinAttr<float2, ".remesh.c.uv", AttrFlag::TEMP> uv;
  uv.ensure(m.c.attrs);
  BuiltinAttr<short, ".remesh.f.gauge_rot", AttrFlag::TEMP> gauge_rot;
  gauge_rot.ensure(m.f.attrs);
  for (int f : m.f) {
    int Rf = sys.gauge[f];
    gauge_rot[f] = short(Rf);
    double ang = -double(Rf) * HALF_PI;
    int c0 = m.l.c[m.f.l[f]], cc = c0;
    do {
      int cl = sys.cornerClass[cc];
      float2 g = float2(float(U[cl]), float(V[cl]));
      uv[cc] = rot(ang, g);
      cc = m.c.next[cc];
    } while (cc != c0);
  }

  // Per-edge seam translation t = uv(cb) - R(period*90)*uv(ca). Zero on non-cut
  // edges by construction; the cut-edge values are what M5 quantizes.
  BuiltinAttr<float2, ".remesh.e.translation", AttrFlag::TEMP> trans;
  trans.ensure(m.e.attrs);
  int num_cut = 0;
  double max_seam_t = 0.0;
  for (int e : m.e) {
    trans[e] = float2(0.0f, 0.0f);
    int fa, fb;
    if (!interiorEdge(m, e, fa, fb)) {
      continue;
    }
    int c1 = m.e.c[e];            // corner in fa at vertex v(c1)
    int c2 = m.c.radial_next[c1];
    int cb = m.c.next[c2];        // corner in fb at the same vertex
    float2 uva = uv[c1];
    float2 uvb = uv[cb];
    float2 ra = rot(double(sys.periodEC[e]) * HALF_PI, uva);
    float2 t(uvb[0] - ra[0], uvb[1] - ra[1]);
    trans[e] = t;
    if (is_cut[e]) {
      num_cut++;
    } else {
      double tl = std::sqrt(double(t[0]) * double(t[0]) + double(t[1]) * double(t[1]));
      max_seam_t = std::fmax(max_seam_t, tl);
    }
  }
  stats.num_cut_edges = num_cut;
  stats.max_seam_translation = max_seam_t;

  // Diagnostics: per-face un-gauged gradient (area-weighted over the fan). The
  // u-gradient should be parallel to the cross axis (angle == theta mod 90), and
  // det(grad u, grad v) should stay positive away from singularities.
  Vector<int> cs;
  Vector<float2> loc;
  int num_faces = 0;
  double grad_err = 0.0;
  double min_jac = 0.0;
  bool first_jac = true;
  for (int f : m.f) {
    cs.clear();
    loc.clear();
    int c0 = m.l.c[m.f.l[f]], cc = c0;
    do {
      cs.append(cc);
      cc = m.c.next[cc];
    } while (cc != c0);
    int n = int(cs.size());
    if (n < 3) {
      continue;
    }
    num_faces++;

    float3 X = sys.FX[f], Y = sys.FY[f];
    float3 p0 = m.v.co[m.c.v[cs[0]]];
    for (int i = 0; i < n; i++) {
      float3 d = m.v.co[m.c.v[cs[i]]] - p0;
      loc.append(float2(d.dot(X), d.dot(Y)));
    }

    double gux = 0, guy = 0, gvx = 0, gvy = 0, totA = 0;
    for (int t = 1; t + 1 < n; t++) {
      int idx[3] = {0, t, t + 1};
      float2 q0 = loc[0], q1 = loc[t], q2 = loc[t + 1];
      double r1x = q1[0] - q0[0], r1y = q1[1] - q0[1];
      double r2x = q2[0] - q0[0], r2y = q2[1] - q0[1];
      double det = r1x * r2y - r1y * r2x;
      if (std::fabs(det) < 1e-20) {
        continue;
      }
      double area = 0.5 * std::fabs(det);
      double Gx[3] = {(r1y - r2y) / det, r2y / det, -r1y / det};
      double Gy[3] = {(r2x - r1x) / det, -r2x / det, r1x / det};
      double dux = 0, duy = 0, dvx = 0, dvy = 0;
      for (int a = 0; a < 3; a++) {
        float2 c = uv[cs[idx[a]]];
        dux += Gx[a] * double(c[0]);
        duy += Gy[a] * double(c[0]);
        dvx += Gx[a] * double(c[1]);
        dvy += Gy[a] * double(c[1]);
      }
      gux += area * dux;
      guy += area * duy;
      gvx += area * dvx;
      gvy += area * dvy;
      totA += area;
    }
    if (totA <= 1e-20) {
      continue;
    }
    gux /= totA;
    guy /= totA;
    gvx /= totA;
    gvy /= totA;

    double ang = std::atan2(guy, gux);
    double err = std::fabs(reduceQuarter(ang - double(theta[f])));
    grad_err = std::fmax(grad_err, err);
    double jac = gux * gvy - guy * gvx;
    if (first_jac || jac < min_jac) {
      min_jac = jac;
      first_jac = false;
    }
  }
  stats.num_faces = num_faces;
  stats.grad_angle_err = grad_err;
  stats.min_jacobian = first_jac ? 0.0 : min_jac;

  return stats;
}

} // namespace sculptcore::remesh
