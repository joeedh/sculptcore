#include "remesh/field/singularity_adjust.h"
#include "remesh/field/cross_field.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"

#include "litestl/math/vector.h"
#include "litestl/util/set.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include "eigen/include/eigen5/Eigen/Sparse"
#include "eigen/include/eigen5/Eigen/SparseCholesky"

#include <cmath>
#include <complex>
#include <vector>

namespace sculptcore::remesh {

using litestl::math::float3;
using litestl::util::Vector;
using sculptcore::mesh::AttrFlag;
using sculptcore::mesh::AttrType;
using sculptcore::mesh::BuiltinAttr;
using sculptcore::mesh::Mesh;

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double HALF_PI = PI * 0.5;
constexpr double TWO_PI = PI * 2.0;

// Reduce an angle to the cross field's fundamental domain (−π/4, π/4].
inline double reduceQuarter(double x)
{
  return x - HALF_PI * std::round(x / HALF_PI);
}

// True for an interior, manifold (exactly two faces) edge; fills fa/fb.
inline bool interiorEdge(Mesh &m, int e, int &fa, int &fb)
{
  int c1 = m.e.c[e];
  if (c1 == ELEM_NONE) {
    return false;
  }
  int c2 = m.c.radial_next[c1];
  if (c2 == c1 || m.c.radial_next[c2] != c1) {
    return false;
  }
  fa = m.l.f[m.c.l[c1]];
  fb = m.l.f[m.c.l[c2]];
  return fa != fb;
}
} // namespace

double crossFieldCurl(Mesh &m)
{
  m.recalc_normals();
  const int fcap = int(m.f.capacity());

  BuiltinAttr<float, ".remesh.f.theta"> theta;
  theta.ensure(m.f.attrs);

  Vector<float3> FX, FY, FN;
  FX.resize(fcap);
  FY.resize(fcap);
  FN.resize(fcap);
  for (int f : m.f) {
    faceFrame(m, f, FX[f], FY[f], FN[f]);
  }
  auto edgeAngle = [&](int e, int f) -> double {
    float3 d = m.v.co[m.e.vs[e][1]] - m.v.co[m.e.vs[e][0]];
    return double(std::atan2(d.dot(FY[f]), d.dot(FX[f])));
  };

  double sum = 0.0;
  for (int e : m.e) {
    int fa, fb;
    if (!interiorEdge(m, e, fa, fb)) {
      continue;
    }
    double rho = edgeAngle(e, fb) - edgeAngle(e, fa);
    double r = reduceQuarter(double(theta[fb]) - double(theta[fa]) - rho);
    sum += r * r;
  }
  return std::sqrt(sum);
}

SingularityPairStats findSingularityPairs(Mesh &m, int max_hops,
                                          litestl::util::Vector<int> *pair_verts)
{
  SingularityPairStats stats;

  BuiltinAttr<short, ".remesh.v.pole_index"> pole;
  pole.ensure(m.v.attrs);

  if (pair_verts) {
    pair_verts->clear();
  }

  auto eachNeighbor = [&m](int v, auto &&fn) {
    int e0 = m.v.e[v];
    if (e0 == ELEM_NONE) {
      return;
    }
    int ec = e0;
    do {
      int side = m.e.vs[ec][0] == v ? 0 : 1;
      fn(m.e.vs[ec][side ^ 1]);
      ec = m.e.disk[ec][side * 2 + 1];
    } while (ec != e0);
  };

  litestl::util::Set<int> in_pair;
  for (int v : m.v) {
    if (pole[v] == 0) {
      continue;
    }
    stats.num_singularities++;

    // BFS ring expansion to max_hops; pair each pole with higher-index
    // opposite-sign poles in the ring so every pair is counted once.
    litestl::util::Set<int> seen;
    Vector<int> ring;
    seen.add(v);
    eachNeighbor(v, [&](int vn) {
      if (seen.add(vn)) {
        ring.append(vn);
      }
    });
    int frontier = 0;
    for (int hop = 1; hop < max_hops; hop++) {
      int end = int(ring.size());
      for (int i = frontier; i < end; i++) {
        eachNeighbor(ring[i], [&](int vn) {
          if (seen.add(vn)) {
            ring.append(vn);
          }
        });
      }
      frontier = end;
    }
    for (int w : ring) {
      if (w > v && int(pole[v]) * int(pole[w]) < 0) {
        stats.close_pairs++;
        if (in_pair.add(v) && pair_verts) {
          pair_verts->append(v);
        }
        if (in_pair.add(w) && pair_verts) {
          pair_verts->append(w);
        }
      }
    }
  }
  stats.clutter_verts = int(in_pair.size());
  return stats;
}

