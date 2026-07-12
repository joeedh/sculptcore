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

#include <algorithm>
#include <cmath>
#include <complex>
#include <utility>
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

// Per-face tangent frames + the angle of an edge direction in a face's frame.
struct FieldFrames {
  Vector<float3> FX, FY, FN;

  void build(Mesh &m)
  {
    const int fcap = int(m.f.capacity());
    FX.resize(fcap);
    FY.resize(fcap);
    FN.resize(fcap);
    for (int f : m.f) {
      faceFrame(m, f, FX[f], FY[f], FN[f]);
    }
  }

  double edgeAngle(Mesh &m, int e, int f) const
  {
    float3 d = m.v.co[m.e.vs[e][1]] - m.v.co[m.e.vs[e][0]];
    return double(std::atan2(d.dot(FY[f]), d.dot(FX[f])));
  }
};

/* Sign with which a +1 period flip on interior edge e changes vertex v's pole
 * index: +1 when v's pole walk crosses e away from the canonical face (the
 * face of m.e.c[e]), −1 from the other side, 0 if v doesn't lead e. */
inline int periodSignAt(Mesh &m, int e, int v)
{
  int c1 = m.e.c[e];
  if (c1 == ELEM_NONE) {
    return 0;
  }
  if (m.c.v[c1] == v) {
    return 1;
  }
  int c2 = m.c.radial_next[c1];
  if (c2 != c1 && m.c.v[c2] == v) {
    return -1;
  }
  return 0;
}

struct PhaseSolveResult {
  int num_faces = 0;
  int num_singularities = 0;
  long index_sum = 0;
  bool solved = false;
};

/* Fixed-period smoothest-phase Poisson re-solve; optional per-edge period
 * deltas (period flips) are added to the implied periods before assembly.
 * Rewrites .remesh.f.theta / .remesh.e.period / .remesh.v.pole_index. */
PhaseSolveResult solvePhaseField(Mesh &m, double gauge_eps, const Vector<int> *edge_delta)
{
  using cd = std::complex<double>;
  PhaseSolveResult res;

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
  res.num_faces = N;
  if (N == 0) {
    return res;
  }

  FieldFrames frames;
  frames.build(m);

  BuiltinAttr<float, ".remesh.f.theta", AttrFlag::TEMP> theta;
  theta.ensure(m.f.attrs);

  // Smoothest-phase Poisson solve with the period jumps held fixed: minimize
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
    double rho = frames.edgeAngle(m, e, fb) - frames.edgeAngle(m, e, fa);
    double raw = double(theta[fb]) - double(theta[fa]) - rho;
    double p = std::round(raw / HALF_PI);
    if (edge_delta) {
      p += double((*edge_delta)[e]);
    }
    double c_e = rho + HALF_PI * p;

    trips.emplace_back(ia, ia, 1.0);
    trips.emplace_back(ib, ib, 1.0);
    trips.emplace_back(ia, ib, -1.0);
    trips.emplace_back(ib, ia, -1.0);
    d[ia] -= c_e;
    d[ib] += c_e;
  }
  for (int i = 0; i < N; i++) {
    trips.emplace_back(i, i, gauge_eps);
  }

  Eigen::SparseMatrix<double> L(N, N);
  L.setFromTriplets(trips.begin(), trips.end());
  L.makeCompressed();

  Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver;
  solver.compute(L);
  Eigen::VectorXd x = Eigen::VectorXd::Zero(N);
  if (solver.info() == Eigen::Success) {
    x = solver.solve(d);
    res.solved = true;
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
      double rho = frames.edgeAngle(m, e, fb) - frames.edgeAngle(m, e, fa);
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
      double rho = frames.edgeAngle(m, e, fb) - frames.edgeAngle(m, e, fa);
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
  res.index_sum = signed_sum;
  res.num_singularities = num_sing;
  return res;
}
} // namespace

