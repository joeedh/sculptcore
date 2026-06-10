#include "remesh/field/cross_field.h"
#include "remesh/field/constraints.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "mesh/utils/mesh_validate.h" // faceNewellNormal

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include "eigen/include/eigen5/Eigen/Sparse"
#include "eigen/include/eigen5/Eigen/SparseCholesky"

#include <cmath>
#include <complex>
#include <vector>

namespace sculptcore::remesh {

using litestl::math::float2;
using litestl::math::float3;
using litestl::util::Vector;
using sculptcore::mesh::AttrFlag;
using sculptcore::mesh::BuiltinAttr;
using sculptcore::mesh::Mesh;

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double HALF_PI = PI * 0.5;
} // namespace

void faceFrame(Mesh &m, int f, float3 &X, float3 &Y, float3 &N)
{
  float3 nn = mesh::faceNewellNormal(m, f);
  float nl = nn.length();
  N = nl > 1e-20f ? nn * (1.0f / nl) : float3(0.0f, 0.0f, 1.0f);

  int c0 = m.l.c[m.f.l[f]];
  int c1 = m.c.next[c0];
  float3 d = m.v.co[m.c.v[c1]] - m.v.co[m.c.v[c0]];
  d = d - N * N.dot(d);
  float dl = d.length();
  if (dl > 1e-12f) {
    X = d * (1.0f / dl);
  } else {
    float3 t = std::fabs(N[0]) < 0.9f ? float3(1, 0, 0) : float3(0, 1, 0);
    t = t - N * N.dot(t);
    float tl = t.length();
    X = tl > 1e-12f ? t * (1.0f / tl) : float3(1, 0, 0);
  }
  Y = N.cross(X);
}

