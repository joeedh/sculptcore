#pragma once

/* Delaunay triangulation of a vertex set.
 *
 * Adds new triangle faces to `m` whose vertices are drawn from the
 * input vertex index set. The input verts are projected onto a
 * best-fit plane (or one defined by the optional normal argument) and
 * a 2D Delaunay triangulation is computed via Bowyer-Watson. Existing
 * topology is preserved; only new edges/faces are added.
 *
 * Degenerate inputs (<3 unique projected points, all collinear) yield
 * no faces and a successful return.
 */

#include "../mesh.h"
#include "../mesh_base.h"
#include "../mesh_proxy.h"

#include "litestl/math/vector.h"
#include "litestl/util/error.h"
#include "litestl/util/map.h"
#include "litestl/util/set.h"
#include "litestl/util/span.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdint>
#include <optional>

namespace sculptcore::mesh {

namespace detail_delaunay {

using litestl::math::float2;
using litestl::math::float3;

struct DTri {
  int a, b, c; /* indices into projected-point array */
  bool alive;
};

struct DEdge {
  int a, b;
  bool operator==(const DEdge &o) const
  {
    return (a == o.a && b == o.b) || (a == o.b && b == o.a);
  }
};

static inline float dt_cross2(float2 u, float2 v)
{
  return u[0] * v[1] - u[1] * v[0];
}

/* Returns true if p is strictly inside the circumcircle of (a,b,c).
 * Robust enough for randomized tests; not exact predicates. */
static inline bool inCircumcircle(float2 a, float2 b, float2 c, float2 p)
{
  /* Ensure CCW orientation. */
  float orient = dt_cross2(b - a, c - a);
  if (orient == 0.0f) {
    return false;
  }

  float2 A = a - p;
  float2 B = b - p;
  float2 C = c - p;

  double ax = A[0], ay = A[1];
  double bx = B[0], by = B[1];
  double cx = C[0], cy = C[1];

  double det = (ax * ax + ay * ay) * (bx * cy - cx * by) -
               (bx * bx + by * by) * (ax * cy - cx * ay) +
               (cx * cx + cy * cy) * (ax * by - bx * ay);

  /* For CCW triangles det > 0 iff p strictly inside. Flip sign for CW. */
  if (orient < 0.0f) {
    det = -det;
  }
  return det > 0.0;
}

static inline float3 fitPlaneNormal(litestl::util::span<const float3> pts)
{
  using namespace litestl;
  if (pts.size() < 3) {
    return float3(0.0f, 0.0f, 1.0f);
  }

  /* Find AABB extent, pick axis of greatest spread for u. */
  float3 mn = pts[0], mx = pts[0];
  for (size_t i = 1; i < pts.size(); i++) {
    mn.min(pts[i]);
    mx.max(pts[i]);
  }
  float3 ext = mx - mn;
  int axis = 0;
  if (ext[1] > ext[axis]) axis = 1;
  if (ext[2] > ext[axis]) axis = 2;

  /* Find two extreme points along that axis. */
  int ia = 0, ib = 0;
  for (size_t i = 0; i < pts.size(); i++) {
    if (pts[i][axis] < pts[ia][axis]) ia = int(i);
    if (pts[i][axis] > pts[ib][axis]) ib = int(i);
  }
  if (ia == ib) {
    return float3(0.0f, 0.0f, 1.0f);
  }
  float3 u = pts[ib] - pts[ia];
  if (u.normalize() == 0.0f) {
    return float3(0.0f, 0.0f, 1.0f);
  }

  /* Find point farthest from line (pts[ia], u). */
  float bestD = -1.0f;
  int ic = -1;
  float3 bestPerp(0.0f);
  for (size_t i = 0; i < pts.size(); i++) {
    float3 d = pts[i] - pts[ia];
    float t = d.dot(u);
    float3 perp = d - u * t;
    float pl = perp.length();
    if (pl > bestD) {
      bestD = pl;
      ic = int(i);
      bestPerp = perp;
    }
  }
  if (ic < 0 || bestD < 1e-7f) {
    /* Collinear or degenerate. Return any orthogonal axis-aligned normal. */
    float3 axis_v(0.0f);
    int alt = (axis + 1) % 3;
    axis_v[alt] = 1.0f;
    float3 n = u;
    n.crossSelf(axis_v);
    if (n.length() < 1e-6f) {
      axis_v = float3(0.0f);
      axis_v[(axis + 2) % 3] = 1.0f;
      n = u;
      n.crossSelf(axis_v);
    }
    n.normalize();
    return n;
  }

  float3 n = u;
  n.crossSelf(bestPerp);
  n.normalize();
  return n;
}

static inline void planeBasis(float3 n, float3 &u, float3 &v)
{
  /* Build an orthonormal basis with n as the third axis. */
  float3 ref(1.0f, 0.0f, 0.0f);
  if (std::fabs(n[0]) > 0.9f) {
    ref = float3(0.0f, 1.0f, 0.0f);
  }
  u = ref;
  u.crossSelf(n);
  u.normalize();
  v = n;
  v.crossSelf(u);
  v.normalize();
}

// Constrained Delaunay (no Steiner points): helpers run on a flat triangle soup
// (dead tris kept alive=false) + a coord array, adjacency found by scanning.
// Intermediate winding is irrelevant; only the final emit forces CCW.

struct CDTri {
  int v[3];
  bool alive;
};

static inline int64_t cdtEdgeKey(int a, int b)
{
  if (a > b) {
    int t = a;
    a = b;
    b = t;
  }
  return (int64_t(a) << 32) | uint32_t(b);
}

/** Proper crossing of open segments a-b and c-d (shared endpoints or collinear
 * touching count as no crossing). */
static inline bool cdtSegCross(float2 a, float2 b, float2 c, float2 d)
{
  float d1 = dt_cross2(b - a, c - a);
  float d2 = dt_cross2(b - a, d - a);
  float d3 = dt_cross2(d - c, a - c);
  float d4 = dt_cross2(d - c, b - c);
  return ((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f)) &&
         ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f));
}

