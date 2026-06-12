#pragma once

#include "../mesh.h"
#include "litestl/math/geom.h"
#include "litestl/util/vector.h"

#include <cfloat>
#include <cmath>
#include <string>

namespace sculptcore::mesh {

/* Structured health report for a mesh, shared by the remesh tests and the
 * `remesh_validate` debug verb. Topology-level fields (census, manifold, Euler,
 * winding, inversions, valence) are computed by remeshValidate(); the
 * parametrization-dependent fields (isoline closure, T-junctions) carry a
 * `*_checked` flag and are filled by the M6 extraction stage once (u,v) exist.
 * See documentation/plans/quad-remeshing.md. */
struct RemeshReport {
  // Face-type census.
  int face_count = 0;
  int tri_count = 0;
  int quad_count = 0;
  int ngon_count = 0;
  bool all_quad = false;

  // Cycle / manifold integrity (disk, radial, loop cycles + corner agreement),
  // via checkTopology.
  bool manifold = false;
  std::string manifold_error;

  // Euler characteristic from the live topology (V - E + F = 2 - 2g closed).
  int vert_count = 0;
  int edge_count = 0;
  int euler = 0;

  // Orientability: edges shared by != 2 faces, and faces whose winding opposes
  // their neighbour across a shared edge.
  int non_manifold_edges = 0;
  int boundary_edges = 0;
  bool consistent_winding = false;

  // Geometry health (the M6 reprojection-drift guards).
  int degenerate_faces = 0; // ~zero-area
  int inverted_faces = 0;   // Newell normal opposes the incident vertex normals
  // Tier-0d interior-edge geometric folds (countGeometricFolds): fold90 =
  // adjacent unit face normals dot < 0 (pathological crease); fold180 = dot
  // < -0.95 (true fold-back; a subset of fold90).
  int fold90_edges = 0;
  int fold180_edges = 0;

  // Valence / singularity inventory (meaningful on all-quad output).
  int irregular_interior_verts = 0; // interior verts with valence != 4
  int valence_hist[17] = {0};       // index = min(valence, 16)

  // --- Tier-0 extended quality metrics (plans/quad-remeshing-filtering.md) ---
  // valence_hist[] / irregular_interior_verts count interior verts, but %regular
  // on an OPEN character mesh needs the interior denominator, not total verts:
  //   regular_interior_frac = 1 - irregular_interior_verts / interior_vert_count.
  int interior_vert_count = 0;        // non-boundary verts (the %regular denominator)
  float regular_interior_frac = 1.0f; // 1.0 when there are no interior verts

  // Connectivity (definitions fixed so cross-run comparisons are meaningful):
  //  component_count     = connected components over the FACE-adjacency graph
  //                        (two faces adjacent iff they share an edge; verts with
  //                        no incident face are ignored).
  //  boundary_loop_count = number of boundary LOOPS ("holes") = connected
  //                        components of the boundary-edge graph (manifold edges
  //                        with exactly one incident face). NOT genus handles and
  //                        NOT the M6 output residual cap loops.
  int component_count = 0;
  int boundary_loop_count = 0;
  // Worst single component's irregular-interior count (per-component singularity
  // clustering). 0 on a regular or empty mesh.
  int max_component_irregular = 0;

  // Quad-shape / size-transition geometry (computed for any face size). The two
  // "ratios" are >= 1 (max/min over an edge-shared face pair); 1.0 = uniform.
  float max_adjacent_area_ratio = 1.0f; // worst neighbour-quad area ratio
  float max_adjacent_edge_ratio = 1.0f; // worst neighbour mean-edge-length ratio
  // Skinny-quad proxy: smallest interior corner angle over all faces (radians),
  // plus a histogram of each face's MIN corner angle in 10-degree bins over
  // [0,90) (bin 8 also catches >= 80; a quad's min angle is <= 90 by angle-sum).
  float min_interior_angle = 0.0f;
  int min_angle_hist[9] = {0};

  // Pre-extraction parametrization fold count (QuantizeStats::parametrization_
  // folds): folded faces in the seamless (u,v) on the SOLVE mesh, distinct from
  // the output mesh's inverted_faces. remeshValidate cannot derive it (the (u,v)
  // lives on the solve mesh, not the output), so the caller sets it. -1 = unset.
  int parametrization_folds = -1;

