#include "remesh/extract/quad_extract.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sculptcore::remesh {

using litestl::math::float2;
using litestl::math::float3;
using litestl::util::Vector;
using sculptcore::mesh::AttrFlag;
using sculptcore::mesh::BuiltinAttr;
using sculptcore::mesh::Mesh;

namespace {

inline double dot2(float2 a, float2 b) { return double(a[0]) * b[0] + double(a[1]) * b[1]; }
inline float2 sub2(float2 a, float2 b) { return float2(a[0] - b[0], a[1] - b[1]); }
// Discrete CCW rotation by k*90 degrees (gauge/period automorphism of the lattice).
inline float2 rot2(int k, float2 p)
{
  switch (k & 3) {
  case 0: return p;
  case 1: return float2(-p[1], p[0]);
  case 2: return float2(-p[0], -p[1]);
  default: return float2(p[1], -p[0]);
  }
}
inline float3 cross3(float3 a, float3 b)
{
  return float3(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                a[0] * b[1] - a[1] * b[0]);
}
inline float3 sub3(float3 a, float3 b) { return float3(a[0] - b[0], a[1] - b[1], a[2] - b[2]); }
inline double len3(float3 a)
{
  return std::sqrt(double(a[0]) * a[0] + double(a[1]) * a[1] + double(a[2]) * a[2]);
}
inline float3 norm3(float3 a)
{
  double l = len3(a);
  if (l < 1e-30) return float3(0, 0, 0);
  return float3(float(a[0] / l), float(a[1] / l), float(a[2] / l));
}
inline float3 bary3(float3 a, float3 b, float3 c, double u, double v, double w)
{
  return float3(float(double(a[0]) * u + double(b[0]) * v + double(c[0]) * w),
                float(double(a[1]) * u + double(b[1]) * v + double(c[1]) * w),
                float(double(a[2]) * u + double(b[2]) * v + double(c[2]) * w));
}
inline bool baryOf(const float2 P[3], float2 q, double &u, double &v, double &w)
{
  float2 v0 = sub2(P[1], P[0]), v1 = sub2(P[2], P[0]), v2 = sub2(q, P[0]);
  double d00 = dot2(v0, v0), d01 = dot2(v0, v1), d11 = dot2(v1, v1);
  double d20 = dot2(v2, v0), d21 = dot2(v2, v1);
  double den = d00 * d11 - d01 * d01;
  if (std::fabs(den) < 1e-20) { u = 1; v = 0; w = 0; return false; }
  v = (d11 * d20 - d01 * d21) / den;
  w = (d00 * d21 - d01 * d20) / den;
  u = 1.0 - v - w;
  return true;
}

inline bool nearInt(double x, double eps) { return std::fabs(x - std::floor(x + 0.5)) < eps; }

} // namespace