/** Index of an alive triangle containing both a and b, else -1. */
static inline int cdtFindEdgeTri(const litestl::util::Vector<CDTri> &tris, int a, int b)
{
  for (int t = 0; t < int(tris.size()); t++) {
    if (!tris[t].alive) continue;
    const int *v = tris[t].v;
    bool ha = v[0] == a || v[1] == a || v[2] == a;
    bool hb = v[0] == b || v[1] == b || v[2] == b;
    if (ha && hb) return t;
  }
  return -1;
}

/** The vertex of T other than a and b. */
static inline int cdtApex(const CDTri &T, int a, int b)
{
  for (int i = 0; i < 3; i++) {
    if (T.v[i] != a && T.v[i] != b) return T.v[i];
  }
  return -1;
}

/** The other alive triangle sharing edge (a,b), excluding `exclude`, else -1. */
static inline int cdtOtherTri(const litestl::util::Vector<CDTri> &tris, int a, int b, int exclude)
{
  for (int t = 0; t < int(tris.size()); t++) {
    if (t == exclude || !tris[t].alive) continue;
    const int *v = tris[t].v;
    bool ha = v[0] == a || v[1] == a || v[2] == a;
    bool hb = v[0] == b || v[1] == b || v[2] == b;
    if (ha && hb) return t;
  }
  return -1;
}

/** Flip the diagonal shared by t1,t2 from (c,d) to (e,g). */
static inline void cdtFlip(litestl::util::Vector<CDTri> &tris, int t1, int t2,
                           int c, int d, int e, int g)
{
  tris[t1].v[0] = e;
  tris[t1].v[1] = g;
  tris[t1].v[2] = c;
  tris[t2].v[0] = e;
  tris[t2].v[1] = g;
  tris[t2].v[2] = d;
}

/** Recover constraint edge (ca,cb) by flipping crossing, non-constraint,
 * convex-quad edges. Returns false if it stalls (a vertex sits on the segment,
 * or the cap is hit) so the caller can fall back. */
