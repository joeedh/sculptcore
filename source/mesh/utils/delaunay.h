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
    n.cross(axis_v);
    if (n.length() < 1e-6f) {
      axis_v = float3(0.0f);
      axis_v[(axis + 2) % 3] = 1.0f;
      n = u;
      n.cross(axis_v);
    }
    n.normalize();
    return n;
  }

  float3 n = u;
  n.cross(bestPerp);
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
  u.cross(n);
  u.normalize();
  v = n;
  v.cross(u);
  v.normalize();
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

} /* namespace sculptcore::mesh */
