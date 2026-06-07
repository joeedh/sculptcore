#pragma once

#include "../mesh.h"
#include "litestl/util/vector.h"

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

  // Valence / singularity inventory (meaningful on all-quad output).
  int irregular_interior_verts = 0; // interior verts with valence != 4
  int valence_hist[17] = {0};       // index = min(valence, 16)

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
  // incident edge is a boundary/wire edge.
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
    if (!boundary && valence != 4)
      r.irregular_interior_verts++;
  }

  checkIsolineClosure(m, r);
  return r;
}

} // namespace sculptcore::mesh