static inline bool cdtRecoverEdge(litestl::util::Vector<CDTri> &tris,
                                  const litestl::util::Vector<float2> &pts,
                                  const litestl::util::Set<int64_t> &constraintKeys,
                                  int ca, int cb)
{
  int cap = 4 * int(tris.size()) + 64;
  while (cdtFindEdgeTri(tris, ca, cb) < 0) {
    if (--cap < 0) return false;
    bool flipped = false;
    for (int t = 0; t < int(tris.size()) && !flipped; t++) {
      if (!tris[t].alive) continue;
      for (int s = 0; s < 3; s++) {
        int c = tris[t].v[s];
        int d = tris[t].v[(s + 1) % 3];
        if (c == ca || c == cb || d == ca || d == cb) continue;
        if (constraintKeys.contains(cdtEdgeKey(c, d))) continue;
        if (!cdtSegCross(pts[ca], pts[cb], pts[c], pts[d])) continue;
        int t2 = cdtOtherTri(tris, c, d, t);
        if (t2 < 0) continue;
        int e = cdtApex(tris[t], c, d);
        int g = cdtApex(tris[t2], c, d);
        if (e < 0 || g < 0 || e == g) continue;
        if (!cdtSegCross(pts[c], pts[d], pts[e], pts[g])) continue; // non-convex
        cdtFlip(tris, t, t2, c, d, e, g);
        flipped = true;
        break;
      }
    }
    if (!flipped) return false;
  }
  return true;
}

/** Lawson flips on non-constraint edges to restore the empty-circumcircle
 * property. Capped against fp-driven cycling. */
static inline void cdtLawsonRestore(litestl::util::Vector<CDTri> &tris,
                                    const litestl::util::Vector<float2> &pts,
                                    const litestl::util::Set<int64_t> &constraintKeys)
{
  int cap = 8 * int(tris.size()) + 64;
  bool changed = true;
  while (changed && --cap > 0) {
    changed = false;
    for (int t = 0; t < int(tris.size()) && !changed; t++) {
      if (!tris[t].alive) continue;
      for (int s = 0; s < 3; s++) {
        int c = tris[t].v[s];
        int d = tris[t].v[(s + 1) % 3];
        if (constraintKeys.contains(cdtEdgeKey(c, d))) continue;
        int t2 = cdtOtherTri(tris, c, d, t);
        if (t2 < 0) continue;
        int e = cdtApex(tris[t], c, d);
        int g = cdtApex(tris[t2], c, d);
        if (e < 0 || g < 0) continue;
        if (!cdtSegCross(pts[c], pts[d], pts[e], pts[g])) continue; // non-convex
        if (inCircumcircle(pts[c], pts[d], pts[e], pts[g])) {
          cdtFlip(tris, t, t2, c, d, e, g);
          changed = true;
          break;
        }
      }
    }
  }
}

/** Fill nbr[t*3+s] with the triangle across edge (v[s],v[s+1]) of t, or -1. */
static inline void cdtBuildAdjacency(const litestl::util::Vector<CDTri> &tris,
                                     litestl::util::Vector<int> &nbr)
{
  int T = int(tris.size());
  nbr.clear();
  for (int i = 0; i < T * 3; i++) nbr.append(-1);
  for (int t = 0; t < T; t++) {
    if (!tris[t].alive) continue;
    for (int s = 0; s < 3; s++) {
      nbr[t * 3 + s] = cdtOtherTri(tris, tris[t].v[s], tris[t].v[(s + 1) % 3], t);
    }
  }
}

/** Parity flood fill seeded from the super-triangle (outside): crossing a
 * constraint edge toggles inside/outside. `N` is the real point count (super
 * verts are >= N). Fills inside[t] with 1 (interior) / 0 (exterior). */
