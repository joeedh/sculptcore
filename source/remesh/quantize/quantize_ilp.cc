#include "remesh/quantize/quantize_ilp.h"
#include "remesh/param/seamless_internal.h"
#include "remesh/param/seamless_param.h"
#include "remesh/quantize/t_mesh.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"

#include "litestl/math/vector.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include "eigen/include/eigen5/Eigen/Sparse"
#include "eigen/include/eigen5/Eigen/SparseCholesky"

#ifndef WASM
#include <eigen/include/eigen5/Eigen/CholmodSupport>
#endif

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace sculptcore::remesh {

using litestl::math::float2;
using litestl::math::float3;
using litestl::math::int2;
using litestl::util::Vector;
using sculptcore::mesh::AttrFlag;
using sculptcore::mesh::AttrType;
using sculptcore::mesh::BuiltinAttr;
using sculptcore::mesh::Mesh;

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double HALF_PI = PI * 0.5;

// A 2x2 matrix [[a, b], [c, d]] for the gauge/period 90-degree rotations.
struct M2 {
  double a, b, c, d;
};
inline M2 rotM(int k)
{
  k = ((k % 4) + 4) % 4;
  switch (k) {
  case 0:
    return {1, 0, 0, 1};
  case 1:
    return {0, -1, 1, 0};
  case 2:
    return {-1, 0, 0, -1};
  default:
    return {0, 1, -1, 0};
  }
}
inline M2 transp(const M2 &m)
{
  return {m.a, m.c, m.b, m.d};
}
inline M2 negM(const M2 &m)
{
  return {-m.a, -m.b, -m.c, -m.d};
}
inline M2 mul(const M2 &A, const M2 &B)
{
  return {A.a * B.a + A.b * B.c,
          A.a * B.b + A.b * B.d,
          A.c * B.a + A.d * B.c,
          A.c * B.b + A.d * B.d};
}
inline void mv(const M2 &A, double x, double y, double &rx, double &ry)
{
  rx = A.a * x + A.b * y;
  ry = A.c * x + A.d * y;
}

// CCW continuous rotation of a 2D vector by angle ang (for un-gauging).
inline float2 rotc(double ang, float2 p)
{
  double c = std::cos(ang), s = std::sin(ang);
  return float2(float(c * p[0] - s * p[1]), float(s * p[0] + c * p[1]));
}

// Add scale * m into the 2x2 block at (class row, class col) of the 2M system.
inline void addBlock(std::vector<Eigen::Triplet<double>> &trips,
                     int rcl,
                     int ccl,
                     const M2 &m,
                     double scale)
{
  trips.emplace_back(2 * rcl + 0, 2 * ccl + 0, m.a * scale);
  trips.emplace_back(2 * rcl + 0, 2 * ccl + 1, m.b * scale);
  trips.emplace_back(2 * rcl + 1, 2 * ccl + 0, m.c * scale);
  trips.emplace_back(2 * rcl + 1, 2 * ccl + 1, m.d * scale);
}
} // namespace

extern "C" void sc_napi_logf(const char *fmt, ...);

