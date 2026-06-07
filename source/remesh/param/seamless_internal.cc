#include "remesh/param/seamless_internal.h"
#include "remesh/field/cross_field.h"
#include "remesh/param/cut_graph.h"
#include "remesh/param/seamless_param.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"

#include "litestl/math/vector.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

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
constexpr double PI = 3.14159265358979323846;
constexpr double HALF_PI = PI * 0.5;
} // namespace

bool buildSeamlessSystem(Mesh &m, const SeamlessParamParams &params, SeamlessSystem &out)
{
  m.recalc_normals();

  // Need an M2/M3 cross field to follow; compute defaults if absent.
  if (!m.f.attrs.has(AttrType::FLOAT, litestl::util::string(".remesh.f.theta"))) {
    CrossFieldParams cfp;
    computeCrossField(m, cfp);
  }

  // Open all holonomy: cut graph tags .remesh.e.is_cut so the non-cut face
  // adjacency is a tree.
  buildCutGraph(m);

  const int fcap = int(m.f.capacity());
  const int ecap = int(m.e.capacity());
  const int ccap = int(m.c.capacity());
  if (fcap == 0 || ccap == 0) {
    return false;
  }

  BuiltinAttr<float, ".remesh.f.theta"> theta;
  theta.ensure(m.f.attrs);
  BuiltinAttr<bool, ".remesh.e.is_cut", AttrFlag::TEMP> is_cut;
  is_cut.ensure(m.e.attrs);

  // Per-face tangent frames (must match the field's frames exactly).
  out.FX.resize(fcap);
  out.FY.resize(fcap);
  out.FN.resize(fcap);
  for (int f : m.f) {
    faceFrame(m, f, out.FX[f], out.FY[f], out.FN[f]);
  }
  auto edgeAngle = [&](int e, int f) -> double {
    float3 d = m.v.co[m.e.vs[e][1]] - m.v.co[m.e.vs[e][0]];
    return double(std::atan2(d.dot(out.FY[f]), d.dot(out.FX[f])));
  };

  // Period jump per interior edge, oriented face(e.c)=fa -> fb=radial.
  out.periodEC.resize(ecap);
  for (int i = 0; i < ecap; i++) {
    out.periodEC[i] = 0;
  }
  BuiltinAttr<short, ".remesh.e.period", AttrFlag::TEMP> period;
  period.ensure(m.e.attrs);
  for (int e : m.e) {
    int fa, fb;
    if (!interiorEdge(m, e, fa, fb)) {
      period[e] = 0;
      continue;
    }
    double rho = edgeAngle(e, fb) - edgeAngle(e, fa);
    long pj = std::lround((double(theta[fb]) - double(theta[fa]) - rho) / HALF_PI);
    int p = int(((pj % 4) + 4) % 4);
    out.periodEC[e] = p;
    period[e] = short(p);
  }

  // Gauge: BFS the non-cut dual tree, accumulating per-face integer rotations R_f
  // so the cross axes line up. Across a non-cut edge fa->fb, R_fb = R_fa - period.
  out.gauge.resize(fcap);
  for (int i = 0; i < fcap; i++) {
    out.gauge[i] = ELEM_NONE;
  }
  Vector<int> stack;
  for (int froot : m.f) {
    if (out.gauge[froot] != ELEM_NONE) {
      continue;
    }
    out.gauge[froot] = 0;
    stack.clear();
    stack.append(froot);
    while (stack.size() > 0) {
      int f = stack.pop_back();
      int c0 = m.l.c[m.f.l[f]], cc = c0;
      do {
        int e = m.c.e[cc];
        int fa, fb;
        if (interiorEdge(m, e, fa, fb) && !is_cut[e]) {
          int nb = (fa == f) ? fb : fa;
          if (out.gauge[nb] == ELEM_NONE) {
            int jump = (f == fa) ? out.periodEC[e] : -out.periodEC[e];
            out.gauge[nb] = (((out.gauge[f] - jump) % 4) + 4) % 4;
            stack.append(nb);
          }
        }
        cc = m.c.next[cc];
      } while (cc != c0);
    }
  }

  // Union corners across non-cut edges: the two corners on either side at the
  // same vertex share one gauged (u, v) variable. Cut edges leave them split.
  UnionFind ufc;
  ufc.init(ccap);
  for (int e : m.e) {
    int fa, fb;
    if (!interiorEdge(m, e, fa, fb) || is_cut[e]) {
      continue;
    }
    int c1 = m.e.c[e];
    int c2 = m.c.radial_next[c1];
    ufc.unite(c1, m.c.next[c2]); // at vertex v(c1)
    ufc.unite(m.c.next[c1], c2); // at vertex v(next(c1))
  }

  // Compact union-find roots of live corners into a dense class index.
  out.cornerClass.resize(ccap);
  for (int i = 0; i < ccap; i++) {
    out.cornerClass[i] = -1;
  }
  Vector<int> rootToClass;
  rootToClass.resize(ccap);
  for (int i = 0; i < ccap; i++) {
    rootToClass[i] = -1;
  }
  int M = 0;
  int num_corners = 0;
  for (int f : m.f) {
    int c0 = m.l.c[m.f.l[f]], cc = c0;
    do {
      int r = ufc.find(cc);
      if (rootToClass[r] == -1) {
        rootToClass[r] = M++;
      }
      out.cornerClass[cc] = rootToClass[r];
      num_corners++;
      cc = m.c.next[cc];
    } while (cc != c0);
  }
  out.M = M;
  out.num_corners = num_corners;
  if (M == 0) {
    return false;
  }

  // Optional anisotropic density: edge length scales as 1/sqrt(density).
  bool have_density =
      params.use_density &&
      m.v.attrs.has(AttrType::FLOAT, litestl::util::string(".remesh.v.density"));
  BuiltinAttr<float, ".remesh.v.density"> density;
  if (have_density) {
    density.ensure(m.v.attrs);
  }
  const double inv_len =
      params.target_edge_length > 1e-12f ? 1.0 / double(params.target_edge_length) : 1.0;

  // Assemble one cotan-style Laplacian L and two decoupled right-hand sides
  // (target gradients tgt_u, tgt_v) from the per-triangle FEM gradient operator.
  out.baseTrips.clear();
  out.bu = Eigen::VectorXd::Zero(M);
  out.bv = Eigen::VectorXd::Zero(M);

  // Connected components of the cut mesh (one per topological disk).
  UnionFind ufcomp;
  ufcomp.init(M);

  Vector<int> cs;     // corner ids of the current face
  Vector<float2> loc; // their 2D coords in the face frame
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

    float3 X = out.FX[f], Y = out.FY[f];
    float3 p0 = m.v.co[m.c.v[cs[0]]];
    for (int i = 0; i < n; i++) {
      float3 d = m.v.co[m.c.v[cs[i]]] - p0;
      loc.append(float2(d.dot(X), d.dot(Y)));
    }

    // Gauged target: u-gradient along the cross axis, v perpendicular.
    double alpha = double(theta[f]) + double(out.gauge[f]) * HALF_PI;
    double mag = inv_len;
    if (have_density) {
      double davg = 0.0;
      for (int i = 0; i < n; i++) {
        davg += double(density[m.c.v[cs[i]]]);
      }
      davg = davg / double(n);
      mag = inv_len * std::sqrt(davg > 1e-12 ? davg : 1e-12);
    }
    float2 tgt_u(float(mag * std::cos(alpha)), float(mag * std::sin(alpha)));
    float2 tgt_v(float(-mag * std::sin(alpha)), float(mag * std::cos(alpha)));

    int base_cls = out.cornerClass[cs[0]];
    for (int i = 1; i < n; i++) {
      ufcomp.unite(base_cls, out.cornerClass[cs[i]]);
    }

    // Fan-triangulate (cs[0], cs[t], cs[t+1]).
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
      for (int a = 0; a < 3; a++) {
        int ca = out.cornerClass[cs[idx[a]]];
        for (int b = 0; b < 3; b++) {
          int cb = out.cornerClass[cs[idx[b]]];
          double k = area * (Gx[a] * Gx[b] + Gy[a] * Gy[b]);
          out.baseTrips.emplace_back(ca, cb, k);
        }
        out.bu[ca] += area * (Gx[a] * double(tgt_u[0]) + Gy[a] * double(tgt_u[1]));
        out.bv[ca] += area * (Gx[a] * double(tgt_v[0]) + Gy[a] * double(tgt_v[1]));
      }
    }
  }

  // One pin class per connected component (first-seen wins; deterministic).
  Vector<char> comp_pinned;
  comp_pinned.resize(M);
  for (int i = 0; i < M; i++) {
    comp_pinned[i] = 0;
  }
  out.pinClass.clear();
  for (int c = 0; c < M; c++) {
    int r = ufcomp.find(c);
    if (!comp_pinned[r]) {
      comp_pinned[r] = 1;
      out.pinClass.append(c);
    }
  }

  return true;
}

} // namespace sculptcore::remesh