CrossFieldStats computeCrossField(Mesh &m, const CrossFieldParams &params)
{
  using cd = std::complex<double>;
  CrossFieldStats stats;

  m.recalc_normals();

  // Per-face constraints (computes curvature / feature tags as needed).
  Vector<FaceConstraint> cons;
  gatherConstraints(m, params, cons);

  const int fcap = int(m.f.capacity());

  // Compact face indexing (the freemap leaves gaps in face ids).
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
    return stats;
  }

  // Per-face tangent frames.
  Vector<float3> FX, FY, FN;
  FX.resize(fcap);
  FY.resize(fcap);
  FN.resize(fcap);
  for (int f : m.f) {
    faceFrame(m, f, FX[f], FY[f], FN[f]);
  }

  // Transport-angle helper: angle of the shared edge dir in a face's frame.
  auto edgeAngle = [&](int e, int f) -> double {
    float3 d = m.v.co[m.e.vs[e][1]] - m.v.co[m.e.vs[e][0]];
    return double(std::atan2(d.dot(FY[f]), d.dot(FX[f])));
  };

  // Assemble the complex Hermitian system: per interior edge a smoothness term
  // |c_a·exp(i4ρ) − c_b|², plus per-face data terms (hard pin / soft pull) and
  // a tiny Tikhonov shift to keep it SPD.
  std::vector<Eigen::Triplet<cd>> trips;
  const double wsmooth = 1.0;
  int num_interior_edges = 0;

  for (int e : m.e) {
    int c1 = m.e.c[e];
    if (c1 == ELEM_NONE) {
      continue;
    }
    int c2 = m.c.radial_next[c1];
    if (c2 == c1 || m.c.radial_next[c2] != c1) {
      continue; // boundary or non-manifold
    }
    int fa = m.l.f[m.c.l[c1]];
    int fb = m.l.f[m.c.l[c2]];
    if (fa == fb) {
      continue;
    }
    int ia = f2i[fa], ib = f2i[fb];
    double rho = edgeAngle(e, fb) - edgeAngle(e, fa);
    cd t = std::polar(1.0, 4.0 * rho); // exp(i·4·ρ)

    trips.emplace_back(ia, ia, cd(wsmooth, 0.0));
    trips.emplace_back(ib, ib, cd(wsmooth, 0.0));
    trips.emplace_back(ia, ib, -wsmooth * std::conj(t));
    trips.emplace_back(ib, ia, -wsmooth * t);
    num_interior_edges++;
  }

  Eigen::VectorXcd b = Eigen::VectorXcd::Zero(N);
  const double eps_tik = 1e-8;
  const double lambda_hard = 1.0e3;
  int num_hard = 0;
  double soft_total = 0.0;

  for (int f : m.f) {
    int i = f2i[f];
    trips.emplace_back(i, i, cd(eps_tik, 0.0));
    const FaceConstraint &fc = cons[f];
    if (fc.is_hard) {
      trips.emplace_back(i, i, cd(lambda_hard, 0.0));
      b[i] += lambda_hard * cd(fc.cx, fc.cy);
      num_hard++;
    } else if (fc.weight > 0.0f) {
      double w = double(fc.weight);
      trips.emplace_back(i, i, cd(w, 0.0));
      b[i] += w * cd(fc.cx, fc.cy);
      soft_total += w;
    }
  }

  Eigen::SparseMatrix<cd> A(N, N);
  A.setFromTriplets(trips.begin(), trips.end());
  A.makeCompressed();

  Eigen::SimplicialLDLT<Eigen::SparseMatrix<cd>> solver;
  solver.compute(A);

  Eigen::VectorXcd c(N);
  bool constrained = (num_hard > 0) || (soft_total > 1e-6);

  // The system is near-singular when constraints fade (smooth geometry: only the
  // 1e-8 Tikhonov shift holds it up), so a direct solve can break down into
  // NaN/zero — validate and fall back to the eigenvector path instead of letting
  // a dead field propagate NaN into every downstream consumer.
  auto usable = [&](const Eigen::VectorXcd &x) -> bool {
    double n2 = 0.0;
    for (int i = 0; i < N; i++) {
      double re = x[i].real(), im = x[i].imag();
      if (!std::isfinite(re) || !std::isfinite(im)) {
        return false;
      }
      n2 += re * re + im * im;
    }
    return n2 > 1e-60;
  };

  bool solved = false;
  if (constrained && solver.info() == Eigen::Success) {
    c = solver.solve(b);
    solved = usable(c);
  }
  if (!solved) {
    // No data term (e.g. an umbilic sphere): the smoothest non-trivial field is
    // the smallest-eigenvalue eigenvector of the smoothness operator. Recover it
    // by inverse power iteration on A (= smoothness + εI here).
    stats.solved_eigen = true;
    uint32_t s = params.seed ? params.seed : 1u;
    auto rnd = [&]() -> double {
      s = s * 1664525u + 1013904223u;
      return double((s >> 8) & 0xffffffu) / double(0x1000000) * 2.0 - 1.0;
    };
    Eigen::VectorXcd x(N);
    for (int i = 0; i < N; i++) {
      x[i] = cd(rnd(), rnd());
    }
    double nrm = x.norm();
    if (nrm > 0.0) {
      x /= nrm;
    }
    if (solver.info() == Eigen::Success) {
      // Iterate to Rayleigh-quotient convergence, not a fixed count: a fixed count
      // under-converges large systems, collapsing the field (a dense umbilic sphere
      // would otherwise fold everywhere and extract empty).
      double prev_rq = 0.0;
      for (int it = 0; it < 400; it++) {
        Eigen::VectorXcd y = solver.solve(x);
        double yn = y.norm();
        if (yn < 1e-20 || !std::isfinite(yn)) {
          break; // keep the last good iterate
        }
        x = y / yn;
        if (!usable(x)) {
          x = Eigen::VectorXcd::Zero(N);
          for (int i = 0; i < N; i++) {
            x[i] = cd(rnd(), rnd());
          }
          x /= x.norm();
          break;
        }
        double rq = x.dot(A * x).real();
        if (it > 2 && std::fabs(rq - prev_rq) <= 1e-11 * (1.0 + std::fabs(rq))) {
          break;
        }
        prev_rq = rq;
      }
    }
    c = x;
  }

  // Recover θ_f = arg(c_f)/4 ∈ (−π/4, π/4].
  BuiltinAttr<float, ".remesh.f.theta", AttrFlag::TEMP> theta;
  theta.ensure(m.f.attrs);
  Vector<float> th;
  th.resize(fcap);
  for (int f : m.f) {
    double ang = std::arg(c[f2i[f]]) / 4.0;
    th[f] = float(ang);
    theta[f] = float(ang);
  }

  // Per-edge period jump (oriented face(e.c) → radial), stored 0..3.
  BuiltinAttr<short, ".remesh.e.period", AttrFlag::TEMP> period;
  period.ensure(m.e.attrs);
  for (int e : m.e) {
    short p = 0;
    int c1 = m.e.c[e];
    if (c1 != ELEM_NONE) {
      int c2 = m.c.radial_next[c1];
      if (c2 != c1 && m.c.radial_next[c2] == c1) {
        int fa = m.l.f[m.c.l[c1]];
        int fb = m.l.f[m.c.l[c2]];
        if (fa != fb) {
          double rho = edgeAngle(e, fb) - edgeAngle(e, fa);
          double diff = double(th[fb]) - double(th[fa]) - rho;
          long pj = std::lround(diff / HALF_PI);
          p = short(((pj % 4) + 4) % 4);
        }
      }
    }
    period[e] = p;
  }

  // Singularity index per interior vertex (Knöppel et al. 2013): the geometric
  // angle defect Θ_v = 2π − Σ(interior angles) minus the field's residual turning
  // δ around the fan, in quarter-turn units k_v = round((4Θ_v − Σδ)/2π). The δ
  // sum is antisymmetric across edges so it cancels globally, leaving Σ k_v ==
  // 4χ (Poincaré–Hopf for the cross field); δ's sign (vs Θ) sets each
  // singularity's sign. Stored in .remesh.v.pole_index.
  BuiltinAttr<short, ".remesh.v.pole_index", AttrFlag::TEMP> pole;
  pole.ensure(m.v.attrs);

  // Cross the current corner's edge to the adjacent face's corner at the same
  // vertex; ELEM_NONE at an open/non-manifold edge.
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

  const double TWO_PI = 2.0 * PI;
  long signed_sum = 0;
  int num_sing = 0;

  for (int v : m.v) {
    int e0 = m.v.e[v];
    if (e0 == ELEM_NONE) {
      continue;
    }
    int cstart = m.e.c[e0];
    if (cstart == ELEM_NONE) {
      continue; // wire edge
    }
    if (m.c.v[cstart] != v) {
      if (m.c.v[m.c.next[cstart]] == v) {
        cstart = m.c.next[cstart];
      } else if (m.c.v[m.c.prev[cstart]] == v) {
        cstart = m.c.prev[cstart];
      } else {
        continue;
      }
    }

    double sum_delta = 0.0; // field residual turning around the fan
    double angle_sum = 0.0; // Σ interior face angles at v
    bool boundary = false;
    int guard = 0;
    int cc = cstart;
    do {
      int fa = m.l.f[m.c.l[cc]];
      int e = m.c.e[cc];

      // Interior angle of face fa at v (between its two edges meeting at v).
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
      cd resid = c[f2i[fb]] * std::conj(c[f2i[fa]] * std::polar(1.0, 4.0 * rho));
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

  return stats;
}

} // namespace sculptcore::remesh
