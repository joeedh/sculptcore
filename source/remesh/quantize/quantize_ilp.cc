#include "remesh/quantize/quantize_ilp.h"
#include "remesh/field/singularity_adjust.h"
#include "remesh/param/seamless_internal.h"
#include "remesh/param/seamless_param.h"
#include "remesh/quantize/t_mesh.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"

#include "litestl/math/vector.h"
#include "litestl/util/boolvector.h"
#include "litestl/util/string.h"
#include "litestl/util/task.h"
#include "litestl/util/vector.h"

#include "eigen/include/eigen5/Eigen/Sparse"
#include "eigen/include/eigen5/Eigen/SparseCholesky"

#ifndef WASM
#include <eigen/include/eigen5/Eigen/CholmodSupport>
#include <omp.h>
extern "C" void openblas_set_num_threads(int);
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
constexpr double QUARTER_PI = PI * 0.25;

// Reduce an angle to the cross field's fundamental domain (−π/4, π/4].
inline double reduceQuarter(double x)
{
  return x - HALF_PI * std::round(x / HALF_PI);
}

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

QuantizeStats computeQuantization(Mesh &m, const QuantizeParams &params)
{
  QuantizeStats stats;

  using Clock = std::chrono::steady_clock;
  auto msSince = [](Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  };
  const Clock::time_point t_total = Clock::now();
  // Solver-primitive time accumulators (filled by the solve lambdas below); the
  // rounding-phase split in stats is the delta of these around the rounding loop.
  double assemble_acc = 0.0, refactor_acc = 0.0, updown_acc = 0.0,
         backsolve_acc = 0.0;

  // Liveness: long phases stream throttled running sums to stdout so an
  // early-killed run still yields usable counters (the manifest is exit-only).
  Clock::time_point t_prog = Clock::now();
  auto progressDue = [&]() {
    if (msSince(t_prog) < 5000.0) {
      return false;
    }
    t_prog = Clock::now();
    return true;
  };

  SeamlessParamParams spp;
  spp.target_edge_length = params.target_edge_length;
  spp.use_density = params.use_density;
  spp.gauge_eps = params.gauge_eps;

  SeamlessSystem sys;
  if (!buildSeamlessSystem(m, spp, sys)) {
    stats.total_ms = msSince(t_total);
    return stats;
  }
  const int M = sys.M;
  stats.num_corners = sys.num_corners;
  stats.num_classes = M;

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
  double lam_seam = 1.0e6;
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

  // Hessian of lam * || sum_i C[i] x_{cls[i]} - d ||^2: block (i,j) is C[i]^T C[j].
  auto addQuadPenalty = [](std::vector<Eigen::Triplet<double>> &trips,
                           int n,
                           const int *cls,
                           const M2 *C,
                           double lam) {
    for (int i = 0; i < n; i++) {
      for (int j = 0; j < n; j++) {
        addBlock(trips, cls[i], cls[j], mul(transp(C[i]), C[j]), lam);
      }
    }
  };
  // The matching RHS term: lam * C[i]^T d per i.
  auto rhsQuadPenalty =
      [](Eigen::VectorXd &rhs, int n, const int *cls, const M2 *C, double dx,
         double dy, double lam) {
        for (int i = 0; i < n; i++) {
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

  // Frozen face list for the chunked parallel sweeps below. Topology is frozen
  // for the whole call and chunk boundaries are a pure function of (count,
  // grain), so per-chunk partials merged in ascending order are deterministic.
  Vector<int> faceIds;
  for (int f : m.f) {
    faceIds.append(f);
  }
  const int FN = int(faceIds.size());
  auto faceChunkCount = [&](int grain) {
    return FN > grain ? (FN + grain - 1) / grain : 1;
  };
  // body(chunk, begin, end) over faceIds[begin..end); chunks run concurrently.
  auto forFaces = [&](int grain, auto &&body) {
#ifdef NO_PARALLEL_FOR
    const int nchunk = faceChunkCount(grain);
    for (int c = 0; c < nchunk; c++) {
      int b = c * grain;
      body(c, b, std::min(FN, b + grain));
    }
#else
    litestl::task::parallel_for(
        litestl::util::IndexRange(FN),
        [&](litestl::util::IndexRange range) {
          body(range.start / grain, range.start, range.start + range.size);
        },
        grain);
#endif
  };
  // body(begin, end) over [0, count); chunked like forFaces (no chunk index).
  auto forRange = [&](int count, int grain, auto &&body) {
#ifdef NO_PARALLEL_FOR
    (void)grain;
    body(0, count);
#else
    litestl::task::parallel_for(
        litestl::util::IndexRange(count),
        [&](litestl::util::IndexRange range) {
          body(int(range.start), int(range.start + range.size));
        },
        grain);
#endif
  };
  constexpr int kFoldGrain = 512;
  constexpr int kBaseGrain = 256;
  // Shared parallel fold counter; `folded(f)` decides the per-face test.
  auto countFoldsPar = [&](auto &&folded) -> int {
    Vector<int> partial;
    partial.resize(faceChunkCount(kFoldGrain));
    for (int c = 0; c < int(partial.size()); c++) {
      partial[c] = 0;
    }
    forFaces(kFoldGrain, [&](int c, int b, int e) {
      int nf = 0;
      for (int i = b; i < e; i++) {
        nf += folded(faceIds[i]) ? 1 : 0;
      }
      partial[c] = nf;
    });
    int total = 0;
    for (int c = 0; c < int(partial.size()); c++) {
      total += partial[c];
    }
    return total;
  };
  // Parallel folded-face collection, ascending face order (chunks compact into
  // disjoint scratch regions; the merge walks chunks in order).
  Vector<int> foldedScratch, foldedChunkN;
  auto collectFoldedPar = [&](auto &&folded, Vector<int> &out) {
    const int nchunk = faceChunkCount(kFoldGrain);
    foldedScratch.resize(FN);
    foldedChunkN.resize(nchunk);
    forFaces(kFoldGrain, [&](int c, int b, int e) {
      int w = b;
      for (int i = b; i < e; i++) {
        if (folded(faceIds[i])) {
          foldedScratch[w++] = faceIds[i];
        }
      }
      foldedChunkN[c] = w - b;
    });
    out.clear();
    for (int c = 0; c < nchunk; c++) {
      const int b = c * kFoldGrain;
      for (int k = 0; k < foldedChunkN[c]; k++) {
        out.append(foldedScratch[b + k]);
      }
    }
  };

  // Base field RHS + base stiffness in class space. The injectivity untangling
  // pass recomputes both with per-face stiffening weights; they start as the
  // unweighted M4 system so the rounding phase below is unchanged.
  Eigen::VectorXd baseBu = sys.bu;
  Eigen::VectorXd baseBv = sys.bv;
  std::vector<Eigen::Triplet<double>> baseTripsW = sys.baseTrips;

  // The assembled system is kept alongside the factor (plans/miq.md Q1): every
  // full assemble rewrites Acur/bcur, and newly-locked sides fold their fix
  // penalty into Acur incrementally between assembles. The full symmetric
  // pattern is stored, so Eigen's compressed columns double as CSR rows for the
  // local GS tier.
  Eigen::SparseMatrix<double> Acur;
  Eigen::VectorXd bcur;

  // Slot maps for value-only re-assembly. The pattern never changes, so after
  // the first assemble every contribution stream's nnz slots are recorded once;
  // assembleValues() then scatter-adds in assemble()'s exact emission order,
  // reproducing setFromTriplets' duplicate-collapse bit-for-bit.
  bool patternReady = false;
  Vector<int> baseSlotU, baseSlotV; // per baseTripsW entry: (2r,2c) / (2r+1,2c+1)
  Vector<int> pinSlots, diagSlots;  // pin diagonal images; full diagonal (eps)
  Vector<int> seamSlots, fixSlots;  // 64 / 32 slots per side
  Vector<double> seamUnit, fixUnit; // unit-lam values for those slots

  auto slotOf = [&](int row, int col) -> int {
    const int *Ap = Acur.outerIndexPtr();
    const int *Ai = Acur.innerIndexPtr();
    const int *b = Ai + Ap[col];
    const int *e = Ai + Ap[col + 1];
    return int(std::lower_bound(b, e, row) - Ai);
  };

  auto recordSlots = [&]() {
    const int nb = int(baseTripsW.size());
    baseSlotU.resize(nb);
    baseSlotV.resize(nb);
    for (int i = 0; i < nb; i++) {
      int r = baseTripsW[i].row(), c = baseTripsW[i].col();
      baseSlotU[i] = slotOf(2 * r + 0, 2 * c + 0);
      baseSlotV[i] = slotOf(2 * r + 1, 2 * c + 1);
    }
    pinSlots.resize(int(sys.pinClass.size()) * 2);
    for (int i = 0; i < int(sys.pinClass.size()); i++) {
      int c = sys.pinClass[i];
      pinSlots[2 * i + 0] = slotOf(2 * c + 0, 2 * c + 0);
      pinSlots[2 * i + 1] = slotOf(2 * c + 1, 2 * c + 1);
    }
    diagSlots.resize(N);
    for (int c = 0; c < N; c++) {
      diagSlots[c] = slotOf(c, c);
    }
    seamSlots.resize(64 * S);
    seamUnit.resize(64 * S);
    fixSlots.resize(32 * S);
    fixUnit.resize(32 * S);
    for (int s = 0; s < S; s++) {
      M2 A = Aop[s], B = Bop[s];
      int seamCls[4] = {g.clb[s], g.clb2[s], g.cla[s], g.cla2[s]};
      M2 seamC[4] = {B, negM(B), negM(A), A};
      int k = 64 * s;
      for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
          M2 mb = mul(transp(seamC[i]), seamC[j]);
          int rcl = seamCls[i], ccl = seamCls[j];
          seamSlots[k] = slotOf(2 * rcl + 0, 2 * ccl + 0);
          seamUnit[k++] = mb.a;
          seamSlots[k] = slotOf(2 * rcl + 0, 2 * ccl + 1);
          seamUnit[k++] = mb.b;
          seamSlots[k] = slotOf(2 * rcl + 1, 2 * ccl + 0);
          seamUnit[k++] = mb.c;
          seamSlots[k] = slotOf(2 * rcl + 1, 2 * ccl + 1);
          seamUnit[k++] = mb.d;
        }
      }
      int pcls[2][2] = {{g.clb[s], g.cla[s]}, {g.clb2[s], g.cla2[s]}};
      M2 pc[2] = {B, negM(A)};
      k = 32 * s;
      for (int p = 0; p < 2; p++) {
        for (int i = 0; i < 2; i++) {
          for (int j = 0; j < 2; j++) {
            M2 mb = mul(transp(pc[i]), pc[j]);
            int rcl = pcls[p][i], ccl = pcls[p][j];
            fixSlots[k] = slotOf(2 * rcl + 0, 2 * ccl + 0);
            fixUnit[k++] = mb.a;
            fixSlots[k] = slotOf(2 * rcl + 0, 2 * ccl + 1);
            fixUnit[k++] = mb.b;
            fixSlots[k] = slotOf(2 * rcl + 1, 2 * ccl + 0);
            fixUnit[k++] = mb.c;
            fixSlots[k] = slotOf(2 * rcl + 1, 2 * ccl + 1);
            fixUnit[k++] = mb.d;
          }
        }
      }
    }
    patternReady = true;
  };

  // bcur = base field RHS + each side's seam/fix penalty RHS, in assemble()'s
  // historical accumulation order (the seam term contributes exact zeros).
  auto assembleBcur = [&]() {
    bcur = Eigen::VectorXd::Zero(N);
    for (int i = 0; i < M; i++) {
      bcur[2 * i + 0] = baseBu[i];
      bcur[2 * i + 1] = baseBv[i];
    }
    for (int s = 0; s < S; s++) {
      M2 A = Aop[s], B = Bop[s];
      int seamCls[4] = {g.clb[s], g.clb2[s], g.cla[s], g.cla2[s]};
      M2 seamC[4] = {B, negM(B), negM(A), A};
      rhsQuadPenalty(bcur, 4, seamCls, seamC, 0.0, 0.0, lam_seam);
      if (fixed[s]) {
        double kx = double(g.t_int[s][0]), ky = double(g.t_int[s][1]);
        int p1[2] = {g.clb[s], g.cla[s]};
        int p2[2] = {g.clb2[s], g.cla2[s]};
        M2 pc[2] = {B, negM(A)};
        rhsQuadPenalty(bcur, 2, p1, pc, kx, ky, lam_fix);
        rhsQuadPenalty(bcur, 2, p2, pc, kx, ky, lam_fix);
      }
    }
  };

  // Base block-diagonal system (M4 stiffness on U and on V) + per-component pins
  // + Tikhonov shift + the always-on seam penalty + the fix penalty for sides
  // already locked. Writes Acur/bcur. First-call path only; later assembles go
  // through assembleValues().
  auto assemble = [&]() {
    Clock::time_point t0 = Clock::now();
    std::vector<Eigen::Triplet<double>> trips;
    trips.reserve(baseTripsW.size() * 2 + S * 96 + N);
    for (const auto &tr : baseTripsW) {
      trips.emplace_back(2 * tr.row() + 0, 2 * tr.col() + 0, tr.value());
      trips.emplace_back(2 * tr.row() + 1, 2 * tr.col() + 1, tr.value());
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
      addQuadPenalty(trips, 4, seamCls, seamC, lam_seam);
      if (fixed[s]) {
        int p1[2] = {g.clb[s], g.cla[s]};
        int p2[2] = {g.clb2[s], g.cla2[s]};
        M2 pc[2] = {B, negM(A)};
        addQuadPenalty(trips, 2, p1, pc, lam_fix);
        addQuadPenalty(trips, 2, p2, pc, lam_fix);
      }
    }
    assembleBcur();
    Acur.resize(N, N);
    Acur.setFromTriplets(trips.begin(), trips.end());
    Acur.makeCompressed();
    if (!patternReady) {
      recordSlots();
    }
    assemble_acc += msSince(t0);
  };

  // Value-only re-assembly over the recorded slots, replaying assemble()'s
  // triplet-stream order so every slot accumulates identically.
  auto assembleValues = [&]() {
    Clock::time_point t0 = Clock::now();
    double *Av = Acur.valuePtr();
    std::memset(Av, 0, sizeof(double) * size_t(Acur.nonZeros()));
    const int nb = int(baseSlotU.size());
    for (int i = 0; i < nb; i++) {
      double v = baseTripsW[i].value();
      Av[baseSlotU[i]] += v;
      Av[baseSlotV[i]] += v;
    }
    for (int i = 0; i < int(pinSlots.size()); i++) {
      Av[pinSlots[i]] += 1.0e6;
    }
    for (int c = 0; c < N; c++) {
      Av[diagSlots[c]] += eps;
    }
    for (int s = 0; s < S; s++) {
      const int o = 64 * s;
      for (int k = 0; k < 64; k++) {
        Av[seamSlots[o + k]] += seamUnit[o + k] * lam_seam;
      }
      if (fixed[s]) {
        const int of = 32 * s;
        for (int k = 0; k < 32; k++) {
          Av[fixSlots[of + k]] += fixUnit[of + k] * lam_fix;
        }
      }
    }
    assembleBcur();
    assemble_acc += msSince(t0);
  };

  auto assembleSystem = [&]() {
    if (!patternReady) {
      assemble();
      return;
    }
    assembleValues();
  };

  // RHS-only assembly (base field + each locked side's fix penalty; the seam
  // penalty, pins and Tikhonov shift contribute zero RHS). Cheap — no sparse build.
  auto assembleRhs = [&](Eigen::VectorXd &rhs) {
    rhs = Eigen::VectorXd::Zero(N);
    for (int i = 0; i < M; i++) {
      rhs[2 * i + 0] = baseBu[i];
      rhs[2 * i + 1] = baseBv[i];
    }
    for (int s = 0; s < S; s++) {
      if (!fixed[s]) {
        continue;
      }
      double kx = double(g.t_int[s][0]), ky = double(g.t_int[s][1]);
      double rx, ry;
      mv(transp(Bop[s]), kx, ky, rx, ry); // B^T k at both clb endpoints
      rhs[2 * g.clb[s] + 0] += lam_fix * rx;
      rhs[2 * g.clb[s] + 1] += lam_fix * ry;
      rhs[2 * g.clb2[s] + 0] += lam_fix * rx;
      rhs[2 * g.clb2[s] + 1] += lam_fix * ry;
      mv(transp(negM(Aop[s])), kx, ky, rx, ry); // -A^T k at both cla endpoints
      rhs[2 * g.cla[s] + 0] += lam_fix * rx;
      rhs[2 * g.cla[s] + 1] += lam_fix * ry;
      rhs[2 * g.cla2[s] + 0] += lam_fix * rx;
      rhs[2 * g.cla2[s] + 1] += lam_fix * ry;
    }
  };

  Eigen::VectorXd x = Eigen::VectorXd::Zero(N);
  bool all_solved = true;

  // Fold a newly-locked side's fix penalty into the kept Acur. Values only: the
  // fix-penalty pattern is a subset of the always-present seam pattern, so every
  // slot was recorded off the first assemble.
  auto appendLockToSystem = [&](int s) {
    double *Av = Acur.valuePtr();
    const int o = 32 * s;
    for (int k = 0; k < 32; k++) {
      Av[fixSlots[o + k]] += fixUnit[o + k] * lam_fix;
    }
  };

  // Local Gauss-Seidel re-solve tier (plans/miq.md Q1, CoMISo Lesson 4).
  // Active-set GS over the kept Acur, seeded at the newly-locked sides' class
  // components: relax in index-sorted sweeps (deterministic — no hash-set
  // order), push a component's structural neighbors whenever it moved by more
  // than gs_tol, drain -> accept x, hit the visit cap -> escalate to the direct
  // path. The system is SPD so GS always converges; the cap only decides where
  // the local tier is worth it (the escalation rate is the Q5 decision data).
  // gs_tol is decision-grade, not metric-grade: rounding only consumes x via
  // sideAvg against tau ~ 0.3, and the loop's final solve is always direct.
  Eigen::VectorXd gsRhs;
  Vector<int> gsQ0, gsQ1, gsSeenList;
  Vector<char> gsInQ, gsSeen;
  gsInQ.resize(N);
  gsSeen.resize(N);
  for (int i = 0; i < N; i++) {
    gsInQ[i] = 0;
    gsSeen[i] = 0;
  }
  const double gs_tol = 1.0e-7;
  // x is the exact direct solution unless the last accepted solve was local.
  bool xIsLocal = false;

  auto tryLocalSolve = [&](const Vector<int> &newLocks) -> bool {
    Clock::time_point t0 = Clock::now();
    stats.gs_rounds++;
    gsQ0.clear();
    for (int li = 0; li < int(newLocks.size()); li++) {
      int s = newLocks[li];
      int cls[4] = {g.cla[s], g.cla2[s], g.clb[s], g.clb2[s]};
      for (int c = 0; c < 4; c++) {
        for (int k = 0; k < 2; k++) {
          int idx = 2 * cls[c] + k;
          if (!gsInQ[idx]) {
            gsInQ[idx] = 1;
            gsQ0.append(idx);
          }
        }
      }
    }
    std::sort(gsQ0.data(), gsQ0.data() + gsQ0.size());
    // Generous per-seed budget, hard-capped near one back-solve's work.
    const int cap = int(std::min<long long>(256ll * int(gsQ0.size()), 4ll * N));
    assembleRhs(gsRhs);
    const int *Ap = Acur.outerIndexPtr();
    const int *Ai = Acur.innerIndexPtr();
    const double *Av = Acur.valuePtr();
    gsSeenList.clear();
    Vector<int> *cur = &gsQ0, *nxt = &gsQ1;
    int visits = 0;
    bool converged = true;
    while (cur->size() > 0 && converged) {
      nxt->clear();
      for (int qi = 0; qi < int(cur->size()); qi++) {
        int i = (*cur)[qi];
        if (++visits > cap) {
          converged = false;
          break;
        }
        gsInQ[i] = 0;
        if (!gsSeen[i]) {
          gsSeen[i] = 1;
          gsSeenList.append(i);
        }
        // r_i = b_i - A_i . x over the symmetric column (== row) i.
        double r = gsRhs[i], diag = 0.0;
        for (int p = Ap[i]; p < Ap[i + 1]; p++) {
          int j = Ai[p];
          double a = Av[p];
          r -= a * x[j];
          if (j == i) {
            diag = a;
          }
        }
        if (diag <= 0.0) {
          continue; // never happens: Tikhonov shift keeps every diagonal > 0
        }
        double dx = r / diag;
        if (std::fabs(dx) <= gs_tol) {
          continue;
        }
        x[i] += dx;
        for (int p = Ap[i]; p < Ap[i + 1]; p++) {
          int j = Ai[p];
          if (j != i && !gsInQ[j]) {
            gsInQ[j] = 1;
            nxt->append(j);
          }
        }
      }
      std::sort(nxt->data(), nxt->data() + nxt->size());
      std::swap(cur, nxt);
    }
    // Reset the scratch flags (mid-sweep leftovers on escalation included).
    for (int qi = 0; qi < int(cur->size()); qi++) {
      gsInQ[(*cur)[qi]] = 0;
    }
    for (int qi = 0; qi < int(nxt->size()); qi++) {
      gsInQ[(*nxt)[qi]] = 0;
    }
    int touched = int(gsSeenList.size());
    for (int qi = 0; qi < touched; qi++) {
      gsSeen[gsSeenList[qi]] = 0;
    }
    stats.gs_visits += visits;
    stats.gs_touched_total += touched;
    stats.gs_touched_max = std::max(stats.gs_touched_max, touched);
    stats.gs_converged += converged ? 1 : 0;
    stats.gs_ms += msSince(t0);
    return converged;
  };

#ifdef WASM
  // WASM: Eigen SimplicialLLT has no rank update, so analyze once (the pattern is
  // invariant — fix penalties land where the seam penalty already has entries and
  // the injectivity pass only rescales) and factorize per round.
  Eigen::SimplicialLLT<Eigen::SparseMatrix<double>> solver;
  bool analyzed = false;
  auto solveAll = [&](bool /*fullRefactor*/) -> bool {
    assembleSystem();
    if (!analyzed) {
      solver.analyzePattern(Acur);
      if (solver.info() != Eigen::Success) {
        return false;
      }
      analyzed = true;
    }
    Clock::time_point t0 = Clock::now();
    solver.factorize(Acur);
    refactor_acc += msSince(t0);
    stats.full_refactors++;
    if (solver.info() != Eigen::Success) {
      return false;
    }
    t0 = Clock::now();
    x = solver.solve(bcur);
    backsolve_acc += msSince(t0);
    stats.back_solves++;
    return solver.info() == Eigen::Success;
  };
  // RHS-only solve against the kept factor (matrix unchanged since the last
  // solveAll). Used by the ARAP inner iterations.
  auto solveRhs = [&]() -> bool {
    Clock::time_point t0 = Clock::now();
    assembleBcur();
    x = solver.solve(bcur);
    backsolve_acc += msSince(t0);
    stats.back_solves++;
    return solver.info() == Eigen::Success;
  };
  // Multi-RHS solve against the kept factor (matrix unchanged); columns are
  // replaced by their solutions. Used by the Tier-1b probe batch.
  auto solveBatch = [&](Eigen::MatrixXd &B) -> bool {
    Clock::time_point t0 = Clock::now();
    Eigen::MatrixXd X = solver.solve(B);
    backsolve_acc += msSince(t0);
    stats.back_solves++;
    if (solver.info() != Eigen::Success) {
      return false;
    }
    B = X;
    return true;
  };
  // Probe-column RHS for the current g.t_int (same values solveAll would see).
  auto probeRhs = [&](Eigen::VectorXd &out) {
    assembleBcur();
    out = bcur;
  };
#else
  // Native: CHOLMOD. Analyze the (pattern-invariant) system once, then maintain
  // the factor incrementally. The greedy rounding loop's only per-round change
  // is the newly-locked sides' fix penalty — a rank<=4 PSD term whose nonzeros
  // are a subset of the always-present seam pattern — so each lock is a
  // cholmod_updown(+1) instead of a full re-factorization. The initial L0 and
  // the injectivity-pass refactors (base stiffness rescaled, not low-rank) stay
  // full numeric factorizations of the assembled matrix.
  //
  // Dual-factor scheme (opt-in, params.use_supernodal): full refactors run on
  // Lsuper, supernodal (BLAS-3) when CHOLMOD_AUTO picks it; cholmod_updown
  // needs a simplicial LDL', so locks apply to Lsimp, a lazy simplicial clone
  // refreshed once per refactor->updown transition. Lsolve = whichever factor
  // holds every lock. Default is forced-simplicial (see use_supernodal).
  cholmod_common cc;
  cholmod_start(&cc);
  // Quantize-scale supernodal fronts are too small for wide BLAS fan-out (16
  // threads measured 2.3x slower than 1-4 at ~18k classes): cap CHOLMOD's and
  // OpenBLAS's OpenMP thread counts, still honoring a lower OMP_NUM_THREADS.
  const int blas_threads = std::max(1, std::min(4, omp_get_max_threads()));
  cc.nthreads_max = blas_threads;
  openblas_set_num_threads(blas_threads);
  cc.supernodal = params.use_supernodal ? CHOLMOD_AUTO : CHOLMOD_SIMPLICIAL;
  cc.final_ll = 0; // keep simplicial factorizations LDL' (updown updates LDL')
  cholmod_factor *Lsuper = nullptr; // analyzed once; numeric-refactorized
  cholmod_factor *Lsimp = nullptr;  // simplicial clone; receives updowns
  cholmod_factor *Lsolve = nullptr; // factor holding all locks; solve target
  bool dualFactor = false;          // Lsuper is supernodal (AUTO's decision)
  bool simpCurrent = false;         // Lsimp matches Lsuper's current numerics
  double convert_acc = 0.0;
  Vector<int> Pinv; // original row -> factor-permuted row (cholmod_updown ordering)
  Vector<char> factored;
  factored.resize(S);
  for (int s = 0; s < S; s++) {
    factored[s] = 0;
  }
  // 4 updown cols/side -> updown when <= updown_max_cols/4 sides/round.
  const int updown_max_cols = params.updown_max_cols;

  // Full numeric factorization of the assembled Acur (analyze only the first
  // time; the pattern never changes). Records the permutation the rank updates
  // must follow and marks every currently-locked side as resident in the factor.
  auto factorFull = [&]() -> bool {
    const Eigen::SparseMatrix<double> &Aref = Acur; // const view for viewAsCholmod
    auto sym = Aref.selfadjointView<Eigen::Lower>();
    cholmod_sparse Ac = Eigen::viewAsCholmod(sym);
    auto analyze = [&]() -> bool {
      Lsuper = cholmod_analyze(&Ac, &cc);
      if (!Lsuper || cc.status < CHOLMOD_OK) {
        return false;
      }
      dualFactor = Lsuper->is_super != 0;
      Pinv.resize(N);
      const int *Perm = reinterpret_cast<const int *>(Lsuper->Perm);
      for (int k = 0; k < N; k++) {
        Pinv[Perm[k]] = k;
      }
      return true;
    };
    if (!Lsuper && !analyze()) {
      return false;
    }
    Clock::time_point t0 = Clock::now();
    cholmod_factorize(&Ac, Lsuper, &cc);
    refactor_acc += msSince(t0);
    stats.full_refactors++;
    if (cc.status < CHOLMOD_OK) {
      return false;
    }
    if (dualFactor && Lsuper->minor < size_t(N)) {
      // Supernodal LL' breakdown (the system spans 1e-9..1e6 scales): fall back
      // to the simplicial LDL' single-factor path for the rest of the run.
      cholmod_free_factor(&Lsuper, &cc);
      if (Lsimp) {
        cholmod_free_factor(&Lsimp, &cc);
      }
      cc.supernodal = CHOLMOD_SIMPLICIAL;
      if (!analyze()) {
        return false;
      }
      Clock::time_point t1 = Clock::now();
      cholmod_factorize(&Ac, Lsuper, &cc);
      refactor_acc += msSince(t1);
      stats.full_refactors++;
      if (cc.status < CHOLMOD_OK) {
        return false;
      }
    }
    simpCurrent = false;
    Lsolve = Lsuper;
    for (int s = 0; s < S; s++) {
      factored[s] = fixed[s];
    }
    return true;
  };

  // cholmod_updown(+1) for every side locked since the last factor/update. Each
  // side contributes two rank-2 columns C = sqrt(lam_fix)*[B,-A]^T over its two
  // endpoint class-pairs; rows are permuted into the factor's ordering (Pinv).
  // A false return means the caller must fall back to a full refactor.
  auto applyPendingLocks = [&]() -> bool {
    int pending = 0;
    for (int s = 0; s < S; s++) {
      pending += (fixed[s] && !factored[s]) ? 1 : 0;
    }
    if (pending == 0) {
      return true;
    }
    if (dualFactor && !simpCurrent) {
      // Refresh the simplicial clone off Lsuper's current numerics: one
      // copy + LL'->LDL' convert per refactor->updown transition.
      Clock::time_point tc = Clock::now();
      if (Lsimp) {
        cholmod_free_factor(&Lsimp, &cc);
      }
      Lsimp = cholmod_copy_factor(Lsuper, &cc);
      if (!Lsimp || cc.status < CHOLMOD_OK) {
        return false;
      }
      if (!cholmod_change_factor(CHOLMOD_REAL, /*to_ll=*/0, /*to_super=*/0,
                                 /*to_packed=*/1, /*to_monotonic=*/1, Lsimp, &cc) ||
          cc.status < CHOLMOD_OK) {
        return false;
      }
      convert_acc += msSince(tc);
      stats.simp_refreshes++;
      simpCurrent = true;
    }
    cholmod_factor *Lt = dualFactor ? Lsimp : Lsuper;
    Clock::time_point t0 = Clock::now();
    std::vector<Eigen::Triplet<double>> ct;
    const double sq = std::sqrt(lam_fix);
    int col = 0;
    auto emitPair = [&](int clb, int cla, const M2 &B, const M2 &A) {
      ct.emplace_back(Pinv[2 * clb + 0], col + 0, sq * B.a);
      ct.emplace_back(Pinv[2 * clb + 1], col + 0, sq * B.b);
      ct.emplace_back(Pinv[2 * cla + 0], col + 0, -sq * A.a);
      ct.emplace_back(Pinv[2 * cla + 1], col + 0, -sq * A.b);
      ct.emplace_back(Pinv[2 * clb + 0], col + 1, sq * B.c);
      ct.emplace_back(Pinv[2 * clb + 1], col + 1, sq * B.d);
      ct.emplace_back(Pinv[2 * cla + 0], col + 1, -sq * A.c);
      ct.emplace_back(Pinv[2 * cla + 1], col + 1, -sq * A.d);
      col += 2;
    };
    for (int s = 0; s < S; s++) {
      if (!fixed[s] || factored[s]) {
        continue;
      }
      emitPair(g.clb[s], g.cla[s], Bop[s], Aop[s]);
      emitPair(g.clb2[s], g.cla2[s], Bop[s], Aop[s]);
      factored[s] = 1;
    }
    Eigen::SparseMatrix<double> C(N, col);
    C.setFromTriplets(ct.begin(), ct.end());
    C.makeCompressed();
    cholmod_sparse Cc = Eigen::viewAsCholmod(C);
    int ok = cholmod_updown(1, &Cc, Lt, &cc);
    updown_acc += msSince(t0);
    stats.updowns++;
    if (!ok || cc.status < CHOLMOD_OK) {
      return false;
    }
    Lsolve = Lt;
    return true;
  };

  auto solveCurrent = [&]() -> bool {
    Clock::time_point t0 = Clock::now();
    Eigen::VectorXd rhs;
    assembleRhs(rhs);
    cholmod_dense bc = Eigen::viewAsCholmod(rhs);
    cholmod_dense *xc = cholmod_solve(CHOLMOD_A, Lsolve, &bc, &cc);
    if (!xc || cc.status < CHOLMOD_OK) {
      return false;
    }
    const double *xd = reinterpret_cast<const double *>(xc->x);
    x.resize(N);
    for (int i = 0; i < N; i++) {
      x[i] = xd[i];
    }
    cholmod_free_dense(&xc, &cc);
    backsolve_acc += msSince(t0);
    stats.back_solves++;
    return true;
  };

  auto solveAll = [&](bool fullRefactor) -> bool {
    // Pending = sides locked since the last factor/update; each costs 4 updown
    // columns. A big batch (early rounds) refactors faster than it updowns.
    int pending = 0;
    for (int s = 0; s < S; s++) {
      pending += (fixed[s] && !factored[s]) ? 1 : 0;
    }
    bool refactor = fullRefactor || !Lsuper || 4 * pending > updown_max_cols;
    if (!refactor && !applyPendingLocks()) {
      refactor = true; // clone/convert/updown failed -> rebuild from scratch
    }
    if (refactor) {
      assembleSystem();
      if (!factorFull()) {
        return false;
      }
    }
    return solveCurrent();
  };
  // RHS-only solve against the kept factor (matrix unchanged since the last
  // solveAll). Used by the ARAP inner iterations.
  auto solveRhs = [&]() -> bool { return solveCurrent(); };
  // Multi-RHS solve against Lsolve (matrix unchanged); columns are replaced by
  // their solutions. Used by the Tier-1b probe batch.
  auto solveBatch = [&](Eigen::MatrixXd &B) -> bool {
    Clock::time_point t0 = Clock::now();
    cholmod_dense bc = Eigen::viewAsCholmod(B);
    cholmod_dense *xc = cholmod_solve(CHOLMOD_A, Lsolve, &bc, &cc);
    if (!xc || cc.status < CHOLMOD_OK) {
      return false;
    }
    const double *xd = reinterpret_cast<const double *>(xc->x);
    for (int j = 0; j < int(B.cols()); j++) {
      for (int i = 0; i < int(B.rows()); i++) {
        B(i, j) = xd[size_t(j) * xc->d + i];
      }
    }
    cholmod_free_dense(&xc, &cc);
    backsolve_acc += msSince(t0);
    stats.back_solves++;
    return true;
  };
  // Probe-column RHS for the current g.t_int (same values solveCurrent uses).
  auto probeRhs = [&](Eigen::VectorXd &out) { assembleRhs(out); };
#endif

  // Per-round solve after locking a batch: fold the new locks into the kept
  // system, try the local GS tier, escalate to the direct path on a cap. The
  // direct path overwrites x wholesale, so a partial GS pass never leaks.
  auto solveRound = [&](Vector<int> &newLocks) -> bool {
    std::sort(newLocks.data(), newLocks.data() + newLocks.size());
    if (params.use_local_gs) {
      for (int i = 0; i < int(newLocks.size()); i++) {
        appendLockToSystem(newLocks[i]);
      }
      if (tryLocalSolve(newLocks)) {
        xIsLocal = true;
        return true;
      }
    }
    if (!solveAll(false)) {
      return false;
    }
    xIsLocal = false;
    return true;
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

  // When set, the base-RHS rebuild retargets every face to the nearest proper
  // rotation of its realized Jacobian instead of the rigid field angle. Off by
  // default (field-aligned); the ARAP untangle fallback below turns it on when
  // the field-aligned map folds too much. The rotation target sits beside
  // wherever the solver already is, so it relaxes folds without fighting the
  // locked seams — whereas the field angle is what folds the exactly-seamless map.
  // rot_max_dev caps how far the rotation target may sit from the nearest
  // field-aligned representative; QUARTER_PI = unclamped (legacy ARAP).
  bool rot_all = false;
  double rot_max_dev = QUARTER_PI;

  // Area-weighted gauged gradient (grad u, grad v) of the current solution x on f.
  auto faceGradX = [&](int f, double &gux, double &guy, double &gvx,
                       double &gvy) -> bool {
    Vector<int> cs;
    int c0 = m.l.c[m.f.l[f]], cc = c0;
    do {
      cs.append(cc);
      cc = m.c.next[cc];
    } while (cc != c0);
    int n = int(cs.size());
    if (n < 3) {
      return false;
    }
    float3 X = sys.FX[f], Y = sys.FY[f];
    float3 p0 = m.v.co[m.c.v[cs[0]]];
    Vector<float2> loc;
    for (int i = 0; i < n; i++) {
      float3 d = m.v.co[m.c.v[cs[i]]] - p0;
      loc.append(float2(d.dot(X), d.dot(Y)));
    }
    double Gux = 0, Guy = 0, Gvx = 0, Gvy = 0, totA = 0;
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
      Gux += area * dux;
      Guy += area * duy;
      Gvx += area * dvx;
      Gvy += area * dvy;
      totA += area;
    }
    if (totA <= 1e-20) {
      return false;
    }
    gux = Gux / totA;
    guy = Guy / totA;
    gvx = Gvx / totA;
    gvy = Gvy / totA;
    return true;
  };

  // Per-face segments of the base stiffness/RHS streams. Geometry + topology
  // are frozen for the whole call, so each face's triplet/RHS counts (and the
  // class each RHS entry feeds) never change; the layout mirrors
  // buildSeamlessSystem's emission order exactly.
  Vector<int> faceTripOfs, faceRhsOfs, rhsCls;
  Vector<double> buStream, bvStream;
  Vector<int> clsRhsOfs, clsRhsIdx;
  {
    faceTripOfs.resize(FN + 1);
    faceRhsOfs.resize(FN + 1);
    faceTripOfs[0] = 0;
    faceRhsOfs[0] = 0;
    Vector<int> cs;
    Vector<float2> loc;
    for (int i = 0; i < FN; i++) {
      int f = faceIds[i];
      cs.clear();
      loc.clear();
      int c0 = m.l.c[m.f.l[f]], cc = c0;
      do {
        cs.append(cc);
        cc = m.c.next[cc];
      } while (cc != c0);
      int n = int(cs.size());
      int ntri = 0;
      if (n >= 3) {
        float3 X = sys.FX[f], Y = sys.FY[f];
        float3 p0 = m.v.co[m.c.v[cs[0]]];
        for (int k = 0; k < n; k++) {
          float3 d = m.v.co[m.c.v[cs[k]]] - p0;
          loc.append(float2(d.dot(X), d.dot(Y)));
        }
        for (int t = 1; t + 1 < n; t++) {
          float2 q0 = loc[0], q1 = loc[t], q2 = loc[t + 1];
          double r1x = q1[0] - q0[0], r1y = q1[1] - q0[1];
          double r2x = q2[0] - q0[0], r2y = q2[1] - q0[1];
          if (std::fabs(r1x * r2y - r1y * r2x) < 1e-20) {
            continue;
          }
          ntri++;
          rhsCls.append(sys.cornerClass[cs[0]]);
          rhsCls.append(sys.cornerClass[cs[t]]);
          rhsCls.append(sys.cornerClass[cs[t + 1]]);
        }
      }
      faceTripOfs[i + 1] = faceTripOfs[i] + 9 * ntri;
      faceRhsOfs[i + 1] = faceRhsOfs[i] + 3 * ntri;
    }
    const int nrhs = faceRhsOfs[FN];
    buStream.resize(nrhs);
    bvStream.resize(nrhs);
    // Class -> ascending RHS-stream indices (CSR): the per-class gather in
    // rebuildBase then sums contributions in the serial loop's exact order.
    clsRhsOfs.resize(M + 1);
    for (int c = 0; c <= M; c++) {
      clsRhsOfs[c] = 0;
    }
    for (int k = 0; k < nrhs; k++) {
      clsRhsOfs[rhsCls[k] + 1]++;
    }
    for (int c = 0; c < M; c++) {
      clsRhsOfs[c + 1] += clsRhsOfs[c];
    }
    clsRhsIdx.resize(nrhs);
    Vector<int> fillN;
    fillN.resize(M);
    for (int c = 0; c < M; c++) {
      fillN[c] = 0;
    }
    for (int k = 0; k < nrhs; k++) {
      int c = rhsCls[k];
      clsRhsIdx[clsRhsOfs[c] + fillN[c]++] = k;
    }
    if (faceTripOfs[FN] != int(baseTripsW.size())) {
      // Layout must mirror buildSeamlessSystem exactly or the recorded slot
      // maps would silently corrupt. Should be unreachable.
      fprintf(stderr,
              "quantize: base-trip stream mismatch (%d vs %d)\n",
              faceTripOfs[FN],
              int(baseTripsW.size()));
      abort();
    }
  }

  // Rebuild the class-space base stiffness + field RHS, scaling each face by
  // faceW[f]. Identical to buildSeamlessSystem's assembly (so faceW==1 reproduces
  // sys.baseTrips/bu/bv); heavily weighting a folded face pulls it toward its
  // (det>0) field-aligned target. Because the target uses the gauge-consistent
  // field angle it never fights the locked seams (unlike a free per-face rotation).
  // matrixToo=false rebuilds only the RHS (ARAP: faceW == 1 and geometry static,
  // so the stiffness stream is invariant there).
  // Two passes: faces write their disjoint stream segments concurrently, then a
  // per-class gather sums each class in ascending stream order — bitwise-identical
  // to the serial face-order accumulation.
  auto rebuildBase = [&](bool matrixToo) {
    if (matrixToo) {
      baseTripsW.resize(size_t(faceTripOfs[FN]));
    }
    forFaces(kBaseGrain, [&](int, int i0, int i1) {
      Vector<int> cs;
      Vector<float2> loc;
      for (int i = i0; i < i1; i++) {
        int f = faceIds[i];
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
        for (int k = 0; k < n; k++) {
          float3 d = m.v.co[m.c.v[cs[k]]] - p0;
          loc.append(float2(d.dot(X), d.dot(Y)));
        }
        double mag = inv_len;
        if (have_density) {
          double davg = 0.0;
          for (int k = 0; k < n; k++) {
            davg += double(density[m.c.v[cs[k]]]);
          }
          davg /= double(n);
          mag = inv_len * std::sqrt(davg > 1e-12 ? davg : 1e-12);
        }
        float2 tgt_u, tgt_v;
        double gux, guy, gvx, gvy;
        if (rot_all && faceGradX(f, gux, guy, gvx, gvy)) {
          // Nearest proper rotation R to the realized gauged Jacobian
          // J=[[gux,guy],[gvx,gvy]]: angle=atan2(c-b,a+d), det(R)=+1.
          double th = std::atan2(gvx - guy, gux + gvy);
          if (rot_max_dev < QUARTER_PI - 1e-12) {
            // Field-aligned rotations sit at th = -alpha (mod 90 deg); snap to
            // the nearest one and keep at most rot_max_dev of deviation.
            double alpha = double(theta[f]) + double(sys.gauge[f]) * HALF_PI;
            double dev = reduceQuarter(th + alpha);
            th += std::clamp(dev, -rot_max_dev, rot_max_dev) - dev;
          }
          double C = std::cos(th), S = std::sin(th);
          tgt_u = float2(float(mag * C), float(-mag * S));
          tgt_v = float2(float(mag * S), float(mag * C));
        } else {
          double alpha = double(theta[f]) + double(sys.gauge[f]) * HALF_PI;
          tgt_u = float2(float(mag * std::cos(alpha)), float(mag * std::sin(alpha)));
          tgt_v = float2(float(-mag * std::sin(alpha)), float(mag * std::cos(alpha)));
        }
        double w = faceW[f];
        int kt = faceTripOfs[i];
        int kr = faceRhsOfs[i];
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
            if (matrixToo) {
              for (int b = 0; b < 3; b++) {
                int cb = sys.cornerClass[cs[idx[b]]];
                baseTripsW[kt++] = Eigen::Triplet<double>(
                    ca, cb, w * area * (Gx[a] * Gx[b] + Gy[a] * Gy[b]));
              }
            }
            buStream[kr] =
                w * area * (Gx[a] * double(tgt_u[0]) + Gy[a] * double(tgt_u[1]));
            bvStream[kr] =
                w * area * (Gx[a] * double(tgt_v[0]) + Gy[a] * double(tgt_v[1]));
            kr++;
          }
        }
      }
    });
    forRange(M, 2048, [&](int cls0, int cls1) {
      for (int c = cls0; c < cls1; c++) {
        double su = 0.0, sv = 0.0;
        for (int k = clsRhsOfs[c]; k < clsRhsOfs[c + 1]; k++) {
          int j = clsRhsIdx[k];
          su += buStream[j];
          sv += bvStream[j];
        }
        baseBu[c] = su;
        baseBv[c] = sv;
      }
    });
  };

  // Area-weighted det(grad u, grad v) of face f from the current gauged x (det is
  // gauge-invariant, so this equals the un-gauged Jacobian). <= 0 means folded.
  // xv overrides the solution vector read (defaults to x).
  auto faceJac = [&](int f, const double *xv = nullptr) -> double {
    const double *xp = xv ? xv : x.data();
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
        dux += Gx[a] * xp[2 * ca + 0];
        duy += Gy[a] * xp[2 * ca + 0];
        dvx += Gx[a] * xp[2 * ca + 1];
        dvy += Gy[a] * xp[2 * ca + 1];
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

  // Allocation-free triangle copy of faceJac — identical arithmetic, so the
  // chunked parallel fold sweeps stay bitwise-identical. Ngons fall back.
  auto faceJacFast = [&](int f) -> double {
    int c0 = m.l.c[m.f.l[f]], cc = c0, n = 0;
    int cs[3] = {0, 0, 0};
    do {
      if (n < 3) {
        cs[n] = cc;
      }
      n++;
      cc = m.c.next[cc];
    } while (cc != c0);
    if (n != 3) {
      return faceJac(f);
    }
    float3 X = sys.FX[f], Y = sys.FY[f];
    float3 p0 = m.v.co[m.c.v[cs[0]]];
    float2 loc[3];
    for (int i = 0; i < 3; i++) {
      float3 d = m.v.co[m.c.v[cs[i]]] - p0;
      loc[i] = float2(d.dot(X), d.dot(Y));
    }
    double gux = 0, guy = 0, gvx = 0, gvy = 0, totA = 0;
    float2 q0 = loc[0], q1 = loc[1], q2 = loc[2];
    double r1x = q1[0] - q0[0], r1y = q1[1] - q0[1];
    double r2x = q2[0] - q0[0], r2y = q2[1] - q0[1];
    double det = r1x * r2y - r1y * r2x;
    if (std::fabs(det) >= 1e-20) {
      double area = 0.5 * std::fabs(det);
      double Gx[3] = {(r1y - r2y) / det, r2y / det, -r1y / det};
      double Gy[3] = {(r2x - r1x) / det, -r2x / det, r1x / det};
      double dux = 0, duy = 0, dvx = 0, dvy = 0;
      for (int a = 0; a < 3; a++) {
        int ca = sys.cornerClass[cs[a]];
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
    if (totA <= 1e-20) {
      return 1.0;
    }
    return (gux * gvy - guy * gvx) / (totA * totA);
  };

  // The faceJac fold count (ARAP gate / stiffening / tier-3), parallel.
  auto countFoldsJ = [&]() {
    return countFoldsPar([&](int f) { return faceJacFast(f) <= 0.0; });
  };

  // Realized translation of side s for both endpoint corner-pairs, t = B x_b -
  // A x_a (equals the un-gauged seam translation uv_b - R(p) uv_a).
  // xv overrides the solution vector read (defaults to x).
  auto realizedT = [&](int s, double &t1x, double &t1y, double &t2x, double &t2y,
                       const double *xv = nullptr) {
    const double *xp = xv ? xv : x.data();
    M2 A = Aop[s], B = Bop[s];
    double ax, ay, bx, by;
    mv(A, xp[2 * g.cla[s] + 0], xp[2 * g.cla[s] + 1], ax, ay);
    mv(B, xp[2 * g.clb[s] + 0], xp[2 * g.clb[s] + 1], bx, by);
    t1x = bx - ax;
    t1y = by - ay;
    mv(A, xp[2 * g.cla2[s] + 0], xp[2 * g.cla2[s] + 1], ax, ay);
    mv(B, xp[2 * g.clb2[s] + 0], xp[2 * g.clb2[s] + 1], bx, by);
    t2x = bx - ax;
    t2y = by - ay;
  };

  // Endpoint-averaged translation of side s and its distance to the nearest
  // integer (the seam penalty makes the two endpoints agree, so the average is
  // the well-defined per-edge translation).
  auto sideAvg = [&](int s, double &ax, double &ay,
                     const double *xv = nullptr) -> double {
    double t1x, t1y, t2x, t2y;
    realizedT(s, t1x, t1y, t2x, t2y, xv);
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
  const double tau = params.confidence_radius; // only lock sides this close to int
  stats.setup_ms = msSince(t_total);
  Clock::time_point t_phase = Clock::now();
  all_solved &= solveAll(true); // initial seamless solve -> builds L0 (no locks)
  stats.initial_factor_ms = msSince(t_phase);

  // ARAP untangle fallback (see QuantizeParams::untangle_fold_threshold). The
  // seam-consistency penalty folds the field-aligned map where the field curls
  // (folds scale directly with lam_seam: ~0% at 0.1, ~34% at 1e6 on a rounded
  // blob). A single low-lam_seam solve is injective but not seamless (integers
  // can't lock). So when the field-aligned solve folds too much, walk lam_seam up
  // geometrically from a low weight, re-solving and ARAP-retargeting every face to
  // the nearest rotation of its realized Jacobian at each step: the map starts
  // injective and stays injective as the seams tighten back to seamless.
  const double untangle_thresh = params.untangle_fold_threshold;
  int initFolds = 0;
  if (all_solved) {
    // Tier-5 gate diagnostic (see QuantizeStats): do the raw seamless solve's
    // folds co-locate with spurious singularity pairs? The folded list's count
    // feeds the (unchanged) ARAP gate below.
    Vector<int> foldedInit;
    collectFoldedPar([&](int f) { return faceJacFast(f) <= 0.0; }, foldedInit);
    initFolds = int(foldedInit.size());
    stats.seamless_folds = initFolds;

    Vector<int> pairVerts;
    SingularityPairStats sps = findSingularityPairs(m, 2, &pairVerts);
    stats.num_singularities = sps.num_singularities;
    stats.spurious_pairs = sps.close_pairs;

    if (initFolds > 0 && pairVerts.size() > 0) {
      auto eachNeighbor = [&](int v, auto &&fn) {
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
      // Multi-source BFS: mark verts within kFoldHops of any paired pole,
      // then count folded faces touching a marked vert.
      constexpr int kFoldHops = 3;
      litestl::util::BoolVector<> nearPair;
      nearPair.resize(int(m.v.capacity()));
      nearPair.clear();
      Vector<int> front;
      for (int v : pairVerts) {
        if (!nearPair.set(v, true)) {
          front.append(v);
        }
      }
      int frontier = 0;
      for (int hop = 0; hop < kFoldHops; hop++) {
        int end = int(front.size());
        for (int i = frontier; i < end; i++) {
          eachNeighbor(front[i], [&](int vn) {
            if (!nearPair.set(vn, true)) {
              front.append(vn);
            }
          });
        }
        frontier = end;
      }
      int near = 0;
      for (int f : foldedInit) {
        int c0 = m.l.c[m.f.l[f]], cc = c0;
        bool hit = false;
        do {
          if (nearPair[m.c.v[cc]]) {
            hit = true;
            break;
          }
          cc = m.c.next[cc];
        } while (cc != c0);
        near += hit ? 1 : 0;
      }
      stats.seamless_folds_near_pairs = near;
    }
  }
  double initFoldFrac = m.f.count ? double(initFolds) / m.f.count : 0.0;
  t_phase = Clock::now();
  if (all_solved && untangle_thresh > 0.0 && initFoldFrac > untangle_thresh) {
    const double lam_hi = lam_seam, lam_lo = 0.1;
    const int steps = 16, inner = 3;
    rot_all = true; // retarget every face to nearest rotation (pure ARAP)
    const double dev_end =
        std::clamp(params.untangle_field_max_dev, 0.0, QUARTER_PI);
    double mult = std::pow(lam_hi / lam_lo, 1.0 / steps);
    double lam = lam_lo;
    for (int sIdx = 0; sIdx <= steps && all_solved; sIdx++) {
      lam_seam = (sIdx < steps) ? lam : lam_hi;
      // Budget schedule: free (45 deg) for the first quarter of the
      // continuation (the low-lam retarget needs full untangling power —
      // tightening earlier re-folds the map), then linear to dev_end by the
      // final step (tightening only at the end can no longer realign).
      double tfrac = std::clamp((double(sIdx) / steps - 0.25) / 0.75, 0.0, 1.0);
      rot_max_dev = QUARTER_PI + (dev_end - QUARTER_PI) * tfrac;
      // faceW == 1 and geometry static here: the matrix changes only with
      // lam_seam, so refactor once per lam step (k == 0) and re-solve the
      // retargeted RHS against the kept factor for the inner iterations.
      for (int k = 0; k < inner && all_solved; k++) {
        rebuildBase(sIdx == 0 && k == 0);
        all_solved &= (k == 0) ? solveAll(true) : solveRhs();
      }
      if (progressDue()) {
        std::printf("[quantize_progress] phase=arap step=%d/%d refactors=%d "
                    "elapsed=%.1fs\n",
                    sIdx + 1, steps + 1, stats.full_refactors,
                    msSince(t_total) / 1000.0);
      }
      lam *= mult;
    }
    lam_seam = lam_hi;
    rot_max_dev = dev_end;
    // rot_all stays on so the post-rounding injectivity pass keeps retargeting
    // residual folds to rotations rather than the (re-folding) field angle.
  }
  stats.arap_ms = msSince(t_phase);

  stats.iters = 0;
  // Rounding-phase solver split = deltas of the primitive accumulators.
  const double snap_assemble = assemble_acc, snap_refactor = refactor_acc,
               snap_updown = updown_acc, snap_backsolve = backsolve_acc;
  t_phase = Clock::now();
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
    Vector<int> newLocks;
    Vector<double> frac;
    Vector<float2> avg;
    frac.resize(S);
    avg.resize(S);
    std::unordered_set<int> usedV;
    int remaining = S;
    // Sides incident to each class (CSR; per-side duplicates harmless). After a
    // converged GS round only sides reading a visited class can have moved, so
    // the confidence re-sort is incremental (plans/miq.md Q2).
    Vector<int> clsOfs, clsSides;
    clsOfs.resize(M + 1);
    for (int c = 0; c <= M; c++) {
      clsOfs[c] = 0;
    }
    for (int s = 0; s < S; s++) {
      clsOfs[g.cla[s] + 1]++;
      clsOfs[g.clb[s] + 1]++;
      clsOfs[g.cla2[s] + 1]++;
      clsOfs[g.clb2[s] + 1]++;
    }
    for (int c = 0; c < M; c++) {
      clsOfs[c + 1] += clsOfs[c];
    }
    clsSides.resize(clsOfs[M]);
    {
      Vector<int> fill;
      fill.resize(M);
      for (int c = 0; c < M; c++) {
        fill[c] = 0;
      }
      for (int s = 0; s < S; s++) {
        int cls[4] = {g.cla[s], g.clb[s], g.cla2[s], g.clb2[s]};
        for (int k = 0; k < 4; k++) {
          clsSides[clsOfs[cls[k]] + fill[cls[k]]++] = s;
        }
      }
    }
    // Confidence priority queue (Q2): lazy min-heap keyed (frac, side) — the
    // side tie-break replaces std::sort's unspecified tie order. An entry is
    // current iff its gen matches sgen[side]; stale pops drop silently.
    struct HeapEnt {
      double frac;
      int side, gen;
    };
    auto heapAfter = [](const HeapEnt &a, const HeapEnt &b) {
      return a.frac != b.frac ? a.frac > b.frac : a.side > b.side;
    };
    Vector<HeapEnt> heap, stash;
    Vector<int> sgen, sdirtyList;
    Vector<char> sdirty;
    sgen.resize(S);
    sdirty.resize(S);
    for (int s = 0; s < S; s++) {
      sgen[s] = 0;
      sdirty[s] = 0;
    }
    auto pushSide = [&](int s) {
      double ax, ay;
      frac[s] = sideAvg(s, ax, ay);
      avg[s] = float2(float(ax), float(ay));
      heap.append(HeapEnt{frac[s], s, ++sgen[s]});
      std::push_heap(heap.data(), heap.data() + heap.size(), heapAfter);
      stats.resort_keys++;
    };
    auto rebuildHeap = [&]() { // direct solves rewrite x: re-key everything
      heap.clear();
      for (int s = 0; s < S; s++) {
        if (!fixed[s]) {
          double ax, ay;
          frac[s] = sideAvg(s, ax, ay);
          avg[s] = float2(float(ax), float(ay));
          heap.append(HeapEnt{frac[s], s, ++sgen[s]});
          stats.resort_keys++;
        }
      }
      std::make_heap(heap.data(), heap.data() + heap.size(), heapAfter);
      stats.resort_full++;
    };
    auto dirtyTouched = [&]() { // converged GS round: re-key visited classes only
      for (int qi = 0; qi < int(gsSeenList.size()); qi++) {
        int cls = gsSeenList[qi] / 2;
        for (int p = clsOfs[cls]; p < clsOfs[cls + 1]; p++) {
          int s = clsSides[p];
          if (!fixed[s] && !sdirty[s]) {
            sdirty[s] = 1;
            sdirtyList.append(s);
          }
        }
      }
      for (int qi = 0; qi < int(sdirtyList.size()); qi++) {
        pushSide(sdirtyList[qi]);
        sdirty[sdirtyList[qi]] = 0;
      }
      sdirtyList.clear();
      stats.resort_incr++;
    };
    // Each round locks an independent set (a vertex can't repeat), so the worst
    // case is one side per round; cap generously above S. DIRECT strategy runs
    // zero greedy rounds — everything locks at once in the block below.
    const bool greedy = params.rounding == RoundingStrategy::GREEDY;
    const int max_rounds = greedy ? S + 8 : 0;
    if (greedy) {
      rebuildHeap(); // round 1 keys against the initial/ARAP-settled x
    }
    for (int iter = 1; iter <= max_rounds && remaining > 0; iter++) {
      stats.iters = iter;
      // Most-confident first; greedily lock a vertex-independent batch.
      usedV.clear();
      newLocks.clear();
      stash.clear();
      int locked = 0;
      while (heap.size() > 0) {
        std::pop_heap(heap.data(), heap.data() + heap.size(), heapAfter);
        HeapEnt e = heap.pop_back();
        int s = e.side;
        if (fixed[s] || e.gen != sgen[s]) {
          continue; // stale: locked earlier or re-keyed since this push
        }
        // Lock only confident sides (within tau); always allow the single most
        // confident (locked==0) so a round with nothing within tau still
        // progresses. Pops ascend, so once past tau the rest are too.
        if (locked > 0 && e.frac > tau) {
          stash.append(e);
          break;
        }
        if (usedV.count(sv0[s]) || usedV.count(sv1[s])) {
          stash.append(e); // shares a one-ring with a side locked this round
          continue;
        }
        g.t_int[s] = float2(std::round(avg[s][0]), std::round(avg[s][1]));
        fixed[s] = 1;
        newLocks.append(s);
        usedV.insert(sv0[s]);
        usedV.insert(sv1[s]);
        remaining--;
        locked++;
      }
      for (int qi = 0; qi < int(stash.size()); qi++) { // unlocked pops persist
        heap.append(stash[qi]);
        std::push_heap(heap.data(), heap.data() + heap.size(), heapAfter);
      }
      if (locked == 0) {
        break;
      }
      if (!solveRound(newLocks)) { // local GS tier, else updown + back-solve
        all_solved = false;
        break;
      }
      if (progressDue()) {
        std::printf("[quantize_progress] phase=round iter=%d locked=%d/%d "
                    "updowns=%d refactors=%d gs_rounds=%d elapsed=%.1fs\n",
                    iter, S - remaining, S, stats.updowns, stats.full_refactors,
                    stats.gs_rounds, msSince(t_total) / 1000.0);
      }
      if (remaining > 0) {
        if (xIsLocal) {
          dirtyTouched();
        }
        else {
          rebuildHeap();
        }
      }
    }
    // Lock any sides the rounds never reached (all of them under DIRECT), then
    // realize once more. This final solve is always direct: everything
    // downstream (fold tests, Tier-1b residuals, the reported residual) must
    // see the exact solution, never a GS approximation.
    newLocks.clear();
    for (int s = 0; s < S; s++) {
      if (!fixed[s]) {
        double ax, ay;
        sideAvg(s, ax, ay);
        g.t_int[s] = float2(float(std::round(ax)), float(std::round(ay)));
        fixed[s] = 1;
        newLocks.append(s);
      }
    }
    if (params.use_local_gs) {
      for (int i = 0; i < int(newLocks.size()); i++) {
        appendLockToSystem(newLocks[i]);
      }
    }
    if (all_solved && (newLocks.size() > 0 || xIsLocal)) {
      all_solved &= solveAll(false); // incremental: updown all pending locks
      xIsLocal = false;
    }
  }
  stats.rounding_ms = msSince(t_phase);
  stats.round_assemble_ms = assemble_acc - snap_assemble;
  stats.round_refactor_ms = refactor_acc - snap_refactor;
  stats.round_updown_ms = updown_acc - snap_updown;
  stats.round_backsolve_ms = backsolve_acc - snap_backsolve;

  // Allocation-free per-triangle fold test (the solve mesh is triangulated). The
  // gauge is uniform per face, so the raw-class signed uv area times the reference
  // (material) orientation has the same sign as faceJac's det; <= 0 means folded.
  // Falls back to faceJac for any non-triangle face.
  auto isFolded = [&](int f, const double *xv = nullptr) -> bool {
    const double *xp = xv ? xv : x.data();
    int c0 = m.l.c[m.f.l[f]], cc = c0, n = 0, cs[3] = {0, 0, 0};
    do {
      if (n < 3) {
        cs[n] = cc;
      }
      n++;
      cc = m.c.next[cc];
    } while (cc != c0);
    if (n != 3) {
      return faceJac(f, xv) <= 0.0;
    }
    float3 X = sys.FX[f], Y = sys.FY[f];
    float3 p0 = m.v.co[m.c.v[cs[0]]];
    float3 d1 = m.v.co[m.c.v[cs[1]]] - p0, d2 = m.v.co[m.c.v[cs[2]]] - p0;
    double refDet = double(d1.dot(X)) * double(d2.dot(Y)) -
                    double(d1.dot(Y)) * double(d2.dot(X));
    int a0 = sys.cornerClass[cs[0]], a1 = sys.cornerClass[cs[1]], a2 = sys.cornerClass[cs[2]];
    double u0 = xp[2 * a0], v0 = xp[2 * a0 + 1];
    double uvDet = (xp[2 * a1] - u0) * (xp[2 * a2 + 1] - v0) -
                   (xp[2 * a1 + 1] - v0) * (xp[2 * a2] - u0);
    return uvDet * refDet <= 0.0;
  };

  // Tier-1b seam-integer relaxation. The greedy rounding froze every cut-edge
  // translation onto an integer; a fold wedged at a cut frequently clears if one
  // incident translation is bumped by a unit. We try each fold-adjacent side's four
  // unit moves with a cheap RHS-only re-solve (the factor already holds every lock),
  // keeping a move only when it strictly cuts the fold count *and* leaves the map
  // feasible (max integer residual within tol) -- so the integer cocycle stays
  // valid and the no-spiral guarantee is never traded away. Runs before the
  // injectivity stiffening so the re-solves use the clean (faceW == 1) factor.
  auto maxResidual = [&](const double *xv = nullptr) -> double {
    double r = 0.0;
    for (int s = 0; s < S; s++) {
      double t1x, t1y, t2x, t2y;
      realizedT(s, t1x, t1y, t2x, t2y, xv);
      double kx = double(g.t_int[s][0]), ky = double(g.t_int[s][1]);
      double d1 = std::sqrt((t1x - kx) * (t1x - kx) + (t1y - ky) * (t1y - ky));
      double d2 = std::sqrt((t2x - kx) * (t2x - kx) + (t2y - ky) * (t2y - ky));
      r = std::fmax(r, std::fmax(d1, d2));
    }
    return r;
  };
  t_phase = Clock::now();
  if (all_solved && S > 0 && params.seam_relax_iters > 0) {
    auto countF = [&]() {
      return countFoldsPar([&](int f) { return isFolded(f); });
    };
    // Fused fold count for the 4 probe solutions in one chunked face sweep.
    Vector<int> foldPart;
    auto countFolds4 = [&](const double *xs[4], const bool feas[4], int nf[4]) {
      const int nchunk = faceChunkCount(kFoldGrain);
      foldPart.resize(nchunk * 4);
      for (int i = 0; i < nchunk * 4; i++) {
        foldPart[i] = 0;
      }
      forFaces(kFoldGrain, [&](int c, int b, int e) {
        int cnt[4] = {0, 0, 0, 0};
        for (int i = b; i < e; i++) {
          for (int k = 0; k < 4; k++) {
            if (feas[k] && isFolded(faceIds[i], xs[k])) {
              cnt[k]++;
            }
          }
        }
        for (int k = 0; k < 4; k++) {
          foldPart[4 * c + k] = cnt[k];
        }
      });
      for (int k = 0; k < 4; k++) {
        nf[k] = 0;
      }
      for (int c = 0; c < nchunk; c++) {
        for (int k = 0; k < 4; k++) {
          nf[k] += foldPart[4 * c + k];
        }
      }
    };
    const float2 moves[4] = {float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1)};
    int curFold = countF();
    Vector<int> foldedFaces;
    Eigen::MatrixXd probeB(N, 4);
    Eigen::VectorXd probeCol;
    for (int round = 0; round < params.seam_relax_iters &&
                        curFold >= params.seam_relax_min_folds && all_solved;
         round++) {
      // Collect the cut-edge sides bordering a folded face (deterministic order).
      std::unordered_set<int> candSet;
      Vector<int> cand;
      collectFoldedPar([&](int f) { return isFolded(f); }, foldedFaces);
      for (int i = 0; i < int(foldedFaces.size()); i++) {
        int c0 = m.l.c[m.f.l[foldedFaces[i]]], cc = c0;
        do {
          int s = g.sideOfEdge[m.c.e[cc]];
          if (s >= 0 && candSet.insert(s).second) {
            cand.append(s);
          }
          cc = m.c.next[cc];
        } while (cc != c0);
      }
      std::sort(cand.data(), cand.data() + cand.size());
      bool anyAccept = false;
      for (int ci = 0; ci < int(cand.size()) && all_solved; ci++) {
        int s = cand[ci];
        float2 base_k = g.t_int[s];
        // All four unit moves solve as one multi-RHS batch against the kept
        // factor (the matrix never changes here); x stays untouched until a
        // move is accepted, so no settle re-solve is needed.
        for (int mi = 0; mi < 4; mi++) {
          g.t_int[s] = base_k + moves[mi];
          stats.tier1b_probes++;
          probeRhs(probeCol);
          probeB.col(mi) = probeCol;
        }
        g.t_int[s] = base_k;
        if (!solveBatch(probeB)) {
          all_solved = false;
          break;
        }
        // Each column's residual is measured against its own probe integer at
        // side s (maxResidual reads g.t_int), exactly like the serial probes.
        const double *xs[4];
        bool feas[4];
        for (int mi = 0; mi < 4; mi++) {
          xs[mi] = probeB.col(mi).data();
          g.t_int[s] = base_k + moves[mi];
          feas[mi] = maxResidual(xs[mi]) <= params.integer_tol;
        }
        g.t_int[s] = base_k;
        int nf[4];
        countFolds4(xs, feas, nf);
        int bestM = -1, bestFold = curFold;
        for (int mi = 0; mi < 4; mi++) {
          if (feas[mi] && nf[mi] < bestFold) {
            bestFold = nf[mi];
            bestM = mi;
          }
        }
        if (bestM >= 0) {
          g.t_int[s] = base_k + moves[bestM];
          x = probeB.col(bestM); // == the settle solve: same factor, same RHS
          curFold = bestFold;
          anyAccept = true;
        }
        if (progressDue()) {
          std::printf("[quantize_progress] phase=tier1b round=%d cand=%d/%d "
                      "probes=%d folds=%d elapsed=%.1fs\n",
                      round, ci + 1, int(cand.size()), stats.tier1b_probes,
                      curFold, msSince(t_total) / 1000.0);
        }
      }
      if (!anyAccept) {
        break;
      }
    }
  }
  stats.tier1b_ms = msSince(t_phase);

  // Local-stiffening injectivity pass (Bommes 2013 IGM). The integer grid is now
  // frozen by the seam/fix/singularity penalties; the linear MIQ map has no
  // injectivity guarantee and can fold where curvature concentrates. Each round
  // multiplies every folded face's Dirichlet energy + field RHS by a ramped
  // weight (capped below the 1e6 penalties so it never breaks the locked grid),
  // pulling it toward its det>0 field-aligned target. Keep the lowest-fold result.
  int inj_iters = params.inj_iters;
  t_phase = Clock::now();
  if (all_solved && inj_iters > 0) {
    Eigen::VectorXd bestX = x;
    int bestFold = countFoldsJ();
    Vector<char> vmark;
    vmark.resize(int(m.v.capacity()));
    Vector<int> foldedFaces;
    int stale = 0;
    for (int it = 0; it < inj_iters && bestFold > 0; it++) {
      // Mark every folded face's vertices, then stiffen the whole 1-ring around
      // them. A fold squeezed off one triangle tends to reappear on an edge
      // neighbor, so stiffening the patch (not just the folded face) converges
      // instead of ping-ponging the fold between neighbors.
      for (int i = 0; i < int(m.v.capacity()); i++) {
        vmark[i] = 0;
      }
      collectFoldedPar([&](int f) { return faceJacFast(f) <= 0.0; }, foldedFaces);
      if (foldedFaces.size() == 0) {
        break;
      }
      for (int i = 0; i < int(foldedFaces.size()); i++) {
        int c0 = m.l.c[m.f.l[foldedFaces[i]]], cc = c0;
        do {
          vmark[m.c.v[cc]] = 1;
          cc = m.c.next[cc];
        } while (cc != c0);
      }
      forFaces(kFoldGrain, [&](int, int i0, int i1) {
        for (int i = i0; i < i1; i++) {
          int f = faceIds[i];
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
      });
      rebuildBase(true);
      if (!solveAll(true)) { // base stiffness rescaled -> full re-factorization
        all_solved = false;
        break;
      }
      int fold = countFoldsJ();
      if (fold < bestFold) {
        bestFold = fold;
        bestX = x;
        stale = 0;
      } else if (++stale >= 4) {
        break;
      }
      if (progressDue()) {
        std::printf("[quantize_progress] phase=stiffen iter=%d/%d folds=%d "
                    "best=%d elapsed=%.1fs\n",
                    it + 1, inj_iters, fold, bestFold,
                    msSince(t_total) / 1000.0);
      }
    }
    x = bestX;
  }
  stats.stiffen_ms = msSince(t_phase);

  // Tier-3 local fold-patch re-parametrization. The global injectivity pass above
  // stiffens folded 1-rings toward the field, but a fold wedged between two locked
  // seams cannot flatten that way. Here we instead *move* the fold patch's interior
  // classes directly: collect each folded face plus a few neighbor rings, pin that
  // patch's boundary (and every seam-endpoint / gauge-pinned class, so the locked
  // integer translations and iso-line continuity are untouched), and minimize the
  // convex fold-removal energy  sum_t max(0, delta - area_t)^2  over the movable
  // interior classes. area_t (the per-triangle signed uv area, sign-matched to
  // faceJac via the reference orientation) is linear in each class, so the energy
  // is convex and plain gradient descent reaches the global optimum for the fixed
  // boundary. Accept only if the total fold count drops. This never re-solves the
  // global system and never moves a seam endpoint -> seamlessness is preserved.
  t_phase = Clock::now();
  if (all_solved && params.local_untangle_iters > 0) {
    Vector<int> foldedFaces;
    collectFoldedPar([&](int f) { return faceJacFast(f) <= 0.0; }, foldedFaces);
    int fold0 = int(foldedFaces.size());
    if (fold0 > 0) {
      const int grow = params.local_untangle_grow;
      // 1. Mark folded faces, then grow the patch by `grow` vertex-rings so the
      //    pinned boundary sits a few faces away from the inversion.
      Vector<char> fmark, vmk;
      fmark.resize(int(m.f.capacity()));
      vmk.resize(int(m.v.capacity()));
      for (int i = 0; i < int(m.f.capacity()); i++) {
        fmark[i] = 0;
      }
      for (int i = 0; i < fold0; i++) {
        fmark[foldedFaces[i]] = 1;
      }
      for (int r = 0; r < grow; r++) {
        for (int i = 0; i < int(m.v.capacity()); i++) {
          vmk[i] = 0;
        }
        for (int f : m.f) {
          if (!fmark[f]) {
            continue;
          }
          int c0 = m.l.c[m.f.l[f]], cc = c0;
          do {
            vmk[m.c.v[cc]] = 1;
            cc = m.c.next[cc];
          } while (cc != c0);
        }
        for (int f : m.f) {
          if (fmark[f]) {
            continue;
          }
          int c0 = m.l.c[m.f.l[f]], cc = c0;
          bool touch = false;
          do {
            if (vmk[m.c.v[cc]]) {
              touch = true;
              break;
            }
            cc = m.c.next[cc];
          } while (cc != c0);
          if (touch) {
            fmark[f] = 1;
          }
        }
      }
      // 2. Pin every seam-endpoint and gauge-pin class (moving these would shift a
      //    locked cut translation or the gauge anchor and tear the IGM).
      Vector<char> pinned;
      pinned.resize(M);
      for (int i = 0; i < M; i++) {
        pinned[i] = 0;
      }
      for (int i = 0; i < int(sys.pinClass.size()); i++) {
        pinned[sys.pinClass[i]] = 1;
      }
      for (int s = 0; s < S; s++) {
        pinned[g.cla[s]] = 1;
        pinned[g.clb[s]] = 1;
        pinned[g.cla2[s]] = 1;
        pinned[g.clb2[s]] = 1;
      }
      // 3. A class is movable iff every corner that maps to it lies in the patch
      //    (so the surrounding map stays C0) and it is not pinned.
      Vector<int> cin, cout;
      cin.resize(M);
      cout.resize(M);
      for (int i = 0; i < M; i++) {
        cin[i] = 0;
        cout[i] = 0;
      }
      for (int f : m.f) {
        int c0 = m.l.c[m.f.l[f]], cc = c0;
        do {
          int cl = sys.cornerClass[cc];
          if (fmark[f]) {
            cin[cl]++;
          } else {
            cout[cl]++;
          }
          cc = m.c.next[cc];
        } while (cc != c0);
      }
      Vector<char> movable;
      movable.resize(M);
      int nmov = 0;
      for (int i = 0; i < M; i++) {
        movable[i] = (cin[i] > 0 && cout[i] == 0 && !pinned[i]) ? 1 : 0;
        if (movable[i]) {
          nmov++;
        }
      }
      if (nmov > 0) {
        // 4. Fan-triangulate the patch faces into class-space triangles. `sgn` is
        //    the reference (material) orientation, so sgn*uvArea has the same sign
        //    as faceJac; targeting sgn*uvArea >= delta removes the inversion.
        struct Tri {
          int a, b, c;
          double sgn;
        };
        std::vector<Tri> tris;
        double areaAcc = 0.0;
        int areaN = 0;
        Vector<int> cs;
        Vector<float2> loc;
        for (int f : m.f) {
          if (!fmark[f]) {
            continue;
          }
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
          for (int t = 1; t + 1 < n; t++) {
            float2 q0 = loc[0], q1 = loc[t], q2 = loc[t + 1];
            double refDet =
                (q1[0] - q0[0]) * (q2[1] - q0[1]) - (q1[1] - q0[1]) * (q2[0] - q0[0]);
            if (std::fabs(refDet) < 1e-20) {
              continue;
            }
            Tri tr;
            tr.a = sys.cornerClass[cs[0]];
            tr.b = sys.cornerClass[cs[t]];
            tr.c = sys.cornerClass[cs[t + 1]];
            tr.sgn = refDet > 0.0 ? 1.0 : -1.0;
            tris.push_back(tr);
            double ua = x[2 * tr.a], va = x[2 * tr.a + 1];
            double ub = x[2 * tr.b], vb = x[2 * tr.b + 1];
            double uc = x[2 * tr.c], vc = x[2 * tr.c + 1];
            areaAcc += std::fabs((ub - ua) * (vc - va) - (vb - va) * (uc - ua));
            areaN++;
          }
        }
        double meanA = areaN ? areaAcc / areaN : 1.0;
        double delta = 0.05 * meanA; // gentle positive injectivity margin
        auto sa = [&](const Tri &t, const Eigen::VectorXd &xx) -> double {
          double ua = xx[2 * t.a], va = xx[2 * t.a + 1];
          double ub = xx[2 * t.b], vb = xx[2 * t.b + 1];
          double uc = xx[2 * t.c], vc = xx[2 * t.c + 1];
          return t.sgn * ((ub - ua) * (vc - va) - (vb - va) * (uc - ua));
        };
        auto energy = [&](const Eigen::VectorXd &xx) -> double {
          double E = 0.0;
          for (const Tri &t : tris) {
            double s = sa(t, xx);
            if (s < delta) {
              double rr = delta - s;
              E += rr * rr;
            }
          }
          return E;
        };
        Eigen::VectorXd xc = x;
        Eigen::VectorXd grad = Eigen::VectorXd::Zero(N);
        double E = energy(xc);
        double step = -1.0;
        for (int it = 0; it < params.local_untangle_iters && E > 0.0; it++) {
          grad.setZero();
          for (const Tri &t : tris) {
            double s = sa(t, xc);
            if (s >= delta) {
              continue;
            }
            // dE/dpos = -2(delta - s) * sgn * d(uvArea)/dpos.
            double gc = 2.0 * (delta - s) * (-1.0) * t.sgn;
            double ua = xc[2 * t.a], va = xc[2 * t.a + 1];
            double ub = xc[2 * t.b], vb = xc[2 * t.b + 1];
            double uc = xc[2 * t.c], vc = xc[2 * t.c + 1];
            if (movable[t.a]) {
              grad[2 * t.a + 0] += gc * (vb - vc);
              grad[2 * t.a + 1] += gc * (uc - ub);
            }
            if (movable[t.b]) {
              grad[2 * t.b + 0] += gc * (vc - va);
              grad[2 * t.b + 1] += gc * (ua - uc);
            }
            if (movable[t.c]) {
              grad[2 * t.c + 0] += gc * (va - vb);
              grad[2 * t.c + 1] += gc * (ub - ua);
            }
          }
          double gnorm2 = 0.0;
          for (int i = 0; i < M; i++) {
            if (movable[i]) {
              gnorm2 += grad[2 * i] * grad[2 * i] + grad[2 * i + 1] * grad[2 * i + 1];
            }
          }
          if (gnorm2 < 1e-30) {
            break;
          }
          if (step < 0.0) {
            step = meanA / std::sqrt(gnorm2); // scale-aware first guess
          }
          Eigen::VectorXd xt = xc;
          double Et = E;
          bool improved = false;
          for (int ls = 0; ls < 30; ls++) {
            xt = xc;
            for (int i = 0; i < M; i++) {
              if (movable[i]) {
                xt[2 * i + 0] -= step * grad[2 * i + 0];
                xt[2 * i + 1] -= step * grad[2 * i + 1];
              }
            }
            Et = energy(xt);
            if (Et < E - 1e-12 * E) {
              improved = true;
              break;
            }
            step *= 0.5;
          }
          if (!improved) {
            break;
          }
          xc = xt;
          E = Et;
          step *= 1.5; // convex: creep the step back up between iterations
        }
        // Accept the re-parametrized patch only if it strictly reduces folds.
        Eigen::VectorXd xsave = x;
        x = xc;
        int fold1 = countFoldsJ();
        if (fold1 >= fold0) {
          x = xsave;
        }
      }
    }
  }
  stats.tier3_ms = msSince(t_phase);

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

  // Write the snapped per-corner (u, v) and the integer per-edge translations.
  BuiltinAttr<float2, ".remesh.c.uv", AttrFlag::TEMP> uv;
  uv.ensure(m.c.attrs);
  // Persist the per-face gauge so extraction can recover the globally-coherent
  // gauged chart (the un-gauged uv below spirals on curved, non-trivial topology).
  BuiltinAttr<short, ".remesh.f.gauge_rot", AttrFlag::TEMP> gauge_rot;
  gauge_rot.ensure(m.f.attrs);
  forFaces(kFoldGrain, [&](int, int i0, int i1) {
    for (int i = i0; i < i1; i++) {
      int f = faceIds[i];
      gauge_rot[f] = short(sys.gauge[f]);
      double ang = -double(sys.gauge[f]) * HALF_PI;
      int c0 = m.l.c[m.f.l[f]], cc = c0;
      do {
        int cl = sys.cornerClass[cc];
        uv[cc] = rotc(ang, float2(float(x[2 * cl + 0]), float(x[2 * cl + 1])));
        cc = m.c.next[cc];
      } while (cc != c0);
    }
  });

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

  // Diagnostic: min per-face det(grad u, grad v) of the snapped map, plus the
  // pre-extraction fold count (faces whose map Jacobian is non-positive). This is
  // the parametrization fold count Tier-0's run report surfaces — distinct from
  // the output mesh's `inverted_faces`. Counts and the min-jac merge are
  // order-independent, so the chunked parallel sweep is deterministic.
  int num_faces = 0;
  int num_folds = 0;
  double min_jac = 0.0;
  bool first_jac = true;
  // Field-alignment accumulators: area-weighted |angle(grad u) - theta| mod
  // 90 deg (uv is un-gauged, so plain theta is the reference).
  double dev_area = 0.0, dev_sum = 0.0, dev_max = 0.0, dev_bad = 0.0;
  {
    const int nchunk = faceChunkCount(kFoldGrain);
    Vector<int> pNFace, pNFold;
    Vector<double> pMinJ;
    Vector<char> pHasJ;
    Vector<double> pDevA, pDevS, pDevM, pDevB;
    pNFace.resize(nchunk);
    pNFold.resize(nchunk);
    pMinJ.resize(nchunk);
    pHasJ.resize(nchunk);
    pDevA.resize(nchunk);
    pDevS.resize(nchunk);
    pDevM.resize(nchunk);
    pDevB.resize(nchunk);
    forFaces(kFoldGrain, [&](int ch, int i0, int i1) {
      Vector<int> cs;
      Vector<float2> loc;
      int nfc = 0, nfo = 0;
      double mj = 0.0;
      bool first = true;
      double dA = 0.0, dS = 0.0, dM = 0.0, dB = 0.0;
      for (int i = i0; i < i1; i++) {
        int f = faceIds[i];
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
        nfc++;
        float3 X = sys.FX[f], Y = sys.FY[f];
        float3 p0 = m.v.co[m.c.v[cs[0]]];
        for (int k = 0; k < n; k++) {
          float3 d = m.v.co[m.c.v[cs[k]]] - p0;
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
        if (jac <= 0.0) {
          nfo++;
        }
        if (first || jac < mj) {
          mj = jac;
          first = false;
        }
        if (gux * gux + guy * guy > 1e-20) {
          double dev =
              std::fabs(reduceQuarter(std::atan2(guy, gux) - double(theta[f])));
          dA += totA;
          dS += totA * dev;
          dM = std::fmax(dM, dev);
          if (dev > PI / 8.0) {
            dB += totA;
          }
        }
      }
      pNFace[ch] = nfc;
      pNFold[ch] = nfo;
      pMinJ[ch] = mj;
      pHasJ[ch] = first ? 0 : 1;
      pDevA[ch] = dA;
      pDevS[ch] = dS;
      pDevM[ch] = dM;
      pDevB[ch] = dB;
    });
    for (int ch = 0; ch < nchunk; ch++) {
      num_faces += pNFace[ch];
      num_folds += pNFold[ch];
      if (pHasJ[ch] && (first_jac || pMinJ[ch] < min_jac)) {
        min_jac = pMinJ[ch];
        first_jac = false;
      }
      dev_area += pDevA[ch];
      dev_sum += pDevS[ch];
      dev_max = std::fmax(dev_max, pDevM[ch]);
      dev_bad += pDevB[ch];
    }
  }
  stats.num_faces = num_faces;
  stats.min_jacobian = first_jac ? 0.0 : min_jac;
  stats.parametrization_folds = num_folds;
  const double rad2deg = 180.0 / PI;
  stats.field_dev_mean_deg = dev_area > 0.0 ? (dev_sum / dev_area) * rad2deg : 0.0;
  stats.field_dev_max_deg = dev_max * rad2deg;
  stats.field_dev_frac = dev_area > 0.0 ? dev_bad / dev_area : 0.0;

#ifndef WASM
  if (Lsuper) {
    cholmod_free_factor(&Lsuper, &cc);
  }
  if (Lsimp) {
    cholmod_free_factor(&Lsimp, &cc);
  }
  cholmod_finish(&cc);
  stats.convert_ms = convert_acc;
#endif

  stats.total_ms = msSince(t_total);
  return stats;
}

} // namespace sculptcore::remesh