QuantizeStats computeQuantization(Mesh &m, const QuantizeParams &params)
{
  QuantizeStats stats;

  SeamlessParamParams spp;
  spp.target_edge_length = params.target_edge_length;
  spp.use_density = params.use_density;
  spp.gauge_eps = params.gauge_eps;

  sc_napi_logf("build seamless params\n");
  SeamlessSystem sys;
  if (!buildSeamlessSystem(m, spp, sys)) {
    return stats;
  }
  const int M = sys.M;
  stats.num_corners = sys.num_corners;
  stats.num_classes = M;

  sc_napi_logf("build quant graph\n");
  QuantGraph g = buildQuantGraph(m, sys.cornerClass, sys.gauge, sys.periodEC);
  const int S = g.num_sides();
  stats.num_cut_edges = S;

  const int N = 2 * M;
  const double eps = double(params.gauge_eps);
  const double inv_len =
      params.target_edge_length > 1e-12f ? 1.0 / double(params.target_edge_length) : 1.0;
  // Seam penalty makes each cut transition seamless (translation equal at both
  // endpoints); fix penalty locks a side onto its chosen integer. Capping the fix
  // penalty at max_lambda is what the over-constrained fallback starves.
  const double lam_seam = 1.0e6;
  const double lam_fix = params.max_lambda < 1.0e6 ? params.max_lambda : 1.0e6;

  // Per-side affine operators A = R(period - ga), B = R(-gb): the un-gauged seam
  // translation of an endpoint corner-pair is t = B x_b - A x_a.
  Vector<M2> Aop, Bop;
  Aop.resize(S);
  Bop.resize(S);
  for (int s = 0; s < S; s++) {
    Aop[s] = rotM(g.period[s] - g.ga[s]);
    Bop[s] = rotM(-g.gb[s]);
  }

  // Add lam * || sum_i C[i] x_{cls[i]} - d ||^2 to (trips, rhs): Hessian block
  // (i,j) is C[i]^T C[j]; the rhs gets C[i]^T d.
  auto addQuadPenalty = [](std::vector<Eigen::Triplet<double>> &trips,
                           Eigen::VectorXd &rhs,
                           int n,
                           const int *cls,
                           const M2 *C,
                           double dx,
                           double dy,
                           double lam) {
    for (int i = 0; i < n; i++) {
      for (int j = 0; j < n; j++) {
        addBlock(trips, cls[i], cls[j], mul(transp(C[i]), C[j]), lam);
      }
      double rx, ry;
      mv(transp(C[i]), dx, dy, rx, ry);
      rhs[2 * cls[i] + 0] += lam * rx;
      rhs[2 * cls[i] + 1] += lam * ry;
    }
  };

  // Sides locked to an integer (g.t_int[s]) so far.
  Vector<char> fixed;
  fixed.resize(S);
  for (int s = 0; s < S; s++) {
    fixed[s] = 0;
    g.t_int[s] = float2(0.0f, 0.0f);
  }

  // Base field RHS + base stiffness in class space. The injectivity untangling
  // pass recomputes both with per-face stiffening weights; they start as the
  // unweighted M4 system so the rounding phase below is unchanged.
  Eigen::VectorXd baseBu = sys.bu;
  Eigen::VectorXd baseBv = sys.bv;
  std::vector<Eigen::Triplet<double>> baseTripsW = sys.baseTrips;

  // Base block-diagonal system (M4 stiffness on U and on V) + per-component pins
  // + Tikhonov shift + the always-on seam penalty + the fix penalty for sides
  // already locked.
  auto assemble = [&](Eigen::SparseMatrix<double> &Lout, Eigen::VectorXd &rhs) {
    std::vector<Eigen::Triplet<double>> trips;
    trips.reserve(baseTripsW.size() * 2 + S * 96 + N);
    for (const auto &tr : baseTripsW) {
      trips.emplace_back(2 * tr.row() + 0, 2 * tr.col() + 0, tr.value());
      trips.emplace_back(2 * tr.row() + 1, 2 * tr.col() + 1, tr.value());
    }
    rhs = Eigen::VectorXd::Zero(N);
    for (int i = 0; i < M; i++) {
      rhs[2 * i + 0] = baseBu[i];
      rhs[2 * i + 1] = baseBv[i];
    }
    for (int i = 0; i < int(sys.pinClass.size()); i++) {
      int c = sys.pinClass[i];
      trips.emplace_back(2 * c + 0, 2 * c + 0, 1.0e6);
      trips.emplace_back(2 * c + 1, 2 * c + 1, 1.0e6);
    }
    for (int c = 0; c < N; c++) {
      trips.emplace_back(c, c, eps);
    }
    for (int s = 0; s < S; s++) {
      M2 A = Aop[s], B = Bop[s];
      // Seamless: B (x_clb1 - x_clb2) - A (x_cla1 - x_cla2) = 0 ties the two
      // endpoint pairs to one shared translation.
      int seamCls[4] = {g.clb[s], g.clb2[s], g.cla[s], g.cla2[s]};
      M2 seamC[4] = {B, negM(B), negM(A), A};
      addQuadPenalty(trips, rhs, 4, seamCls, seamC, 0.0, 0.0, lam_seam);
      if (fixed[s]) {
        double kx = double(g.t_int[s][0]), ky = double(g.t_int[s][1]);
        int p1[2] = {g.clb[s], g.cla[s]};
        int p2[2] = {g.clb2[s], g.cla2[s]};
        M2 pc[2] = {B, negM(A)};
        addQuadPenalty(trips, rhs, 2, p1, pc, kx, ky, lam_fix);
        addQuadPenalty(trips, rhs, 2, p2, pc, kx, ky, lam_fix);
      }
    }
    Lout.resize(N, N);
    Lout.setFromTriplets(trips.begin(), trips.end());
    Lout.makeCompressed();
  };

  sc_napi_logf("create solver\n");
  fflush(stdout);

#ifdef WASM
  Eigen::SimplicialLLT<Eigen::SparseMatrix<double>> solver;
#else
  Eigen::CholmodSupernodalLLT<Eigen::SparseMatrix<double>> solver;
#endif
  Eigen::VectorXd x = Eigen::VectorXd::Zero(N);
  bool all_solved = true;

  auto solveAll = [&]() -> bool {
    Eigen::SparseMatrix<double> L;
    Eigen::VectorXd rhs;
    sc_napi_logf("assemble\n");
    assemble(L, rhs);
    sc_napi_logf("compute\n");
    solver.compute(L);
    if (solver.info() != Eigen::Success) {
      sc_napi_logf("solver failed\n");
      return false;
    }
    sc_napi_logf("solve\n");
    x = solver.solve(rhs);
    sc_napi_logf(solver.info() == Eigen::Success ? "success\n" : "failure\n");
    return solver.info() == Eigen::Success;
  };

  // Per-face stiffening weight for the injectivity pass (1 = unweighted M4).
  Vector<double> faceW;
  faceW.resize(int(m.f.capacity()));
  for (int i = 0; i < int(m.f.capacity()); i++) {
    faceW[i] = 1.0;
  }
  bool have_density =
      params.use_density &&
      m.v.attrs.has(AttrType::FLOAT, litestl::util::string(".remesh.v.density"));
  BuiltinAttr<float, ".remesh.v.density"> density;
  if (have_density) {
    density.ensure(m.v.attrs);
  }
  BuiltinAttr<float, ".remesh.f.theta"> theta;
  theta.ensure(m.f.attrs);

  // Rebuild the class-space base stiffness + field RHS, scaling each face by
  // faceW[f]. Identical to buildSeamlessSystem's assembly (so faceW==1 reproduces
  // sys.baseTrips/bu/bv); heavily weighting a folded face pulls it toward its
  // (det>0) field-aligned target. Because the target uses the gauge-consistent
  // field angle it never fights the locked seams (unlike a free per-face rotation).
  auto rebuildBase = [&]() {
    baseTripsW.clear();
    baseBu.setZero();
    baseBv.setZero();
    Vector<int> cs;
    Vector<float2> loc;
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
      float3 X = sys.FX[f], Y = sys.FY[f];
      float3 p0 = m.v.co[m.c.v[cs[0]]];
      for (int i = 0; i < n; i++) {
        float3 d = m.v.co[m.c.v[cs[i]]] - p0;
        loc.append(float2(d.dot(X), d.dot(Y)));
      }
      double mag = inv_len;
      if (have_density) {
        double davg = 0.0;
        for (int i = 0; i < n; i++) {
          davg += double(density[m.c.v[cs[i]]]);
        }
        davg /= double(n);
        mag = inv_len * std::sqrt(davg > 1e-12 ? davg : 1e-12);
      }
      double alpha = double(theta[f]) + double(sys.gauge[f]) * HALF_PI;
      float2 tgt_u(float(mag * std::cos(alpha)), float(mag * std::sin(alpha)));
      float2 tgt_v(float(-mag * std::sin(alpha)), float(mag * std::cos(alpha)));
      double w = faceW[f];
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
          int ca = sys.cornerClass[cs[idx[a]]];
          for (int b = 0; b < 3; b++) {
            int cb = sys.cornerClass[cs[idx[b]]];
            baseTripsW.emplace_back(ca, cb, w * area * (Gx[a] * Gx[b] + Gy[a] * Gy[b]));
          }
          baseBu[ca] += w * area * (Gx[a] * double(tgt_u[0]) + Gy[a] * double(tgt_u[1]));
          baseBv[ca] += w * area * (Gx[a] * double(tgt_v[0]) + Gy[a] * double(tgt_v[1]));
        }
      }
    }
  };

  // Area-weighted det(grad u, grad v) of face f from the current gauged x (det is
  // gauge-invariant, so this equals the un-gauged Jacobian). <= 0 means folded.
  auto faceJac = [&](int f) -> double {
    int c0 = m.l.c[m.f.l[f]], cc = c0;
    Vector<int> cs;
    do {
      cs.append(cc);
      cc = m.c.next[cc];
    } while (cc != c0);
    int n = int(cs.size());
    if (n < 3)
      return 1.0;
    float3 X = sys.FX[f], Y = sys.FY[f];
    float3 p0 = m.v.co[m.c.v[cs[0]]];
    Vector<float2> loc;
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
        int ca = sys.cornerClass[cs[idx[a]]];
        dux += Gx[a] * x[2 * ca + 0];
        duy += Gy[a] * x[2 * ca + 0];
        dvx += Gx[a] * x[2 * ca + 1];
        dvy += Gy[a] * x[2 * ca + 1];
      }
      gux += area * dux;
      guy += area * duy;
      gvx += area * dvx;
      gvy += area * dvy;
      totA += area;
    }
    if (totA <= 1e-20)
      return 1.0;
    return (gux * gvy - guy * gvx) / (totA * totA);
  };

  // Realized translation of side s for both endpoint corner-pairs, t = B x_b -
  // A x_a (equals the un-gauged seam translation uv_b - R(p) uv_a).
  auto realizedT = [&](int s, double &t1x, double &t1y, double &t2x, double &t2y) {
    M2 A = Aop[s], B = Bop[s];
    double ax, ay, bx, by;
    mv(A, x[2 * g.cla[s] + 0], x[2 * g.cla[s] + 1], ax, ay);
    mv(B, x[2 * g.clb[s] + 0], x[2 * g.clb[s] + 1], bx, by);
    t1x = bx - ax;
    t1y = by - ay;
    mv(A, x[2 * g.cla2[s] + 0], x[2 * g.cla2[s] + 1], ax, ay);
    mv(B, x[2 * g.clb2[s] + 0], x[2 * g.clb2[s] + 1], bx, by);
    t2x = bx - ax;
    t2y = by - ay;
  };

  // Endpoint-averaged translation of side s and its distance to the nearest
  // integer (the seam penalty makes the two endpoints agree, so the average is
  // the well-defined per-edge translation).
  auto sideAvg = [&](int s, double &ax, double &ay) -> double {
    double t1x, t1y, t2x, t2y;
    realizedT(s, t1x, t1y, t2x, t2y);
    ax = 0.5 * (t1x + t2x);
    ay = 0.5 * (t1y + t2y);
    double dx = ax - std::round(ax), dy = ay - std::round(ay);
    return std::sqrt(dx * dx + dy * dy);
  };

  // Greedy maximal-independent-set rounding (Bommes 2013): solve seamlessly, lock
  // a batch of sides, re-solve so the rest absorb the integers, repeat. Rounding
  // all sides at once breaks loop closure (independent rounding is not a
  // cocycle); the batch must be *vertex-independent* (no two sides share an
  // endpoint vertex) so no interior one-ring loop has two of its spokes rounded
  // together. Most-confident-first within each round, re-solve, repeat.
  all_solved &= solveAll();

  sc_napi_logf("postsolve\n");
  stats.iters = 0;
  if (S > 0 && all_solved) {
    // Endpoint vertices of each side, for the vertex-independence test.
    Vector<int> sv0, sv1;
    sv0.resize(S);
    sv1.resize(S);
    for (int s = 0; s < S; s++) {
      int e = g.edge[s];
      sv0[s] = m.e.vs[e][0];
      sv1[s] = m.e.vs[e][1];
    }
    Vector<int> order;
    Vector<double> frac;
    Vector<float2> avg;
    order.resize(S);
    frac.resize(S);
    avg.resize(S);
    std::unordered_set<int> usedV;
    int remaining = S;
    const double tau = 0.1; // confidence radius: only lock sides this close to int
    // Each round locks an independent set (a vertex can't repeat), so the worst
    // case is one side per round; cap generously above S.
    const int max_rounds = S + 8;
    for (int iter = 1; iter <= max_rounds && remaining > 0; iter++) {
      sc_napi_logf("round %d of %d remaining=%d\n", iter, max_rounds, remaining);
      stats.iters = iter;
      int nun = 0;
      for (int s = 0; s < S; s++) {
        if (fixed[s]) {
          continue;
        }
        double ax, ay;
        frac[s] = sideAvg(s, ax, ay);
        avg[s] = float2(float(ax), float(ay));
        order[nun++] = s;
      }
      // Most-confident first; greedily lock a vertex-independent batch.
      std::sort(order.data(), order.data() + nun, [&](int a, int b) {
        return frac[a] < frac[b];
      });
      usedV.clear();
      int locked = 0;
      for (int i = 0; i < nun; i++) {
        int s = order[i];
        // Lock only confident sides (within tau); always allow the single most
        // confident (locked==0) so a round with nothing within tau still
        // progresses. order is ascending, so once past tau the rest are too.
        if (locked > 0 && frac[s] > tau) {
          break;
        }
        if (usedV.count(sv0[s]) || usedV.count(sv1[s])) {
          continue; // shares a one-ring with a side already locked this round
        }
        g.t_int[s] = float2(std::round(avg[s][0]), std::round(avg[s][1]));
        fixed[s] = 1;
        usedV.insert(sv0[s]);
        usedV.insert(sv1[s]);
        remaining--;
        locked++;
      }
      if (locked == 0) {
        break;
      }
      if (!solveAll()) {
        all_solved = false;
        break;
      }
    }
    // Lock any sides the round budget never reached, then realize once more.
    bool leftover = false;
    for (int s = 0; s < S; s++) {
      if (!fixed[s]) {
        double ax, ay;
        sideAvg(s, ax, ay);
        g.t_int[s] = float2(float(std::round(ax)), float(std::round(ay)));
        fixed[s] = 1;
        leftover = true;
      }
    }
    if (leftover && all_solved) {
      all_solved &= solveAll();
    }
  }

  // Local-stiffening injectivity pass (Bommes 2013 IGM). The integer grid is now
  // frozen by the seam/fix/singularity penalties; the linear MIQ map has no
  // injectivity guarantee and can fold where curvature concentrates. Each round
  // multiplies every folded face's Dirichlet energy + field RHS by a ramped
  // weight (capped below the 1e6 penalties so it never breaks the locked grid),
  // pulling it toward its det>0 field-aligned target. Keep the lowest-fold result.
  int inj_iters = params.inj_iters;
  if (all_solved && inj_iters > 0) {
    auto countFolds = [&]() {
      int nf = 0;
      for (int f : m.f) {
        if (faceJac(f) <= 0.0) {
          nf++;
        }
      }
      return nf;
    };
    Eigen::VectorXd bestX = x;
    int bestFold = countFolds();
    Vector<char> vmark;
    vmark.resize(int(m.v.capacity()));
    int stale = 0;
    for (int it = 0; it < inj_iters && bestFold > 0; it++) {
      // Mark every folded face's vertices, then stiffen the whole 1-ring around
      // them. A fold squeezed off one triangle tends to reappear on an edge
      // neighbor, so stiffening the patch (not just the folded face) converges
      // instead of ping-ponging the fold between neighbors.
      for (int i = 0; i < int(m.v.capacity()); i++) {
        vmark[i] = 0;
      }
      int nb = 0;
      for (int f : m.f) {
        if (faceJac(f) <= 0.0) {
          nb++;
          int c0 = m.l.c[m.f.l[f]], cc = c0;
          do {
            vmark[m.c.v[cc]] = 1;
            cc = m.c.next[cc];
          } while (cc != c0);
        }
      }
      if (nb == 0) {
        break;
      }
      for (int f : m.f) {
        int c0 = m.l.c[m.f.l[f]], cc = c0;
        bool touches = false;
        do {
          if (vmark[m.c.v[cc]]) {
            touches = true;
            break;
          }
          cc = m.c.next[cc];
        } while (cc != c0);
        if (touches) {
          faceW[f] = std::min(faceW[f] * 4.0, 1.0e4);
        }
      }
      rebuildBase();
      if (!solveAll()) {
        all_solved = false;
        break;
      }
      int fold = countFolds();
      if (fold < bestFold) {
        bestFold = fold;
        bestX = x;
        stale = 0;
      } else if (++stale >= 4) {
        break;
      }
    }
    x = bestX;
  }

  // Final integer residual: max over all sides of how far each endpoint's
  // realized translation sits from its locked integer (a side the budget could
  // not pull onto an integer shows up here -> reported infeasible).
  double residual = 0.0;
  for (int s = 0; s < S; s++) {
    double t1x, t1y, t2x, t2y;
    realizedT(s, t1x, t1y, t2x, t2y);
    g.t_real[s] = float2(float(0.5 * (t1x + t2x)), float(0.5 * (t1y + t2y)));
    double kx = double(g.t_int[s][0]), ky = double(g.t_int[s][1]);
    double d1 = std::sqrt((t1x - kx) * (t1x - kx) + (t1y - ky) * (t1y - ky));
    double d2 = std::sqrt((t2x - kx) * (t2x - kx) + (t2y - ky) * (t2y - ky));
    residual = std::fmax(residual, std::fmax(d1, d2));
  }

  stats.solved = all_solved;
  stats.max_integer_residual = residual;
  stats.feasible = all_solved && (residual < params.integer_tol);
  sc_napi_logf("postsolve2\n");

  // Write the snapped per-corner (u, v) and the integer per-edge translations.
  BuiltinAttr<float2, ".remesh.c.uv", AttrFlag::TEMP> uv;
  uv.ensure(m.c.attrs);
  // Persist the per-face gauge so extraction can recover the globally-coherent
  // gauged chart (the un-gauged uv below spirals on curved, non-trivial topology).
  BuiltinAttr<short, ".remesh.f.gauge_rot", AttrFlag::TEMP> gauge_rot;
  gauge_rot.ensure(m.f.attrs);
  for (int f : m.f) {
    gauge_rot[f] = short(sys.gauge[f]);
    double ang = -double(sys.gauge[f]) * HALF_PI;
    int c0 = m.l.c[m.f.l[f]], cc = c0;
    do {
      int cl = sys.cornerClass[cc];
      uv[cc] = rotc(ang, float2(float(x[2 * cl + 0]), float(x[2 * cl + 1])));
      cc = m.c.next[cc];
    } while (cc != c0);
  }

  BuiltinAttr<int2, ".remesh.e.translation_q", AttrFlag::TEMP> tq;
  tq.ensure(m.e.attrs);
  for (int e : m.e) {
    int s = g.sideOfEdge[e];
    if (s >= 0) {
      tq[e] = int2(int(std::lround(g.t_int[s][0])), int(std::lround(g.t_int[s][1])));
    } else {
      tq[e] = int2(0, 0);
    }
  }

  stats.max_loop_closure = loopClosureResidual(m, g, sys.periodEC);

  // Diagnostic: min per-face det(grad u, grad v) of the snapped map.
  Vector<int> cs;
  Vector<float2> loc;
  int num_faces = 0;
  double min_jac = 0.0;
  bool first_jac = true;
  int _loopguard = 0;
  for (int f : m.f) {
    cs.clear();
    loc.clear();
    int c0 = m.l.c[m.f.l[f]], cc = c0;
    do {
      cs.append(cc);
      cc = m.c.next[cc];
      if (_loopguard++ > 10000) {
        sc_napi_logf("mesh error infinite loop\n");
        break;
      }
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
    double jac = gux * gvy - guy * gvx;
    if (first_jac || jac < min_jac) {
      min_jac = jac;
      first_jac = false;
    }
  }
  stats.num_faces = num_faces;
  stats.min_jacobian = first_jac ? 0.0 : min_jac;

  return stats;
}

} // namespace sculptcore::remesh