static inline void cdtFloodInterior(const litestl::util::Vector<CDTri> &tris,
                                    const litestl::util::Vector<int> &nbr,
                                    const litestl::util::Set<int64_t> &constraintKeys,
                                    int N,
                                    litestl::util::Vector<int> &inside)
{
  int T = int(tris.size());
  inside.clear();
  for (int i = 0; i < T; i++) inside.append(-1);
  litestl::util::Vector<int> stack;
  for (int t = 0; t < T; t++) {
    if (!tris[t].alive) continue;
    const int *v = tris[t].v;
    if ((v[0] >= N || v[1] >= N || v[2] >= N) && inside[t] == -1) {
      inside[t] = 0;
      stack.append(t);
    }
  }
  while (stack.size()) {
    int t = stack[int(stack.size()) - 1];
    stack.pop_back();
    for (int s = 0; s < 3; s++) {
      int nb = nbr[t * 3 + s];
      if (nb < 0 || inside[nb] != -1) continue;
      bool cross =
          constraintKeys.contains(cdtEdgeKey(tris[t].v[s], tris[t].v[(s + 1) % 3]));
      inside[nb] = cross ? (inside[t] ^ 1) : inside[t];
      stack.append(nb);
    }
  }
  for (int t = 0; t < T; t++) {
    if (tris[t].alive && inside[t] == -1) inside[t] = 1;
  }
}

} /* namespace detail_delaunay */

using litestl::util::SuccessOrError;

template <typename VertSpan>
static SuccessOrError<"delaunay", "failed to triangulate point set">
delaunayTriangulate(Mesh &m,
                    VertSpan vert_indices,
                    std::optional<litestl::math::float3> plane_normal = std::nullopt,
                    litestl::util::Vector<int> *out_faces = nullptr)
{
  using namespace litestl;
  using namespace litestl::math;
  using namespace detail_delaunay;

  /* Collect & dedupe input vertex indices. */
  util::Vector<int> verts;
  util::Set<int> seen;
  for (int vi : vert_indices) {
    if (seen.add(vi)) {
      verts.append(vi);
    }
  }
  if (verts.size() < 3) {
    return true;
  }

  /* Gather 3D positions. */
  util::Vector<float3> pos3;
  for (int vi : verts) {
    pos3.append(m.v.co[vi]);
  }

  /* Choose plane. */
  float3 n = plane_normal.has_value() ?
                 plane_normal.value() :
                 fitPlaneNormal(litestl::util::span<const float3>(pos3.data(), pos3.size()));
  if (n.length() < 1e-6f) {
    n = float3(0.0f, 0.0f, 1.0f);
  } else {
    n.normalize();
  }

  float3 ub, vb;
  planeBasis(n, ub, vb);

  /* Project to 2D and dedupe near-duplicate projections. */
  const float dup_eps = 1e-7f;
  util::Vector<float2> pts2;
  util::Vector<int> remap; /* index in pts2 -> index in verts */
  for (int i = 0; i < int(pos3.size()); i++) {
    float3 d = pos3[i];
    float2 p(d.dot(ub), d.dot(vb));
    bool dup = false;
    for (int j = 0; j < int(pts2.size()); j++) {
      float2 q = pts2[j];
      if (std::fabs(p[0] - q[0]) < dup_eps && std::fabs(p[1] - q[1]) < dup_eps) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      pts2.append(p);
      remap.append(i);
    }
  }
  if (pts2.size() < 3) {
    return true;
  }

  /* Reject all-collinear input. */
  {
    bool collinear = true;
    float2 a = pts2[0];
    float2 b = pts2[1];
    float2 dir = b - a;
    for (int i = 2; i < int(pts2.size()); i++) {
      float2 d = pts2[i] - a;
      float c = dt_cross2(dir, d);
      if (std::fabs(c) > 1e-6f) {
        collinear = false;
        break;
      }
    }
    if (collinear) {
      return true;
    }
  }

  /* Build super-triangle around AABB (much larger than extent). */
  float2 mn = pts2[0], mx = pts2[0];
  for (int i = 1; i < int(pts2.size()); i++) {
    mn.min(pts2[i]);
    mx.max(pts2[i]);
  }
  float2 c = (mn + mx) * 0.5f;
  float2 ext = mx - mn;
  float r = std::max(ext[0], ext[1]);
  if (r < 1.0f) r = 1.0f;
  r *= 64.0f;

  int N = int(pts2.size());
  pts2.append(float2(c[0] - 2.0f * r, c[1] - r));
  pts2.append(float2(c[0] + 2.0f * r, c[1] - r));
  pts2.append(float2(c[0], c[1] + 2.0f * r));
  int sa = N, sb = N + 1, sc = N + 2;

  util::Vector<DTri> tris;
  tris.append({sa, sb, sc, true});

  /* Bowyer-Watson incremental. */
  for (int pi = 0; pi < N; pi++) {
    float2 p = pts2[pi];

    util::Vector<DEdge> boundary;

    for (int t = 0; t < int(tris.size()); t++) {
      if (!tris[t].alive) continue;
      DTri &T = tris[t];
      if (inCircumcircle(pts2[T.a], pts2[T.b], pts2[T.c], p)) {
        DEdge edges[3] = {{T.a, T.b}, {T.b, T.c}, {T.c, T.a}};
        for (DEdge e : edges) {
          /* Toggle: shared edges cancel, leaving only the cavity boundary. */
          bool found = false;
          for (int k = 0; k < int(boundary.size()); k++) {
            if (boundary[k] == e) {
              boundary.remove_at(k, true);
              found = true;
              break;
            }
          }
          if (!found) {
            boundary.append(e);
          }
        }
        T.alive = false;
      }
    }

    for (DEdge e : boundary) {
      tris.append({e.a, e.b, pi, true});
    }
  }

  /* Drop tris touching the super-triangle. */
  util::Vector<DTri> finalTris;
  for (const DTri &T : tris) {
    if (!T.alive) continue;
    if (T.a >= N || T.b >= N || T.c >= N) continue;
    finalTris.append(T);
  }

  /* Emit faces using original mesh vertex indices. */
  for (const DTri &T : finalTris) {
    int v0 = verts[remap[T.a]];
    int v1 = verts[remap[T.b]];
    int v2 = verts[remap[T.c]];

    /* Make sure CCW in the chosen plane (for consistent face normal). */
    float2 A = pts2[T.a], B = pts2[T.b], C = pts2[T.c];
    float orient = dt_cross2(B - A, C - A);

    int tri[3] = {v0, v1, v2};
    if (orient < 0.0f) {
      tri[0] = v0;
      tri[1] = v2;
      tri[2] = v1;
    }
    int fi = m.make_face(std::span<int>(tri, 3));
    if (out_faces) {
      out_faces->append(fi);
    }
  }

  return true;
}