mesh::Mesh *extractQuadMesh(Mesh &m, const ExtractParams &params, ExtractStats &stats)
{
  m.thawTopo();
  if (m.f.count == 0) return nullptr;

  BuiltinAttr<float2, ".remesh.c.uv", AttrFlag::TEMP> uv;
  uv.ensure(m.c.attrs);

  // Re-gauge to the globally-coherent chart. The stored uv is un-gauged (each
  // face rotated back by -R_f*90), so on a curved, non-trivial-topology surface
  // the per-face charts spiral and the uv range collapses. Rotating each corner
  // by +R_f*90 recovers the gauged class variable, which is identical across
  // every non-cut edge -> one coherent chart over the cut-open disk, so lattice
  // enumeration and a 3D-position weld stay robust.
  BuiltinAttr<short, ".remesh.f.gauge_rot", AttrFlag::TEMP> gauge_rot;
  gauge_rot.ensure(m.f.attrs);
  Vector<float2> guv;
  guv.resize(int(m.c.capacity()));
  for (int f : m.f) {
    int R = ((int(gauge_rot[f]) % 4) + 4) % 4;
    int c0 = m.l.c[m.f.l[f]], cc = c0;
    do {
      guv[cc] = rot2(R, uv[cc]);
      cc = m.c.next[cc];
    } while (cc != c0);
  }

  // ---- bounding box → weld tolerance + hash cell size ----
  float3 bbmin = m.v.co[*m.v.begin()], bbmax = bbmin;
  for (int v : m.v) {
    float3 p = m.v.co[v];
    for (int i = 0; i < 3; i++) {
      bbmin[i] = std::min(bbmin[i], p[i]);
      bbmax[i] = std::max(bbmax[i], p[i]);
    }
  }
  double diag = len3(sub3(bbmax, bbmin));
  double tol = params.weld_tol * (diag > 0 ? diag : 1.0);
  if (tol <= 0) tol = 1e-7;
  double invcell = 1.0 / (2.0 * tol);

  // ---- per-triangle accessor (each face's own gauged chart, no perturbation:
  // shared-edge crossings then agree to solve precision and weld in 3D) ----
  auto getTri = [&](int f, int c[3], float2 P[3], float3 X[3]) {
    int li = m.f.l[f];
    c[0] = m.l.c[li];
    c[1] = m.c.next[c[0]];
    c[2] = m.c.next[c[1]];
    for (int i = 0; i < 3; i++) {
      P[i] = guv[c[i]];
      X[i] = m.v.co[m.c.v[c[i]]];
    }
  };

  // ---- node pool: integer lattice points (grid vertices) and iso-line/edge
  // crossing "ports" (contracted away later), welded by 3D position. A node is
  // `lattice` iff both its (u,v) are integer. weld accumulates the incident face
  // normal (rotation-system basis) and OR-s the lattice flag on a hit. ----
  std::unordered_map<int64_t, std::vector<int>> hash;
  Vector<float3> gvpos;
  Vector<char> nodeLat;
  Vector<float3> gvnrm;
  auto keyOf = [](int a, int b, int c) -> int64_t {
    return (int64_t(a) * 73856093) ^ (int64_t(b) * 19349663) ^ (int64_t(c) * 83492791);
  };
  auto weld = [&](float3 p, bool lattice, float3 fn) -> int {
    int ix = int(std::floor(p[0] * invcell));
    int iy = int(std::floor(p[1] * invcell));
    int iz = int(std::floor(p[2] * invcell));
    for (int dx = -1; dx <= 1; dx++)
      for (int dy = -1; dy <= 1; dy++)
        for (int dz = -1; dz <= 1; dz++) {
          auto it = hash.find(keyOf(ix + dx, iy + dy, iz + dz));
          if (it == hash.end()) continue;
          for (int gv : it->second)
            if (len3(sub3(gvpos[gv], p)) <= tol) {
              if (lattice) nodeLat[gv] = 1;
              gvnrm[gv] = float3(gvnrm[gv][0] + fn[0], gvnrm[gv][1] + fn[1],
                                 gvnrm[gv][2] + fn[2]);
              return gv;
            }
        }
    int id = int(gvpos.size());
    gvpos.append(p);
    nodeLat.append(lattice ? 1 : 0);
    gvnrm.append(fn);
    hash[keyOf(ix, iy, iz)].push_back(id);
    return id;
  };

  // ---- per-triangle grid-line clip → nodes + segments + edge-port incidences ----
  struct Seg {
    int a, b;
  };
  Vector<Seg> segs;
  struct BInc {
    int e, f;
    double t;
    int node;
  };
  Vector<BInc> bincs;
  // guv is stored as float2; its ulp at the larger |uv| magnitudes (~1e2) is
  // ~2e-5, so integer tests must sit above float precision, not at 1e-6.
  const double epsLine = 1e-4; // |corner - grid line| treated as on the line
  const double epsLat = 1e-4;  // |x - round(x)| treated as integer
  const double epsSeg = 1e-9;

  // Clip the single grid line {coord[axis]==val} to triangle f, emit its nodes
  // (two boundary endpoints + interior integer lattice points) joined in order.
  // Endpoints strictly interior to a mesh edge are recorded for the cut stitch.
  auto processLine = [&](int f, const int c[3], const float2 P[3], const float3 X[3],
                         float3 fn, int axis, int val) {
    int fixc = axis, frec = 1 - axis;
    double fd[3];
    for (int k = 0; k < 3; k++) fd[k] = double(P[k][fixc]) - val;
    double crF[4]; // free coord of a boundary meeting point
    int crE[4];    // triangle-edge (corner) index, -1 at a vertex
    double crT[4]; // parameter along that edge
    int ncr = 0;
    bool edgeOn = false;
    int eolE = -1;
    for (int e = 0; e < 3; e++) {
      int a = e, b = (e + 1) % 3;
      double fa = fd[a], fb = fd[b];
      bool aon = std::fabs(fa) < epsLine, bon = std::fabs(fb) < epsLine;
      if (aon && bon) { edgeOn = true; eolE = e; }
      else if (aon) { crF[ncr] = P[a][frec]; crE[ncr] = -1; crT[ncr] = 0; ncr++; }
      else if (bon) { /* counted as the next edge's `aon` */ }
      else if (fa * fb < 0) {
        double te = fa / (fa - fb);
        crF[ncr] = P[a][frec] + te * (P[b][frec] - P[a][frec]);
        crE[ncr] = e;
        crT[ncr] = te;
        ncr++;
      }
    }
    double loF, hiF, loT, hiT;
    int loE, hiE;
    if (edgeOn) {
      int a = eolE, b = (eolE + 1) % 3;
      loF = std::min<double>(P[a][frec], P[b][frec]);
      hiF = std::max<double>(P[a][frec], P[b][frec]);
      loE = hiE = -1;
      loT = hiT = 0;
    } else {
      if (ncr != 2) return;
      int lo = crF[0] <= crF[1] ? 0 : 1, hi = 1 - lo;
      loF = crF[lo]; loE = crE[lo]; loT = crT[lo];
      hiF = crF[hi]; hiE = crE[hi]; hiT = crT[hi];
      if (hiF - loF < epsSeg) return;
    }
    struct Nd {
      double fr, t;
      int edge;
      bool endpoint;
    };
    Nd nds[256];
    int nn = 0;
    nds[nn++] = Nd{loF, loT, loE, true};
    int j0 = int(std::ceil(loF - epsLat)), j1 = int(std::floor(hiF + epsLat));
    for (int j = j0; j <= j1 && nn < 255; j++) {
      double fr = double(j);
      if (fr <= loF + epsLat || fr >= hiF - epsLat) continue; // already an endpoint
      nds[nn++] = Nd{fr, 0, -1, false};
    }
    nds[nn++] = Nd{hiF, hiT, hiE, true};
    int prev = -1;
    for (int t = 0; t < nn; t++) {
      float2 q = axis == 0 ? float2(float(val), float(nds[t].fr))
                           : float2(float(nds[t].fr), float(val));
      double bu, bv, bw;
      if (!baryOf(P, q, bu, bv, bw)) { prev = -1; continue; }
      float3 pos = bary3(X[0], X[1], X[2], bu, bv, bw);
      int id = weld(pos, nearInt(nds[t].fr, epsLat), fn);
      if (nds[t].endpoint && nds[t].edge >= 0) {
        int e = m.c.e[c[nds[t].edge]];
        int va = m.c.v[c[nds[t].edge]];
        double tc = nds[t].t;
        if (m.e.vs[e][0] != va) tc = 1.0 - tc; // canonical: e.vs[0] -> e.vs[1]
        if (tc > 1e-4 && tc < 1.0 - 1e-4) bincs.append(BInc{e, f, tc, id});
      }
      if (prev >= 0 && prev != id) segs.append(Seg{prev, id});
      prev = id;
    }
  };

  // Global feasibility gate (plan: gate M5 behind a feasibility check). A
  // trustworthy integer-grid map folds only in the singularity 1-rings — a few
  // percent of faces. A high fold fraction means the seamless/quantize solve
  // tangled (typically a high-resolution input against a fine target); rasterizing
  // it produces runaway, non-manifold output, so bail cleanly and let the caller
  // retry with a coarser target rather than emit garbage.
  {
    const double kMaxFoldFraction = 0.10;
    int nfold = 0, ntot = 0;
    for (int f : m.f) {
      int c[3];
      float2 P[3];
      float3 X[3];
      getTri(f, c, P, X);
      double a = (double(P[1][0]) - P[0][0]) * (double(P[2][1]) - P[0][1]) -
                 (double(P[2][0]) - P[0][0]) * (double(P[1][1]) - P[0][1]);
      ntot++;
      if (a <= 1e-9) nfold++;
    }
    if (ntot > 0 && double(nfold) > kMaxFoldFraction * double(ntot)) {
      stats.ok = false;
      return nullptr;
    }
  }

  for (int f : m.f) {
    int c[3];
    float2 P[3];
    float3 X[3];
    getTri(f, c, P, X);
    // Skip folded (inverted-uv) faces: M4 leaves the chart non-injective in
    // singularity 1-rings, so clipping them yields garbage; the even-gon holes
    // this leaves around each cone are closed by the cap pass below.
    {
      double a = (double(P[1][0]) - P[0][0]) * (double(P[2][1]) - P[0][1]) -
                 (double(P[2][0]) - P[0][0]) * (double(P[1][1]) - P[0][1]);
      if (a <= 1e-9) continue;
    }
    float3 fn = cross3(sub3(X[1], X[0]), sub3(X[2], X[0]));
    double umin = std::min({P[0][0], P[1][0], P[2][0]});
    double umax = std::max({P[0][0], P[1][0], P[2][0]});
    double vmin = std::min({P[0][1], P[1][1], P[2][1]});
    double vmax = std::max({P[0][1], P[1][1], P[2][1]});
    // Span guard: a well-scaled triangle spans O(1) integer cells (|grad u|~1/t,
    // physical size ~t). An over-scaled face (huge but positive uv area, e.g. a
    // cone branch point the negative-area fold test misses) would rasterize across
    // thousands of grid lines and explode the output; skip it (the cap pass closes
    // the hole). Backstop against upstream over-scale, not a correctness fix.
    const int kMaxSpan = 64;
    if (umax - umin > kMaxSpan || vmax - vmin > kMaxSpan)
      continue;
    for (int i = int(std::ceil(umin - epsLat)); i <= int(std::floor(umax + epsLat)); i++)
      processLine(f, c, P, X, fn, 0, i);
    for (int j = int(std::ceil(vmin - epsLat)); j <= int(std::floor(vmax + epsLat)); j++)
      processLine(f, c, P, X, fn, 1, j);
  }

  int NG = int(gvpos.size());
  if (NG == 0) return nullptr;

  // ---- union duplicate ports on each interior mesh edge (cut-edge stitch) ----
  // Non-cut edges already 3D-welded (identity transition); cut edges differ by the
  // realized non-integer transition, so their two faces' ports land a residual
  // apart. Pair the two faces' ports by parameter order along the edge and union —
  // a single-edge, accumulation-free stitch that survives folds near singularities.
  Vector<int> uf;
  uf.resize(NG);
  for (int i = 0; i < NG; i++) uf[i] = i;
  auto find = [&](int a) {
    while (uf[a] != a) { uf[a] = uf[uf[a]]; a = uf[a]; }
    return a;
  };
  auto uni = [&](int a, int b) {
    a = find(a);
    b = find(b);
    if (a != b) uf[a] = b;
  };
  {
    std::unordered_map<int, std::vector<int>> edgeBincs;
    for (int i = 0; i < int(bincs.size()); i++) edgeBincs[bincs[i].e].push_back(i);
    auto byT = [&](int x, int y) { return bincs[x].t < bincs[y].t; };
    for (auto &kv : edgeBincs) {
      auto &list = kv.second;
      int fA = -1, fB = -1;
      for (int idx : list) {
        int f = bincs[idx].f;
        if (fA < 0 || f == fA) fA = f;
        else if (fB < 0) fB = f;
      }
      if (fB < 0) continue; // boundary edge: a single face, nothing to stitch
      std::vector<int> la, lb;
      for (int idx : list) (bincs[idx].f == fA ? la : lb).push_back(idx);
      std::sort(la.begin(), la.end(), byT);
      std::sort(lb.begin(), lb.end(), byT);
      if (la.size() == lb.size()) {
        for (size_t k = 0; k < la.size(); k++)
          uni(bincs[la[k]].node, bincs[lb[k]].node);
      } else {
        std::vector<int> &s = la.size() < lb.size() ? la : lb;
        std::vector<int> &big = la.size() < lb.size() ? lb : la;
        for (int x : s) {
          double bt = 1e300;
          int by = -1;
          for (int y : big) {
            double dd = std::fabs(bincs[y].t - bincs[x].t);
            if (dd < bt) { bt = dd; by = y; }
          }
          if (by >= 0) uni(bincs[x].node, bincs[by].node);
        }
      }
    }
  }

  // Collapse the unioned lattice flag + normal onto roots.
  for (int i = 0; i < NG; i++) {
    int r = find(i);
    if (r == i) continue;
    if (nodeLat[i]) nodeLat[r] = 1;
    gvnrm[r] = float3(gvnrm[r][0] + gvnrm[i][0], gvnrm[r][1] + gvnrm[i][1],
                      gvnrm[r][2] + gvnrm[i][2]);
  }
  int latCount = 0;
  for (int i = 0; i < NG; i++)
    if (find(i) == i && nodeLat[i]) latCount++;
  stats.num_grid_verts = latCount;

  // ---- adjacency over union roots ----
  Vector<Vector<int>> adj;
  adj.resize(NG);
  for (int s = 0; s < int(segs.size()); s++) {
    int a = find(segs[s].a), b = find(segs[s].b);
    if (a == b) continue;
    bool dup = false;
    for (int x : adj[a])
      if (x == b) { dup = true; break; }
    if (dup) continue;
    adj[a].append(b);
    adj[b].append(a);
  }

  // ---- contract port chains into grid arcs (lattice root -> lattice root) ----
  struct Arc {
    int g, h;
    float3 tan;
  };
  Vector<Arc> arcs;
  Vector<Vector<int>> gvArcs;
  gvArcs.resize(NG);
  auto addArc = [&](int g, int h, float3 tan) {
    for (int ai : gvArcs[g]) {
      if (arcs[ai].h != h) continue;
      float3 t2 = arcs[ai].tan;
      double dp = double(t2[0]) * tan[0] + double(t2[1]) * tan[1] + double(t2[2]) * tan[2];
      double l1 = len3(tan), l2 = len3(t2);
      if (l1 > 0 && l2 > 0 && dp / (l1 * l2) > 0.9) return; // duplicate ray
    }
    int id = int(arcs.size());
    arcs.append(Arc{g, h, tan});
    gvArcs[g].append(id);
  };

  for (int g = 0; g < NG; g++) {
    if (find(g) != g || !nodeLat[g]) continue;
    for (int n0 : adj[g]) {
      int prev = g, cur = n0, guard = 0;
      while (!nodeLat[cur]) {
        if (int(adj[cur].size()) != 2) { cur = -1; break; }
        int nx = adj[cur][0] == prev ? adj[cur][1] : adj[cur][0];
        prev = cur;
        cur = nx;
        if (++guard > 100000) { cur = -1; break; }
      }
      if (cur < 0) { stats.open_arcs++; continue; }
      if (cur == g) continue;
      addArc(g, cur, norm3(sub3(gvpos[n0], gvpos[g])));
    }
  }
  stats.num_arcs = int(arcs.size());

  // (per-grid-vertex normals were accumulated during welding and collapsed onto
  // union roots, above.)

  // ---- sort each gv's arcs CCW in its tangent plane (rotation system) ----
  for (int g = 0; g < NG; g++) {
    auto &al = gvArcs[g];
    if (al.size() < 2) continue;
    float3 N = norm3(gvnrm[g]);
    float3 t0 = arcs[al[0]].tan;
    // Project the first incident tangent into the gv tangent plane for the basis.
    double tn = double(t0[0]) * N[0] + double(t0[1]) * N[1] + double(t0[2]) * N[2];
    float3 T = norm3(float3(float(t0[0] - tn * N[0]), float(t0[1] - tn * N[1]),
                            float(t0[2] - tn * N[2])));
    if (len3(T) < 1e-12) T = norm3(cross3(N, float3(1, 0, 0)));
    float3 B = norm3(cross3(N, T));
    std::vector<std::pair<double, int>> order;
    order.reserve(al.size());
    for (int ai : al) {
      float3 t = arcs[ai].tan;
      double x = double(t[0]) * T[0] + double(t[1]) * T[1] + double(t[2]) * T[2];
      double y = double(t[0]) * B[0] + double(t[1]) * B[1] + double(t[2]) * B[2];
      order.push_back({std::atan2(y, x), ai});
    }
    std::sort(order.begin(), order.end(),
              [](const std::pair<double, int> &a, const std::pair<double, int> &b) {
                return a.first < b.first;
              });
    for (int k = 0; k < int(al.size()); k++) al[k] = order[k].second;
  }

  // ---- twin + position-in-ring ----
  Vector<int> posInRing;
  posInRing.resize(int(arcs.size()));
  for (int g = 0; g < NG; g++)
    for (int p = 0; p < int(gvArcs[g].size()); p++) posInRing[gvArcs[g][p]] = p;
  Vector<int> twin;
  twin.resize(int(arcs.size()));
  for (int a = 0; a < int(arcs.size()); a++) {
    twin[a] = -1;
    int g = arcs[a].g, h = arcs[a].h;
    for (int b : gvArcs[h])
      if (arcs[b].h == g) { twin[a] = b; break; }
  }

  auto nextHE = [&](int a) -> int {
    int t = twin[a];
    if (t < 0) return -1;
    auto &ring = gvArcs[arcs[a].h];
    return ring[(posInRing[t] + 1) % int(ring.size())];
  };

  // ---- walk the rotation system → one quad per grid cell ----
  Mesh *out = litestl::alloc::New<Mesh>("Mesh QuadExtract");
  Vector<int> gvOut;
  gvOut.resize(NG);
  for (int g = 0; g < NG; g++) gvOut[g] = -1;
  Vector<char> used;
  used.resize(int(arcs.size()));
  for (int a = 0; a < int(arcs.size()); a++) used[a] = 0;

  for (int a0 = 0; a0 < int(arcs.size()); a0++) {
    if (used[a0]) continue;
    int seq[8];
    int n = 0, a = a0;
    bool ok = true;
    while (n < 8) {
      used[a] = 1;
      seq[n++] = arcs[a].g;
      int nx = nextHE(a);
      if (nx < 0) { ok = false; break; }
      if (nx == a0) break;
      a = nx;
    }
    if (!ok || n != 4) { stats.nonquad_cells++; continue; }
    Vector<int> vs;
    for (int t = 0; t < 4; t++) {
      int g = seq[t];
      if (gvOut[g] < 0) gvOut[g] = out->make_vertex(gvpos[g]);
      vs.append(gvOut[g]);
    }
    out->make_face(vs);
    stats.num_quads++;
  }

  out->recalc_normals();

  // ---- close holes: cap each even boundary loop with a quad fan ----
  // Skipping the folded (degenerate) face 1-rings leaves a clean even ring at each
  // unresolved cone / over-scaled cap; close it with a fan of quads so the result
  // stays watertight and all-quad. A closed input has no legitimate boundary, so
  // every output loop is a spurious hole and is capped (cone center if one is
  // enclosed, else the loop centroid); an open input keeps loops that don't
  // encircle a cone, preserving its real borders (grid edge, open tube end).
  {
    Mesh &o = *out;
    auto isBndIn = [&](int e) {
      int c0 = m.e.c[e];
      if (c0 == ELEM_NONE) return true;
      int r = 0, cc = c0;
      do { r++; cc = m.c.radial_next[cc]; } while (cc != c0 && r < 100);
      return r == 1;
    };
    bool inputClosed = true;
    for (int e : m.e)
      if (isBndIn(e)) { inputClosed = false; break; }
    auto isBnd = [&](int e) {
      int c0 = o.e.c[e];
      if (c0 == ELEM_NONE) return true;
      int r = 0, cc = c0;
      do { r++; cc = o.c.radial_next[cc]; } while (cc != c0 && r < 100);
      return r == 1;
    };
    Vector<float3> singPos; // cone vertices = candidate cap centers
    if (m.v.attrs.has(mesh::AttrType::SHORT, litestl::util::string(".remesh.v.pole_index"))) {
      BuiltinAttr<short, ".remesh.v.pole_index"> pole;
      pole.ensure(m.v.attrs);
      for (int sv : m.v)
        if (pole[sv] != 0) singPos.append(m.v.co[sv]);
    }
    Vector<char> seen;
    seen.resize(int(o.e.capacity()));
    for (int i = 0; i < int(o.e.capacity()); i++) seen[i] = 0;
    // The next rim edge after `e` at vertex `v`, found by pivoting through v's
    // face fan from e's wedge (cross interior edges radially until the wedge's
    // far boundary edge). Disk-order "first boundary edge" is ambiguous at a
    // pinch vertex shared by two rims and can jump rims mid-trace.
    auto nextRim = [&](int e, int v) -> int {
      int cc = o.e.c[e]; // boundary edge: its single incident corner
      if (cc == ELEM_NONE) return -1;
      for (int guard = 0; guard < 100; guard++) {
        // The face's other edge at v (one of the two corner-edges at v is the
        // edge we arrived through).
        int ca = cc, fguard = 0;
        while (o.c.v[ca] != v && ++fguard < 100) ca = o.c.next[ca];
        if (fguard >= 100) return -1;
        int cb = o.c.prev[ca];
        int cnext = o.c.e[ca] == e ? cb : ca;
        int cand = o.c.e[cnext];
        if (isBnd(cand)) return cand;
        cc = o.c.radial_next[cnext]; // cross into the adjacent face, keep pivoting
        e = cand;
      }
      return -1;
    };
    // Collect ordered boundary loops first (capping mutates the mesh topology).
    Vector<Vector<int>> loops;
    for (int e0 : o.e) {
      if (seen[e0] || !isBnd(e0)) continue;
      Vector<int> loop;
      int e = e0, v = o.e.vs[e0][1], guard = 0;
      do {
        seen[e] = 1;
        loop.append(v);
        int nxt = nextRim(e, v);
        if (nxt < 0) { loop.clear(); break; }
        e = nxt;
        v = o.e.vs[e][0] == v ? o.e.vs[e][1] : o.e.vs[e][0];
      } while (e != e0 && ++guard < 100000);
      if (e != e0) loop.clear(); // unclosed trace: not a cappable rim
      if (loop.size()) {
        loops.append(loop);
      } else {
        stats.holes_open++;
        stats.holes_open_untraced++;
      }
    }
    // A real hole rim (cone 1-ring / over-scaled cap) is small; a loop in the
    // thousands is a non-manifold artifact of a surviving fold tangle. Fanning it
    // would emit ~n/2 overlapping garbage quads, so leave it open (a hole) instead.
    const int kMaxCapLoop = 2048;
    for (int li = 0; li < int(loops.size()); li++) {
      Vector<int> &loop = loops[li];
      int n = int(loop.size());
      // Center-fan cap: an even rim closes with pure quads. An odd rim is
      // unquadable, so it needs one trailing triangle — only do that when
      // cap_odd_holes is set (else leave it open to keep the all-quad contract).
      if (n < 4 || n > kMaxCapLoop) {
        stats.holes_open++;
        stats.holes_open_size++;
        continue;
      }
      if ((n & 1) && !params.cap_odd_holes) {
        stats.holes_open++;
        stats.holes_open_odd++;
        continue;
      }
      // A rim that visits a vertex twice is pinched; a fan over it would make the
      // pinch (and its center spokes) non-manifold. Leave it open instead.
      {
        bool pinched = false;
        for (int i = 0; i < n && !pinched; i++)
          for (int j = i + 1; j < n; j++)
            if (loop[i] == loop[j]) { pinched = true; break; }
        if (pinched) {
          stats.holes_open++;
          stats.holes_open_pinched++;
          continue;
        }
      }
      float3 cen(0, 0, 0);
      for (int v : loop)
        cen = float3(cen[0] + o.v.co[v][0], cen[1] + o.v.co[v][1], cen[2] + o.v.co[v][2]);
      cen = float3(cen[0] / n, cen[1] / n, cen[2] / n);
      double radius = 0;
      for (int v : loop) radius += len3(sub3(o.v.co[v], cen));
      radius /= n;
      // Cap centered on the enclosed cone; on a closed input a loop that encircles
      // no cone is still a spurious hole, so cap it at its own centroid instead.
      float3 cpos = cen;
      double bd = 1e30;
      for (int s = 0; s < int(singPos.size()); s++) {
        double d = len3(sub3(singPos[s], cen));
        if (d < bd) { bd = d; cpos = singPos[s]; }
      }
      if (bd > radius) {
        if (!inputClosed) { // open-input border: leave it open
          stats.holes_open++;
          stats.holes_open_border++;
          continue;
        }
        cpos = cen; // closed-input hole: cap at the centroid
      }
      int C = o.make_vertex(cpos);
      for (int i = 0; i < n; i += 2) {
        Vector<int> q;
        q.append(C);
        if (i + 2 <= n) {
          q.append(loop[(i + 2) % n]); // spans two rim edges -> quad
        }
        q.append(loop[(i + 1) % n]); // odd remainder spans one edge -> triangle
        q.append(loop[i]);
        o.make_face(q);
      }
      stats.holes_capped++;
      if (n & 1) stats.holes_capped_odd++;
    }
  }

  out->recalc_normals();

  // Backstop: a trustworthy grid map welds to a quad mesh with F ~ V (Euler).
  // If anything above still produced F >> V the map tangled beyond repair; bail
  // cleanly so the caller retries coarser rather than handing back garbage.
  if (out->v.count > 0 && double(out->f.count) > 4.0 * double(out->v.count)) {
    stats.ok = false;
    litestl::alloc::Delete<Mesh>(out);
    return nullptr;
  }

  stats.ok = stats.num_quads > 0;
  return out;
}

} // namespace sculptcore::remesh
