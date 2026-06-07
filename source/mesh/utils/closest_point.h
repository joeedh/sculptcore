#pragma once

/* Closest-point query over a SpatialTree's BVH (M1). Physically in mesh/utils/
 * but it pulls in spatial/spatial.h, so only consumers that already link the
 * `spatial` library (the remesh reproject stage + tests) ever compile it — the
 * mesh library itself never includes it, so this is not a layering violation.
 * See documentation/plans/quad-remeshing.md. */

#include "litestl/math/geom.h" // closestPointOnTri
#include "litestl/math/vector.h"
#include "spatial/node.h"
#include "spatial/spatial.h"

#include <cmath>
#include <limits>

namespace sculptcore::mesh {

struct ClosestPointResult {
  bool hit = false;
  int face = -1;               // mesh face index of the owning triangle
  int tri_c[3] = {-1, -1, -1}; // the triangle's mesh corners
  math::float3 point{0.0f, 0.0f, 0.0f};
  math::float3 bary{0.0f, 0.0f, 0.0f}; // barycentric weights (u, v, w)
  float dist = std::numeric_limits<float>::max();
};

namespace detail {

/* Squared distance from p to an AABB (0 when inside). */
static inline float closestPointAabbDistSqr(const spatial::SpatialNode::AABB &box,
                                            const math::float3 &p)
{
  float d2 = 0.0f;
  for (int i = 0; i < 3; i++) {
    float lo = box.min[i], hi = box.max[i];
    float x = p[i] < lo ? lo - p[i] : (p[i] > hi ? p[i] - hi : 0.0f);
    d2 += x * x;
  }
  return d2;
}

/* Barycentric coordinates of p relative to triangle (a, b, c). */
static inline math::float3 closestPointBary(const math::float3 &p,
                                            const math::float3 &a,
                                            const math::float3 &b,
                                            const math::float3 &c)
{
  math::float3 v0 = b - a, v1 = c - a, v2 = p - a;
  float d00 = v0.dot(v0), d01 = v0.dot(v1), d11 = v1.dot(v1);
  float d20 = v2.dot(v0), d21 = v2.dot(v1);
  float denom = d00 * d11 - d01 * d01;
  if (std::fabs(denom) < 1e-20f) {
    return math::float3(1.0f, 0.0f, 0.0f);
  }
  float v = (d11 * d20 - d01 * d21) / denom;
  float w = (d00 * d21 - d01 * d20) / denom;
  return math::float3(1.0f - v - w, v, w);
}

static inline void closestPointWalk(spatial::SpatialTree &tree,
                                    spatial::SpatialNode *node,
                                    const math::float3 &p,
                                    ClosestPointResult &best)
{
  if (!node) {
    return;
  }

  if (!(node->flag & spatial::Spatial_Leaf)) {
    spatial::SpatialNode *a = node->children[0];
    spatial::SpatialNode *b = node->children[1];
    const float inf = std::numeric_limits<float>::max();
    float da = a ? closestPointAabbDistSqr(a->aabb, p) : inf;
    float db = b ? closestPointAabbDistSqr(b->aabb, p) : inf;
    // Descend the nearer child first, then prune the far one by the best so far.
    spatial::SpatialNode *first = da <= db ? a : b;
    spatial::SpatialNode *second = da <= db ? b : a;
    float dfirst = da <= db ? da : db;
    float dsecond = da <= db ? db : da;
    if (first && dfirst <= best.dist * best.dist) {
      closestPointWalk(tree, first, p, best);
    }
    if (second && dsecond <= best.dist * best.dist) {
      closestPointWalk(tree, second, p, best);
    }
    return;
  }

  tree.ensure_node_tris(node);
  if (!node->data) {
    return;
  }
  Mesh *m = node->data->m;
  for (spatial::NodeTri &tri : node->data->tris) {
    math::float3 a = m->v.co[m->c.v[tri.c[0]]];
    math::float3 b = m->v.co[m->c.v[tri.c[1]]];
    math::float3 c = m->v.co[m->c.v[tri.c[2]]];
    math::float3 cp = math::closestPointOnTri(p, a, b, c);
    float d = (cp - p).length();
    if (d < best.dist) {
      best.dist = d;
      best.point = cp;
      best.face = tri.f;
      best.hit = true;
      best.tri_c[0] = tri.c[0];
      best.tri_c[1] = tri.c[1];
      best.tri_c[2] = tri.c[2];
      best.bary = closestPointBary(cp, a, b, c);
    }
  }
}

} // namespace detail

/* Nearest surface point to `p` over the tree's triangles. BVH-pruned
 * (nearer-child-first), so amortized O(log n) per query on a static tree —
 * the M6 reprojection of every output vertex. */
static inline ClosestPointResult findClosestPoint(spatial::SpatialTree &tree,
                                                   const math::float3 &p)
{
  ClosestPointResult best;
  detail::closestPointWalk(tree, tree.getRoot(), p, best);
  return best;
}

} // namespace sculptcore::mesh