/** Constrained Delaunay triangulation of a 2D polygon, possibly with holes, using
 * only the input points (no Steiner points). `constraints` are the undirected
 * boundary segments — the outer loop plus each hole loop, every loop closed.
 * They define the region: interior triangles (inside the outer loop, outside the
 * holes) are emitted as flat CCW index triples into `points`; everything else is
 * dropped. With no constraints the result is empty.
 *
 * Degenerate input (<3 unique points, all-collinear) yields an empty result and
 * success; a constraint-recovery failure (a vertex lying on a constraint, or a
 * self-intersecting boundary) also yields empty, so callers can fall back. */
static inline SuccessOrError<"delaunay", "constrained triangulation failed">
constrainedDelaunay2D(litestl::util::span<const litestl::math::float2> points,
                      litestl::util::span<const detail_delaunay::DEdge> constraints,
                      litestl::util::Vector<int> &out_tris,
                      bool restore_delaunay = true)
{
  using namespace litestl;
  using namespace litestl::math;
  using namespace detail_delaunay;

  out_tris.clear();
  int rawN = int(points.size());
  if (rawN < 3) return true;

  // Dedup near-duplicate points; map raw->unique and unique->first-raw.
  const float dup_eps = 1e-7f;
  util::Vector<float2> pts;
  util::Vector<int> rawToUniq, uniqToRaw;
  for (int i = 0; i < rawN; i++) {
    int found = -1;
    for (int j = 0; j < int(pts.size()); j++) {
      if (std::fabs(points[i][0] - pts[j][0]) < dup_eps &&
          std::fabs(points[i][1] - pts[j][1]) < dup_eps) {
        found = j;
        break;
      }
    }
    if (found < 0) {
      found = int(pts.size());
      pts.append(points[i]);
      uniqToRaw.append(i);
    }
    rawToUniq.append(found);
  }
  int N = int(pts.size());
  if (N < 3) return true;

  // Constraint edge set (remapped, undirected, degenerate dropped).
  util::Set<int64_t> constraintKeys;
  util::Vector<DEdge> constraintEdges;
  for (const DEdge &e : constraints) {
    if (e.a < 0 || e.b < 0 || e.a >= rawN || e.b >= rawN) continue;
    int a = rawToUniq[e.a], b = rawToUniq[e.b];
    if (a == b) continue;
    if (constraintKeys.add(cdtEdgeKey(a, b))) {
      constraintEdges.append({a, b});
    }
  }

  // Reject all-collinear input.
  {
    bool collinear = true;
    float2 dir = pts[1] - pts[0];
    for (int i = 2; i < N; i++) {
      if (std::fabs(dt_cross2(dir, pts[i] - pts[0])) > 1e-6f) {
        collinear = false;
        break;
      }
    }
    if (collinear) return true;
  }

  // Super-triangle around the AABB.
  float2 mn = pts[0], mx = pts[0];
  for (int i = 1; i < N; i++) {
    mn.min(pts[i]);
    mx.max(pts[i]);
  }
  float2 cen = (mn + mx) * 0.5f;
  float2 ext = mx - mn;
  float r = std::max(ext[0], ext[1]);
  if (r < 1.0f) r = 1.0f;
  r *= 64.0f;
  pts.append(float2(cen[0] - 2.0f * r, cen[1] - r));
  pts.append(float2(cen[0] + 2.0f * r, cen[1] - r));
  pts.append(float2(cen[0], cen[1] + 2.0f * r));

  // Bowyer-Watson over the real points; the super-triangle is kept (it seeds the
  // interior flood fill).
  util::Vector<CDTri> tris;
  tris.append({{N, N + 1, N + 2}, true});
  for (int pi = 0; pi < N; pi++) {
    float2 p = pts[pi];
    util::Vector<DEdge> boundary;
    for (int t = 0; t < int(tris.size()); t++) {
      if (!tris[t].alive) continue;
      CDTri &T = tris[t];
      if (inCircumcircle(pts[T.v[0]], pts[T.v[1]], pts[T.v[2]], p)) {
        DEdge edges[3] = {{T.v[0], T.v[1]}, {T.v[1], T.v[2]}, {T.v[2], T.v[0]}};
        for (DEdge e : edges) {
          bool found = false;
          for (int k = 0; k < int(boundary.size()); k++) {
            if (boundary[k] == e) {
              boundary.remove_at(k, true);
              found = true;
              break;
            }
          }
          if (!found) boundary.append(e);
        }
        T.alive = false;
      }
    }
    for (DEdge e : boundary) {
      tris.append({{e.a, e.b, pi}, true});
    }
  }

  // Recover each constraint edge into the triangulation.
  for (const DEdge &ce : constraintEdges) {
    if (!cdtRecoverEdge(tris, pts, constraintKeys, ce.a, ce.b)) {
      out_tris.clear();
      return true;
    }
  }

  if (restore_delaunay) {
    cdtLawsonRestore(tris, pts, constraintKeys);
  }

  // Classify interior, then emit interior non-super triangles CCW.
  util::Vector<int> nbr, inside;
  cdtBuildAdjacency(tris, nbr);
  cdtFloodInterior(tris, nbr, constraintKeys, N, inside);

  for (int t = 0; t < int(tris.size()); t++) {
    if (!tris[t].alive || inside[t] != 1) continue;
    int a = tris[t].v[0], b = tris[t].v[1], c = tris[t].v[2];
    if (a >= N || b >= N || c >= N) continue;
    if (dt_cross2(pts[b] - pts[a], pts[c] - pts[a]) < 0.0f) {
      int tmp = b;
      b = c;
      c = tmp;
    }
    out_tris.append(uniqToRaw[a]);
    out_tris.append(uniqToRaw[b]);
    out_tris.append(uniqToRaw[c]);
  }

  return true;
}

} /* namespace sculptcore::mesh */