SingularityAdjustStats adjustSingularities(Mesh &m,
                                           const SingularityAdjustParams &params)
{
  using cd = std::complex<double>;
  SingularityAdjustStats stats;

  m.recalc_normals();

  // Need an M2 field to refine; compute one with defaults if absent.
  if (!m.f.attrs.has(AttrType::FLOAT, litestl::util::string(".remesh.f.theta"))) {
    CrossFieldParams cfp;
    computeCrossField(m, cfp);
  }
  stats.curl_before = crossFieldCurl(m);

  const int fcap = int(m.f.capacity());

  Vector<int> f2i;
  f2i.resize(fcap);
  for (int i = 0; i < fcap; i++) {
    f2i[i] = -1;
  }
  int N = 0;
  for (int f : m.f) {
    f2i[f] = N++;
  }
  stats.num_faces = N;
  if (N == 0) {
    stats.curl_after = stats.curl_before;
    return stats;
  }

  Vector<float3> FX, FY, FN;
  FX.resize(fcap);
  FY.resize(fcap);
  FN.resize(fcap);
  for (int f : m.f) {
    faceFrame(m, f, FX[f], FY[f], FN[f]);
  }
  auto edgeAngle = [&](int e, int f) -> double {
    float3 d = m.v.co[m.e.vs[e][1]] - m.v.co[m.e.vs[e][0]];
    return double(std::atan2(d.dot(FY[f]), d.dot(FX[f])));
  };

  BuiltinAttr<float, ".remesh.f.theta", AttrFlag::TEMP> theta;
  theta.ensure(m.f.attrs);

  // Smoothest-phase Poisson solve with the M2 period jumps held fixed: minimize
  // Σ_e (θ_b − θ_a − c_e)², c_e = ρ_e + (π/2)·p_e. Matrix is the face dual-graph
  // Laplacian (constant); a tiny Tikhonov shift fixes its constant null space.
  std::vector<Eigen::Triplet<double>> trips;
  Eigen::VectorXd d = Eigen::VectorXd::Zero(N);
  for (int e : m.e) {
    int fa, fb;
    if (!interiorEdge(m, e, fa, fb)) {
      continue;
    }
    int ia = f2i[fa], ib = f2i[fb];
    double rho = edgeAngle(e, fb) - edgeAngle(e, fa);
    double raw = double(theta[fb]) - double(theta[fa]) - rho;
    double p = std::round(raw / HALF_PI);
    double c_e = rho + HALF_PI * p;

    trips.emplace_back(ia, ia, 1.0);
    trips.emplace_back(ib, ib, 1.0);
    trips.emplace_back(ia, ib, -1.0);
    trips.emplace_back(ib, ia, -1.0);
    d[ia] -= c_e;
    d[ib] += c_e;
  }
  const double eps = double(params.gauge_eps);
  for (int i = 0; i < N; i++) {
    trips.emplace_back(i, i, eps);
  }

  Eigen::SparseMatrix<double> L(N, N);
  L.setFromTriplets(trips.begin(), trips.end());
  L.makeCompressed();

  Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver;
  solver.compute(L);
  Eigen::VectorXd x = Eigen::VectorXd::Zero(N);
  if (solver.info() == Eigen::Success) {
    x = solver.solve(d);
  }

  // Store the refined phase, reduced to (−π/4, π/4]; unit complex per face.
  Vector<cd> cf;
  cf.resize(fcap);
  for (int f : m.f) {
    double th = reduceQuarter(x[f2i[f]]);
    theta[f] = float(th);
    cf[f] = std::polar(1.0, 4.0 * th);
  }

  // Recompute period jumps (oriented face(e.c) → radial), stored 0..3.
  BuiltinAttr<short, ".remesh.e.period", AttrFlag::TEMP> period;
  period.ensure(m.e.attrs);
  for (int e : m.e) {
    short pjump = 0;
    int fa, fb;
    if (interiorEdge(m, e, fa, fb)) {
      double rho = edgeAngle(e, fb) - edgeAngle(e, fa);
      long pj = std::lround((double(theta[fb]) - double(theta[fa]) - rho) / HALF_PI);
      pjump = short(((pj % 4) + 4) % 4);
    }
    period[e] = pjump;
  }

  // Optional user pins: their index is frozen across the adjustment.
  bool have_pins =
      m.v.attrs.has(AttrType::BOOL, litestl::util::string(".remesh.v.pole_pinned"));
  BuiltinAttr<bool, ".remesh.v.pole_pinned"> pinned;
  if (have_pins) {
    pinned.ensure(m.v.attrs);
  }

  BuiltinAttr<short, ".remesh.v.pole_index", AttrFlag::TEMP> pole;
  pole.ensure(m.v.attrs);

  // Cross the corner's edge to the adjacent face's corner at the same vertex.
  auto nextFaceCorner = [&](int cc) -> int {
    int v = m.c.v[cc];
    int cr = m.c.radial_next[cc];
    if (cr == cc) {
      return ELEM_NONE;
    }
    if (m.c.v[cr] == v) {
      return cr;
    }
    int cn = m.c.next[cr];
    if (m.c.v[cn] == v) {
      return cn;
    }
    int cp = m.c.prev[cr];
    if (m.c.v[cp] == v) {
      return cp;
    }
    return ELEM_NONE;
  };

  long signed_sum = 0;
  int num_sing = 0;
  for (int v : m.v) {
    if (have_pins && pinned[v]) {
      // Frozen: keep the prior index, still count it toward the totals.
      long k = long(pole[v]);
      signed_sum += k;
      if (k != 0) {
        num_sing++;
      }
      continue;
    }

    int e0 = m.v.e[v];
    if (e0 == ELEM_NONE) {
      pole[v] = 0;
      continue;
    }
    int cstart = m.e.c[e0];
    if (cstart == ELEM_NONE) {
      pole[v] = 0;
      continue;
    }
    if (m.c.v[cstart] != v) {
      if (m.c.v[m.c.next[cstart]] == v) {
        cstart = m.c.next[cstart];
      } else if (m.c.v[m.c.prev[cstart]] == v) {
        cstart = m.c.prev[cstart];
      } else {
        pole[v] = 0;
        continue;
      }
    }

    double sum_delta = 0.0;
    double angle_sum = 0.0;
    bool boundary = false;
    int guard = 0;
    int cc = cstart;
    do {
      int fa = m.l.f[m.c.l[cc]];
      int e = m.c.e[cc];

      float3 vp = m.v.co[v];
      float3 a = m.v.co[m.c.v[m.c.next[cc]]] - vp;
      float3 bb = m.v.co[m.c.v[m.c.prev[cc]]] - vp;
      double al = a.length(), bl = bb.length();
      if (al > 1e-20 && bl > 1e-20) {
        double cs = double(a.dot(bb)) / (al * bl);
        cs = cs < -1.0 ? -1.0 : (cs > 1.0 ? 1.0 : cs);
        angle_sum += std::acos(cs);
      }

      int cnext = nextFaceCorner(cc);
      if (cnext == ELEM_NONE) {
        boundary = true;
        break;
      }
      int fb = m.l.f[m.c.l[cnext]];
      double rho = edgeAngle(e, fb) - edgeAngle(e, fa);
      cd resid = cf[fb] * std::conj(cf[fa] * std::polar(1.0, 4.0 * rho));
      sum_delta += std::arg(resid);
      cc = cnext;
      if (++guard > 100000) {
        boundary = true;
        break;
      }
    } while (cc != cstart);

    if (boundary) {
      pole[v] = 0;
      continue;
    }

    double theta_defect = TWO_PI - angle_sum;
    long k = std::lround((4.0 * theta_defect - sum_delta) / TWO_PI);
    pole[v] = short(k);
    signed_sum += k;
    if (k != 0) {
      num_sing++;
    }
  }
  stats.index_sum = int(signed_sum);
  stats.num_singularities = num_sing;

  stats.curl_after = crossFieldCurl(m);
  return stats;
}

} // namespace sculptcore::remesh
