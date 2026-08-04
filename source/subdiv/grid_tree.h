#pragma once

/** Spatial structure over a GridLevelDomain (grids-native brush path, G1).
 *
 * A leaf is a cluster of WHOLE grids — same-cage-face grids first, adjacent
 * faces merged toward a per-leaf vert target. Built once per level; multires
 * topology never changes, so there are no splits or merges, ever. Each level
 * vert is OWNED by exactly one leaf (the leaf holding its canonical owning
 * grid), which is the brush-iteration unit; a leaf's AABB covers every lattice
 * vert of its grids (a superset of the owned set), so queries stay
 * conservative. Sphere queries are a flat scan over leaf AABBs — a few
 * thousand leaves at production levels, microseconds; castRay walks candidate
 * leaves' cells as bilinear quads split into two triangles (buildLevelTopo's
 * winding), returning the same hit shape as spatial::CastRayIsect. */

#include "litestl/math/aabb.h"
#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <span>

namespace sculptcore::subdiv {

struct GridLevelDomain;

struct GridRayHit {
  using float3 = litestl::math::float3;
  using float2 = litestl::math::float2;

  float3 p;
  float3 normal;
  float t = 0.0f;
  float2 uv; // barycentric weights of the hit tri's first two verts
  int leaf = -1;
  int grid = -1;
  int cellU = 0, cellV = 0;
  /** 0: cell tri (a,b,c), 1: (a,c,d) — levelTriIndicesOut's split order. */
  int cellTri = 0;
  int nearestVert = -1;
};

struct GridTree {
  using float3 = litestl::math::float3;
  using AABB = litestl::math::AABB<float3>;
  template <typename T> using Vector = litestl::util::Vector<T>;

  struct Leaf {
    Vector<int> grids;
    /** Verts whose canonical owning grid is in `grids` — each level vert
     * appears in exactly one leaf's list. */
    Vector<int> ownedVerts;
    AABB aabb;
  };

  /** Cluster `d`'s grids into leaves of ~`leafVertTarget` lattice verts
   * (<= 0 uses the default) and build bounds. */
  void build(GridLevelDomain &d, int leafVertTarget = 0);

  void refreshAllBounds();
  /** Recompute the AABBs of `leafIds` (a stroke's touched-leaf set). */
  void refreshBounds(std::span<const int> leafIds);

  /** Leaf indices whose AABB intersects the sphere. Returns any hit. */
  bool query(const float3 &co, float radius, Vector<int> &out) const;

  /** Closest forward hit over all leaves; false when nothing is hit. */
  bool castRay(const float3 &orig, const float3 &dir, GridRayHit &out) const;

  Vector<Leaf> leaves;
  Vector<int> leafOfGrid; // grid id -> leaf index
  Vector<int> leafOfVert; // vert id -> owning leaf index

  static constexpr int kDefaultLeafVertTarget = 512;

private:
  void leafBounds(Leaf &leaf);

  GridLevelDomain *d_ = nullptr;
};

} // namespace sculptcore::subdiv