  // Tier 6.7 boundary preservation (boundaryDeviation): this mesh's rim verts'
  // distance to the INPUT boundary polyline. Needs both meshes, so the caller
  // fills it; -1 = unset (no boundary on one side, or not computed).
  float boundary_dev_mean = -1.0f;
  float boundary_dev_max = -1.0f;

  // M6 no-spiral guarantee — edge-strip (isoline) closure on the all-quad
  // output. Filled by checkIsolineClosure (auto-run from remeshValidate when the
  // mesh is all-quad + manifold); isolines_checked stays false otherwise.
  bool isolines_checked = false;
  bool isolines_close = false; // every isoline is a closed loop (no open/spiral)
  int closed_isolines = 0;     // strips that closed into a loop
  int open_isolines = 0;       // strips that ran to a boundary (legit on open meshes)
  int spiral_isolines = 0;     // strips that blew the step budget = corruption/spiral
  bool tjunctions_checked = false;
  int tjunction_count = 0;

  /* Structural health independent of parametrization: a sane mesh to hand the
   * pipeline, and a sane mesh out of extraction (minus the all-quad / isoline
   * fields, which callers assert explicitly). */
  bool structurallyOk() const
  {
    return manifold && consistent_winding && non_manifold_edges == 0 &&
           degenerate_faces == 0 && inverted_faces == 0;
  }
};

/* Cycle-integrity / manifold check (promoted from the debug-local checkManifold
 * and generalized to any face size). Verifies disk + radial + loop cycles
 * close, corner edge/vert agree, and edges reference valid distinct verts.
 * `requireTriangles` additionally asserts every face is a triangle (the dyntopo
 * consumers' precondition). Fills `err` on the first problem found. */
static inline bool checkTopology(Mesh &m, std::string &err,
                                 bool requireTriangles = false)
{
  char buf[160];
  for (int ei : m.e) {
    int v1 = m.e.vs[ei][0], v2 = m.e.vs[ei][1];
    if (v1 < 0 || v2 < 0 || v1 >= int(m.v.capacity()) ||
        v2 >= int(m.v.capacity()) || m.v.freemap[v1] || m.v.freemap[v2] ||
        v1 == v2) {
      snprintf(buf, sizeof(buf), "edge %d bad/degenerate vert refs %d %d", ei,
               v1, v2);
      err = buf;
      return false;
    }
  }
  for (int vi : m.v) {
    int e0 = m.v.e[vi];
    if (e0 == ELEM_NONE)
      continue;
    int steps = 0, ec = e0;
    do {
      int side = m.e.vs[ec][0] == vi ? 0 : 1;
      int next = m.e.disk[ec][side * 2 + 1], prev = m.e.disk[ec][side * 2];
      int sn = m.e.vs[next][0] == vi ? 0 : 1, sp = m.e.vs[prev][0] == vi ? 0 : 1;
      if (m.e.disk[next][sn * 2] != ec || m.e.disk[prev][sp * 2 + 1] != ec) {
        snprintf(buf, sizeof(buf), "disk prev/next mismatch v=%d e=%d", vi, ec);
        err = buf;
        return false;
      }
      ec = next;
      if (++steps > 4000000) {
        err = "vert disk did not close";
        return false;
      }
    } while (ec != e0);
  }
  for (int ei : m.e) {
    int c0 = m.e.c[ei];
    if (c0 == ELEM_NONE)
      continue;
    int steps = 0, cc = c0;
    do {
      if (m.c.e[cc] != ei) {
        err = "radial corner c.e mismatch";
        return false;
      }
      int rn = m.c.radial_next[cc], rp = m.c.radial_prev[cc];
      if (m.c.radial_prev[rn] != cc || m.c.radial_next[rp] != cc) {
        snprintf(buf, sizeof(buf), "radial prev/next mismatch e=%d c=%d", ei,
                 cc);
        err = buf;
        return false;
      }
      cc = rn;
      if (++steps > 4000000) {
        err = "edge radial did not close";
        return false;
      }
    } while (cc != c0);
  }
  for (int fi : m.f) {
    if (m.f.list_count[fi] != 1) {
      snprintf(buf, sizeof(buf), "face %d has %d loops (multi-loop unsupported)",
               fi, int(m.f.list_count[fi]));
      err = buf;
      return false;
    }
    int sz = m.l.size[m.f.l[fi]];
    if (sz < 3 || (requireTriangles && sz != 3)) {
      snprintf(buf, sizeof(buf), "face %d has %d sides%s", fi, sz,
               requireTriangles ? " (expected triangle)" : "");
      err = buf;
      return false;
    }
    int li = m.f.l[fi], c0 = m.l.c[li], cc = c0, n = 0;
    do {
      if (m.c.l[cc] != li) {
        err = "corner c.l != list";
        return false;
      }
      int cn = m.c.next[cc];
      if (m.c.prev[cn] != cc) {
        err = "corner prev/next mismatch";
        return false;
      }
      int ce = m.c.e[cc], vh = m.c.v[cc], vn = m.c.v[cn];
      int ev0 = m.e.vs[ce][0], ev1 = m.e.vs[ce][1];
      if (!((ev0 == vh && ev1 == vn) || (ev1 == vh && ev0 == vn))) {
        err = "corner edge-vert mismatch";
        return false;
      }
      cc = cn;
      if (++n > 4000000) {
        err = "face loop did not close";
        return false;
      }
    } while (cc != c0);
  }
  return true;
}

/* Twice the signed area-vector (Newell normal) of a face's outer loop. Its
 * length is 2*area (→ degeneracy test) and its direction is the geometric
 * winding normal (→ orientation test). */
static inline math::float3 faceNewellNormal(Mesh &m, int f)
{
  using math::float3;
  float3 n{0.0f, 0.0f, 0.0f};
  int li = m.f.l[f], c0 = m.l.c[li], cc = c0;
  do {
    int cn = m.c.next[cc];
    float3 a = m.v.co[m.c.v[cc]];
    float3 b = m.v.co[m.c.v[cn]];
    n[0] += (a[1] - b[1]) * (a[2] + b[2]);
    n[1] += (a[2] - b[2]) * (a[0] + b[0]);
    n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    cc = cn;
  } while (cc != c0);
  return n;
}

/* Tier-0d geometric fold counts over interior manifold edges, from per-face
 * Newell normals (vertex normals lie in folded regions, so they can't be the
 * reference). fold90 = adjacent unit normals dot < 0; fold180 = dot < -0.95.
 * Wire/boundary/non-manifold edges and edges touching a degenerate face are
 * skipped. Sampled on the input, the Tier-9 pre-pass output, and (via
 * remeshValidate) the final output. */
struct FoldCounts {
  int fold90 = 0;
  int fold180 = 0;
  int degenerate_faces = 0;
};

static inline FoldCounts countGeometricFolds(Mesh &m)
{
  using litestl::util::Vector;
  using math::float3;

  FoldCounts fc;
  Vector<float3> fno;
  Vector<char> fok;
  fno.resize(int(m.f.capacity()));
  fok.resize(int(m.f.capacity()));
  for (int f : m.f) {
    float3 n = faceNewellNormal(m, f);
    float len = n.length();
    if (len < 1e-12f) {
      fc.degenerate_faces++;
      fok[f] = 0;
      continue;
    }
    fno[f] = n * (1.0f / len);
    fok[f] = 1;
  }
  for (int e : m.e) {
    int c0 = m.e.c[e];
    if (c0 == ELEM_NONE)
      continue; // wire
    int c1 = m.c.radial_next[c0];
    if (c1 == c0 || m.c.radial_next[c1] != c0)
      continue; // boundary or non-manifold
    int f0 = m.l.f[m.c.l[c0]], f1 = m.l.f[m.c.l[c1]];
    if (!fok[f0] || !fok[f1])
      continue;
    float d = fno[f0].dot(fno[f1]);
    if (d < 0.0f) {
      fc.fold90++;
      if (d < -0.95f)
        fc.fold180++;
    }
  }
  return fc;
}

/* Isoline (edge-strip) closure — the pipeline's headline no-spiral guarantee,
 * machine-checked on the extracted quad mesh's own connectivity. An isoline is
 * the transversal that enters a quad across one edge and exits across the
 * opposite edge (corner two steps round the loop); chaining transversals across
 * the shared edge (radial twin) traces a strip. Forward-stepping is a bijection
 * on directed transversals of a manifold quad mesh, so every strip is a finite
 * cycle (closed loop) or a path between two boundary edges — a strip that
 * exceeds 2*F steps signals connectivity corruption (a genuine spiral). On a
 * closed surface every isoline must close; that is what M5's integer
 * quantization guarantees. (This certifies each isoline closes into one simple
 * loop or ends at a boundary; it does not count homological winding — a (1,k)
 * torus knot still "closes" — which would need the (u,v) map.)
 *
 * Requires all_quad + manifold + consistent winding; a no-op otherwise. */
static inline void checkIsolineClosure(Mesh &m, RemeshReport &r)
{
  using litestl::util::Vector;
  if (!(r.all_quad && r.manifold && r.consistent_winding))
    return;

  int ccap = int(m.c.capacity());
  Vector<char> seen;
  seen.resize(ccap);
  for (int i = 0; i < ccap; i++)
    seen[i] = 0;

  const long budget = 2L * m.f.count + 8;
  auto opp = [&](int c) { return m.c.next[m.c.next[c]]; };
  auto isBnd = [&](int ce) { return m.c.radial_next[ce] == ce; };

  // Forward from entry corner `start`; marks each transversal's two corners.
  // 0 = closed back to start, 1 = hit a boundary, 2 = budget blown.
  auto traceFwd = [&](int start) -> int {
    int c = start;
    long guard = 0;
    while (true) {
      seen[c] = 1;
      int ce = opp(c);
      seen[ce] = 1;
      if (isBnd(ce))
        return 1;
      int twin = m.c.radial_next[ce];
      if (twin == start)
        return 0;
      c = twin;
      if (++guard > budget)
        return 2;
    }
  };

  r.isolines_checked = true;
  for (int f : m.f) {
    int c0 = m.l.c[m.f.l[f]], cc = c0;
    do {
      if (!seen[cc]) {
        int res = traceFwd(cc);
        if (res == 0) {
          r.closed_isolines++;
        } else if (res == 1) {
          traceFwd(opp(cc)); // cover the reverse half to the other boundary
          r.open_isolines++;
        } else {
          r.spiral_isolines++;
        }
      }
      cc = m.c.next[cc];
    } while (cc != c0);
  }
  r.isolines_close = r.open_isolines == 0 && r.spiral_isolines == 0;
}

/* Tier-0 extended quality metrics (plans/quad-remeshing-filtering.md): face-
 * component / boundary-loop counts, per-component singularity clustering, the
 * adjacent-quad size-transition ratios, and the skinny-quad min-angle proxy.
 * Split out of remeshValidate to keep that function readable. @p irrFaces is one
 * incident face per irregular interior vert (collected by the caller) so the
 * per-component tally needs no second boundary-detection pass. One pass each
 * over faces and edges — not a hot path. */
static inline void computeTier0Metrics(Mesh &m, RemeshReport &r,
                                       const litestl::util::Vector<int> &irrFaces)
{
  using litestl::util::Vector;
  using math::float3;

  int fcap = int(m.f.capacity());
  int vcap = int(m.v.capacity());
  if (fcap == 0)
    return;

  // Per-face area + mean edge length + min interior angle (binned). Area is the
  // Newell normal's half-length; the min angle is the smallest corner angle.
  Vector<double> faceArea, faceMeanEdge;
  faceArea.resize(fcap);
  faceMeanEdge.resize(fcap);
  for (int i = 0; i < fcap; i++) {
    faceArea[i] = -1.0;
    faceMeanEdge[i] = -1.0;
  }
  double minAngle = 0.0;
  bool firstAngle = true;
  for (int fi : m.f) {
    faceArea[fi] = 0.5 * double(faceNewellNormal(m, fi).length());

    int li = m.f.l[fi], c0 = m.l.c[li], cc = c0;
    double sumLen = 0.0, faceMin = 1e30;
    int ne = 0;
    do {
      int cn = m.c.next[cc], cp = m.c.prev[cc];
      float3 p = m.v.co[m.c.v[cc]];
      float3 toNext = m.v.co[m.c.v[cn]] - p;
      float3 toPrev = m.v.co[m.c.v[cp]] - p;
      double l1 = double(toNext.length()), l2 = double(toPrev.length());
      sumLen += l1;
      ne++;
      if (l1 > 1e-20 && l2 > 1e-20) {
        double cosa = double(toNext.dot(toPrev)) / (l1 * l2);
        cosa = cosa < -1.0 ? -1.0 : (cosa > 1.0 ? 1.0 : cosa);
        double ang = std::acos(cosa);
        if (ang < faceMin)
          faceMin = ang;
      }
      cc = cn;
    } while (cc != c0);
    faceMeanEdge[fi] = ne > 0 ? sumLen / double(ne) : 0.0;
    if (faceMin < 1e29) {
      if (firstAngle || faceMin < minAngle) {
        minAngle = faceMin;
        firstAngle = false;
      }
      int bin = int(faceMin * (180.0 / 3.14159265358979323846) / 10.0);
      r.min_angle_hist[bin < 0 ? 0 : (bin > 8 ? 8 : bin)]++;
    }
  }
  if (!firstAngle)
    r.min_interior_angle = float(minAngle);

  // One edge pass: face-component union-find (shared edge), boundary-vert
  // union-find (boundary edge), and the adjacent-pair area/edge size ratios.
  Vector<int> fuf, vuf;
  Vector<char> vBnd;
  fuf.resize(fcap);
  vuf.resize(vcap);
  vBnd.resize(vcap);
  for (int i = 0; i < fcap; i++)
    fuf[i] = i;
  for (int i = 0; i < vcap; i++) {
    vuf[i] = i;
    vBnd[i] = 0;
  }
  auto find = [](Vector<int> &uf, int x) {
    while (uf[x] != x) {
      uf[x] = uf[uf[x]];
      x = uf[x];
    }
    return x;
  };
  auto unite = [&](Vector<int> &uf, int a, int b) {
    a = find(uf, a);
    b = find(uf, b);
    if (a != b)
      uf[a] = b;
  };

  double maxAreaRatio = 1.0, maxEdgeRatio = 1.0;
  for (int ei : m.e) {
    int c0 = m.e.c[ei];
    if (c0 == ELEM_NONE)
      continue; // wire edge: neither a face-adjacency nor a boundary-loop edge
    int corners[2] = {ELEM_NONE, ELEM_NONE}, radial = 0, cc = c0;
    do {
      if (radial < 2)
        corners[radial] = cc;
      radial++;
      cc = m.c.radial_next[cc];
    } while (cc != c0 && radial < 1000000);

    if (radial == 1) {
      int v0 = m.e.vs[ei][0], v1 = m.e.vs[ei][1];
      unite(vuf, v0, v1);
      vBnd[v0] = vBnd[v1] = 1;
    } else if (radial == 2) {
      int fa = m.l.f[m.c.l[corners[0]]], fb = m.l.f[m.c.l[corners[1]]];
      unite(fuf, fa, fb);
      double aA = faceArea[fa], aB = faceArea[fb];
      if (aA > 1e-20 && aB > 1e-20) {
        double rr = aA > aB ? aA / aB : aB / aA;
        if (rr > maxAreaRatio)
          maxAreaRatio = rr;
      }
      double eA = faceMeanEdge[fa], eB = faceMeanEdge[fb];
      if (eA > 1e-20 && eB > 1e-20) {
        double rr = eA > eB ? eA / eB : eB / eA;
        if (rr > maxEdgeRatio)
          maxEdgeRatio = rr;
      }
    }
  }
  r.max_adjacent_area_ratio = float(maxAreaRatio);
  r.max_adjacent_edge_ratio = float(maxEdgeRatio);

  // Distinct face-component roots; per-component irregular-vert tally → worst.
  Vector<int> compIrr;
  compIrr.resize(fcap);
  for (int i = 0; i < fcap; i++)
    compIrr[i] = 0;
  int comps = 0;
  for (int fi : m.f) {
    if (find(fuf, fi) == fi)
      comps++;
  }
  r.component_count = comps;
  for (int i = 0; i < int(irrFaces.size()); i++) {
    int f = irrFaces[i];
    if (f != ELEM_NONE)
      compIrr[find(fuf, f)]++;
  }
  int maxIrr = 0;
  for (int fi : m.f) {
    if (compIrr[fi] > maxIrr)
      maxIrr = compIrr[fi];
  }
  r.max_component_irregular = maxIrr;

  // Distinct boundary-loop roots over verts touched by a boundary edge.
  int loops = 0;
  for (int vi = 0; vi < vcap; vi++) {
    if (vBnd[vi] && find(vuf, vi) == vi)
      loops++;
  }
  r.boundary_loop_count = loops;
}

/* Full structural report. One pass each over edges / faces / verts; cycle
 * integrity delegated to checkTopology (the manifold gate). On an all-quad
 * manifold mesh it also runs checkIsolineClosure (the no-spiral gate); the
 * isoline / T-junction fields stay unchecked otherwise. */
static inline RemeshReport remeshValidate(Mesh &m)
{
  using litestl::util::Vector;
  using math::float3;

  RemeshReport r;
  r.manifold = checkTopology(m, r.manifold_error, false);

  r.vert_count = m.v.count;
  r.edge_count = m.e.count;
  r.face_count = m.f.count;
  r.euler = r.vert_count - r.edge_count + r.face_count;

  // Face census + geometry health.
  for (int fi : m.f) {
    int sz = m.l.size[m.f.l[fi]];
    if (sz == 3)
      r.tri_count++;
    else if (sz == 4)
      r.quad_count++;
    else
      r.ngon_count++;

    float3 nn = faceNewellNormal(m, fi);
    if (nn.length() < 1e-12f) {
      r.degenerate_faces++;
      continue;
    }
    // Sum incident vertex normals; a face flipped relative to the surface has
    // its winding normal opposing that local field.
    float3 vsum{0.0f, 0.0f, 0.0f};
    int li = m.f.l[fi], c0 = m.l.c[li], cc = c0;
    do {
      vsum += m.v.no[m.c.v[cc]];
      cc = m.c.next[cc];
    } while (cc != c0);
    if (vsum.length() > 1e-9f && nn.dot(vsum) < 0.0f)
      r.inverted_faces++;
  }
  r.all_quad = (r.face_count > 0 && r.tri_count == 0 && r.ngon_count == 0);

  FoldCounts fc = countGeometricFolds(m);
  r.fold90_edges = fc.fold90;
  r.fold180_edges = fc.fold180;

  // Per-edge: radial multiplicity + winding consistency across each manifold
  // edge (its two corners must traverse it in opposite directions).
  bool winding_ok = true;
  for (int ei : m.e) {
    int c0 = m.e.c[ei];
    if (c0 == ELEM_NONE) {
      r.non_manifold_edges++; // wire edge: no incident face
      continue;
    }
    int radial = 0, cc = c0;
    int corners[2] = {ELEM_NONE, ELEM_NONE};
    do {
      if (radial < 2)
        corners[radial] = cc;
      radial++;
      cc = m.c.radial_next[cc];
    } while (cc != c0 && radial < 1000000);

    if (radial == 1) {
      r.boundary_edges++;
    } else if (radial == 2) {
      int c1 = corners[0], c2 = corners[1];
      int from1 = m.c.v[c1], from2 = m.c.v[c2];
      // Opposite traversal ⇒ the two corners start at different endpoints.
      if (from1 == from2)
        winding_ok = false;
    } else {
      r.non_manifold_edges++;
    }
  }
  r.consistent_winding = winding_ok && r.non_manifold_edges == 0;

  // Per-vert valence + interior-irregular count. A vert is boundary if any
  // incident edge is a boundary/wire edge. `irrFaces` records one incident face
  // per irregular interior vert so the Tier-0 block below can tally singularities
  // per face-component without a second boundary-detection pass.
  Vector<int> irrFaces;
  for (int vi : m.v) {
    int e0 = m.v.e[vi];
    if (e0 == ELEM_NONE)
      continue;
    int valence = 0, ec = e0;
    bool boundary = false;
    do {
      int c0 = m.e.c[ec];
      if (c0 == ELEM_NONE) {
        boundary = true;
      } else {
        int radial = 0, cc = c0;
        do {
          radial++;
          cc = m.c.radial_next[cc];
        } while (cc != c0 && radial < 1000000);
        if (radial != 2)
          boundary = true;
      }
      int side = m.e.vs[ec][0] == vi ? 0 : 1;
      ec = m.e.disk[ec][side * 2 + 1];
      if (++valence > 1000000)
        break;
    } while (ec != e0);

    r.valence_hist[valence < 17 ? valence : 16]++;
    if (!boundary) {
      r.interior_vert_count++;
      if (valence != 4) {
        r.irregular_interior_verts++;
        int c = m.e.c[e0]; // interior vert ⇒ e0 has a face
        irrFaces.append(c != ELEM_NONE ? m.l.f[m.c.l[c]] : ELEM_NONE);
      }
    }
  }
  r.regular_interior_frac =
      r.interior_vert_count > 0
          ? 1.0f - float(r.irregular_interior_verts) / float(r.interior_vert_count)
          : 1.0f;

  computeTier0Metrics(m, r, irrFaces);

  checkIsolineClosure(m, r);
  return r;
}

/* Tier 6.7 boundary-preservation error: distance from each boundary vert of
 * @p test to the nearest boundary segment of @p ref (brute force over segments
 * — corpus rims are O(1k) edges). Counts are 0 when either side has none. */
struct BoundaryDeviation {
  int ref_boundary_edges = 0;  // boundary segments in the reference mesh
  int test_boundary_verts = 0; // boundary verts measured in the test mesh
  float mean_dist = 0.0f;
  float max_dist = 0.0f;
};

static inline BoundaryDeviation boundaryDeviation(Mesh &ref, Mesh &test)
{
  using litestl::util::Vector;
  using math::float3;

  BoundaryDeviation bd;

  auto edgeRadial = [](Mesh &m, int ei) {
    int c0 = m.e.c[ei];
    if (c0 == ELEM_NONE)
      return 0;
    int radial = 0, cc = c0;
    do {
      radial++;
      cc = m.c.radial_next[cc];
    } while (cc != c0 && radial < 1000000);
    return radial;
  };

  Vector<float3> seg; // flat (a, b) pairs
  for (int ei : ref.e) {
    if (edgeRadial(ref, ei) == 1) {
      seg.append(ref.v.co[ref.e.vs[ei][0]]);
      seg.append(ref.v.co[ref.e.vs[ei][1]]);
    }
  }
  bd.ref_boundary_edges = int(seg.size()) / 2;

  Vector<char> isBnd;
  isBnd.resize(int(test.v.capacity()));
  for (int i = 0; i < int(test.v.capacity()); i++)
    isBnd[i] = 0;
  for (int ei : test.e) {
    if (edgeRadial(test, ei) == 1) {
      isBnd[test.e.vs[ei][0]] = 1;
      isBnd[test.e.vs[ei][1]] = 1;
    }
  }

  if (bd.ref_boundary_edges == 0)
    return bd;

  double sum = 0.0;
  for (int vi : test.v) { // ascending id order -> deterministic mean
    if (!isBnd[vi])
      continue;
    float3 p = test.v.co[vi];
    float best = FLT_MAX;
    for (int s = 0; s < int(seg.size()); s += 2) {
      float t;
      float3 cp = math::closestPointOnSegment(p, seg[s], seg[s + 1], true, t);
      best = std::fmin(best, (cp - p).length());
    }
    bd.test_boundary_verts++;
    sum += double(best);
    bd.max_dist = std::fmax(bd.max_dist, best);
  }
  if (bd.test_boundary_verts > 0)
    bd.mean_dist = float(sum / bd.test_boundary_verts);
  return bd;
}

} // namespace sculptcore::mesh