double crossFieldCurl(Mesh &m)
{
  m.recalc_normals();

  BuiltinAttr<float, ".remesh.f.theta"> theta;
  theta.ensure(m.f.attrs);

  FieldFrames frames;
  frames.build(m);

  double sum = 0.0;
  for (int e : m.e) {
    int fa, fb;
    if (!interiorEdge(m, e, fa, fb)) {
      continue;
    }
    double rho = frames.edgeAngle(m, e, fb) - frames.edgeAngle(m, e, fa);
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
      ec = mesh::diskEdge(m.e.disk[ec][side * 2 + 1]);
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
  SingularityAdjustStats stats;

  m.recalc_normals();

  // Need an M2 field to refine; compute one with defaults if absent.
  if (!m.f.attrs.has(AttrType::FLOAT, litestl::util::string(".remesh.f.theta"))) {
    CrossFieldParams cfp;
    computeCrossField(m, cfp);
  }
  stats.curl_before = crossFieldCurl(m);

  PhaseSolveResult res = solvePhaseField(m, double(params.gauge_eps), nullptr);
  stats.num_faces = res.num_faces;
  stats.num_singularities = res.num_singularities;
  stats.index_sum = int(res.index_sum);
  if (res.num_faces == 0) {
    stats.curl_after = stats.curl_before;
    return stats;
  }

  stats.curl_after = crossFieldCurl(m);
  return stats;
}

SingularityCancelStats cancelSingularityPairs(Mesh &m,
                                              const SingularityCancelParams &params)
{
  SingularityCancelStats stats;

  // Need an M3-consistent field + pole layer; derive them if absent.
  bool have_field =
      m.f.attrs.has(AttrType::FLOAT, litestl::util::string(".remesh.f.theta")) &&
      m.v.attrs.has(AttrType::SHORT, litestl::util::string(".remesh.v.pole_index"));
  if (!have_field) {
    SingularityAdjustParams ap;
    ap.gauge_eps = params.gauge_eps;
    ap.seed = params.seed;
    adjustSingularities(m, ap);
  } else {
    m.recalc_normals();
  }

  BuiltinAttr<float, ".remesh.f.theta", AttrFlag::TEMP> theta;
  theta.ensure(m.f.attrs);
  BuiltinAttr<short, ".remesh.e.period", AttrFlag::TEMP> period;
  period.ensure(m.e.attrs);
  BuiltinAttr<short, ".remesh.v.pole_index", AttrFlag::TEMP> pole;
  pole.ensure(m.v.attrs);

  bool have_pins =
      m.v.attrs.has(AttrType::BOOL, litestl::util::string(".remesh.v.pole_pinned"));
  BuiltinAttr<bool, ".remesh.v.pole_pinned"> pinned;
  if (have_pins) {
    pinned.ensure(m.v.attrs);
  }

  auto countPoles = [&](int &nsing, long &isum) {
    nsing = 0;
    isum = 0;
    for (int v : m.v) {
      long k = long(pole[v]);
      isum += k;
      if (k != 0) {
        nsing++;
      }
    }
  };

  int cur_sing = 0;
  long cur_isum = 0;
  countPoles(cur_sing, cur_isum);
  stats.num_singularities = cur_sing;
  stats.index_sum = int(cur_isum);

  const double max_dist = double(params.max_sep) * double(params.target_edge_length);
  if (max_dist <= 0.0 || cur_sing == 0) {
    stats.curl_after = crossFieldCurl(m);
    return stats;
  }

  const int vcap = int(m.v.capacity());
  const int ecap = int(m.e.capacity());
  const int fcap = int(m.f.capacity());

  // Pair paths may only cross interior, manifold, consistently-oriented
  // regions: the pole walk is closed there and the flip sign is well-defined.
  Vector<bool> e_ok, v_ok;
  e_ok.resize(ecap);
  v_ok.resize(vcap);
  for (int i = 0; i < ecap; i++) {
    e_ok[i] = false;
  }
  for (int i = 0; i < vcap; i++) {
    v_ok[i] = false;
  }
  for (int v : m.v) {
    v_ok[v] = m.v.e[v] != ELEM_NONE;
  }
  for (int e : m.e) {
    int fa, fb;
    bool ok = interiorEdge(m, e, fa, fb);
    if (ok) {
      int c1 = m.e.c[e];
      ok = m.c.v[c1] != m.c.v[m.c.radial_next[c1]];
    }
    e_ok[e] = ok;
    if (!ok) {
      v_ok[m.e.vs[e][0]] = false;
      v_ok[m.e.vs[e][1]] = false;
    }
  }

  auto eachEdge = [&m](int v, auto &&fn) {
    int e0 = m.v.e[v];
    if (e0 == ELEM_NONE) {
      return;
    }
    int ec = e0;
    do {
      int side = m.e.vs[ec][0] == v ? 0 : 1;
      fn(ec, m.e.vs[ec][side ^ 1]);
      ec = mesh::diskEdge(m.e.disk[ec][side * 2 + 1]);
    } while (ec != e0);
  };

  // Dijkstra scratch, stamped per source so it's allocated once.
  Vector<int> vstamp, done_stamp, prev_v, prev_e, used;
  Vector<double> vdist;
  vstamp.resize(vcap);
  done_stamp.resize(vcap);
  prev_v.resize(vcap);
  prev_e.resize(vcap);
  used.resize(vcap);
  vdist.resize(vcap);
  for (int i = 0; i < vcap; i++) {
    vstamp[i] = -1;
    done_stamp[i] = -1;
    used[i] = -1;
  }
  int stamp = 0;

  struct Cand {
    double dist;
    int s, t;
    int vofs, eofs, len; // verts pool_v[vofs..vofs+len), edges pool_e[eofs..eofs+len-1)
  };
  Vector<Cand> cands;
  Vector<int> pool_v, pool_e, frontier, reached;
  Vector<int> edge_delta;
  edge_delta.resize(ecap);

  Vector<float> theta_snap;
  Vector<short> period_snap, pole_snap;
  theta_snap.resize(fcap);
  period_snap.resize(ecap);
  pole_snap.resize(vcap);

  for (int round = 0; round < params.max_rounds; round++) {
    cands.clear();
    pool_v.clear();
    pool_e.clear();

    // One bounded Dijkstra per +1 pole finds every (+1, −1) pair once.
    for (int s : m.v) {
      if (pole[s] != 1 || !v_ok[s] || (have_pins && pinned[s])) {
        continue;
      }
      stamp++;
      frontier.clear();
      reached.clear();
      vstamp[s] = stamp;
      vdist[s] = 0.0;
      prev_v[s] = -1;
      prev_e[s] = -1;
      frontier.append(s);
      while (frontier.size() > 0) {
        int best = 0;
        for (int i = 1; i < int(frontier.size()); i++) {
          int a = frontier[i], b = frontier[best];
          if (vdist[a] < vdist[b] || (vdist[a] == vdist[b] && a < b)) {
            best = i;
          }
        }
        int u = frontier[best];
        frontier[best] = frontier[int(frontier.size()) - 1];
        frontier.pop_back();
        done_stamp[u] = stamp;
        if (u != s && pole[u] == -1) {
          reached.append(u);
        }
        double du = vdist[u];
        eachEdge(u, [&](int e, int w) {
          if (!e_ok[e] || !v_ok[w] || done_stamp[w] == stamp) {
            return;
          }
          if (have_pins && pinned[w]) {
            return;
          }
          double nd =
              du + double((m.v.co[m.e.vs[e][1]] - m.v.co[m.e.vs[e][0]]).length());
          if (nd > max_dist) {
            return;
          }
          if (vstamp[w] != stamp) {
            vstamp[w] = stamp;
            frontier.append(w);
            vdist[w] = nd;
            prev_v[w] = u;
            prev_e[w] = e;
          } else if (nd < vdist[w]) {
            vdist[w] = nd;
            prev_v[w] = u;
            prev_e[w] = e;
          }
        });
      }

      for (int t : reached) {
        int vofs = int(pool_v.size());
        int eofs = int(pool_e.size());
        int len = 0;
        for (int v = t; v != -1; v = prev_v[v]) {
          pool_v.append(v);
          if (prev_e[v] != -1) {
            pool_e.append(prev_e[v]);
          }
          len++;
        }
        // Collected t→s; reverse in place to s→t.
        for (int i = 0; i < len / 2; i++) {
          std::swap(pool_v[vofs + i], pool_v[vofs + len - 1 - i]);
        }
        int elen = len - 1;
        for (int i = 0; i < elen / 2; i++) {
          std::swap(pool_e[eofs + i], pool_e[eofs + elen - 1 - i]);
        }
        Cand c;
        c.dist = vdist[t];
        c.s = s;
        c.t = t;
        c.vofs = vofs;
        c.eofs = eofs;
        c.len = len;
        cands.append(c);
      }
    }

    if (cands.size() == 0) {
      break;
    }

    std::sort(cands.data(), cands.data() + cands.size(),
              [](const Cand &a, const Cand &b) {
                if (a.dist != b.dist) {
                  return a.dist < b.dist;
                }
                if (a.s != b.s) {
                  return a.s < b.s;
                }
                return a.t < b.t;
              });

    // Greedy vertex-disjoint selection (disjoint paths ⇒ each edge flips once);
    // chain ±1 flips so every intermediate vertex's index transfer nets zero.
    for (int i = 0; i < ecap; i++) {
      edge_delta[i] = 0;
    }
    int selected = 0;
    for (const Cand &c : cands) {
      bool free_path = true;
      for (int i = 0; i < c.len; i++) {
        if (used[pool_v[c.vofs + i]] == round) {
          free_path = false;
          break;
        }
      }
      if (!free_path) {
        continue;
      }
      for (int i = 0; i < c.len; i++) {
        used[pool_v[c.vofs + i]] = round;
      }
      int delta = -int(pole[c.s]) * periodSignAt(m, pool_e[c.eofs], c.s);
      edge_delta[pool_e[c.eofs]] += delta;
      for (int i = 1; i < c.len - 1; i++) {
        int vmid = pool_v[c.vofs + i];
        int e_prev = pool_e[c.eofs + i - 1];
        int e_next = pool_e[c.eofs + i];
        delta = -periodSignAt(m, e_prev, vmid) * periodSignAt(m, e_next, vmid) * delta;
        edge_delta[e_next] += delta;
      }
      selected++;
    }

    if (selected == 0) {
      break;
    }
    stats.attempted_pairs += selected;
    stats.rounds++;

    for (int f : m.f) {
      theta_snap[f] = theta[f];
    }
    for (int e : m.e) {
      period_snap[e] = period[e];
    }
    for (int v : m.v) {
      pole_snap[v] = pole[v];
    }

    PhaseSolveResult res = solvePhaseField(m, double(params.gauge_eps), &edge_delta);

    int new_sing = 0;
    long new_isum = 0;
    countPoles(new_sing, new_isum);

    // Accept only if the index sum is conserved and the pole count strictly
    // drops; otherwise the flips moved energy somewhere worse — roll back.
    if (!res.solved || new_isum != cur_isum || new_sing >= cur_sing) {
      for (int f : m.f) {
        theta[f] = theta_snap[f];
      }
      for (int e : m.e) {
        period[e] = period_snap[e];
      }
      for (int v : m.v) {
        pole[v] = pole_snap[v];
      }
      stats.reverted_rounds++;
      break;
    }
    stats.cancelled_pairs += (cur_sing - new_sing) / 2;
    cur_sing = new_sing;
    cur_isum = new_isum;
  }

  stats.num_singularities = cur_sing;
  stats.index_sum = int(cur_isum);
  stats.curl_after = crossFieldCurl(m);
  return stats;
}

} // namespace sculptcore::remesh
