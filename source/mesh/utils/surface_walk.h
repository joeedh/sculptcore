#pragma once

/* Greedy closest-point walk over face adjacency: from a seed face, repeatedly
 * evaluate every face incident to the current best face's vertices and move to
 * whichever improves the closest-point distance, stopping at a local minimum.
 * O(ring) per step with no acceleration structure — the cheap counterpart to
 * closest_point.h's BVH query when a good seed is known (Tier 9g anchors).
 * Requires frozen topology; ngons are evaluated as corner fans. */

#include "../mesh.h"
#include "../mesh_base.h"

#include "litestl/math/geom.h"
#include "litestl/math/vector.h"

#include <cmath>
#include <limits>

namespace sculptcore::mesh {

struct SurfaceWalkResult {
  bool hit = false;
  int face = -1;               // mesh face index holding the closest point
  int tri_c[3] = {-1, -1, -1}; // winning fan triangle's mesh corners
  math::float3 point{0.0f, 0.0f, 0.0f};
  math::float3 bary{0.0f, 0.0f, 0.0f}; // barycentric weights in tri_c
  float dist = std::numeric_limits<float>::max();
  bool converged = false; // reached a local minimum (vs. ran out of steps)
  int steps = 0;          // ring sweeps performed
};

namespace detail_surface_walk {

/* Barycentric coordinates of p relative to triangle (a, b, c). */
static inline math::float3 walkBary(const math::float3 &p,
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

/* Evaluate face f's corner fan against p; returns true if it improved best. */
static inline bool
walkEvalFace(Mesh &m, int f, const math::float3 &p, SurfaceWalkResult &best)
{
  int c0 = m.l.c[m.f.l[f]];
  int ca = m.c.next[c0];
  int cb = m.c.next[ca];
  bool improved = false;
  int guard = 0;
  while (cb != c0 && guard++ < 64) {
    const math::float3 &a = m.v.co[m.c.v[c0]];
    const math::float3 &b = m.v.co[m.c.v[ca]];
    const math::float3 &c = m.v.co[m.c.v[cb]];
    math::float3 cp = math::closestPointOnTri(p, a, b, c);
    float d = (cp - p).length();
    if (d < best.dist) {
      best.dist = d;
      best.point = cp;
      best.face = f;
      best.hit = true;
      best.tri_c[0] = c0;
      best.tri_c[1] = ca;
      best.tri_c[2] = cb;
      best.bary = walkBary(cp, a, b, c);
      improved = true;
    }
    ca = cb;
    cb = m.c.next[cb];
  }
  return improved;
}

} // namespace detail_surface_walk

/* Walk from `start_face` toward the closest surface point to `p`. Each step
 * evaluates all faces incident to the current best face's vertices (its full
 * vertex 1-ring) and moves to the best; `converged` is set when a sweep finds
 * no improvement. An invalid/dead seed returns hit=false — callers fall back
 * to a global query. */
static inline SurfaceWalkResult
walkClosestPoint(Mesh &m, int start_face, const math::float3 &p, int max_steps = 32)
{
  using namespace detail_surface_walk;

  SurfaceWalkResult r;
  if (start_face < 0 || start_face >= int(m.f.capacity()) || m.f.freemap[start_face]) {
    return r;
  }

  walkEvalFace(m, start_face, p, r);
  if (!r.hit) {
    return r;
  }

  for (int step = 0; step < max_steps; step++) {
    int cur = r.face;
    bool improved = false;
    r.steps = step + 1;

    int c0 = m.l.c[m.f.l[cur]], cc = c0;
    int fguard = 0;
    do {
      int v = m.c.v[cc];
      int e0 = m.v.e[v];
      if (e0 != ELEM_NONE) {
        int ec = e0, guard = 0;
        do {
          int side = m.e.vs[ec][0] == v ? 0 : 1;
          int c1 = m.e.c[ec];
          if (c1 != ELEM_NONE) {
            int rc = c1, rguard = 0;
            do {
              int f = m.l.f[m.c.l[rc]];
              if (f != cur) {
                improved |= walkEvalFace(m, f, p, r);
              }
              rc = m.c.radial_next[rc];
            } while (rc != c1 && ++rguard < 64);
          }
          ec = diskEdge(m.e.disk[ec][side * 2 + 1]);
        } while (ec != e0 && ++guard < 256);
      }
      cc = m.c.next[cc];
    } while (cc != c0 && ++fguard < 64);

    if (!improved) {
      r.converged = true;
      break;
    }
  }

  return r;
}

} // namespace sculptcore::mesh
