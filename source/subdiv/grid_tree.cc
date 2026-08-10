#include "grid_tree.h"

#include "grid_domain.h"
#include "multires.h"

#include "litestl/math/geom.h"
#include "litestl/util/assert.h"

#include <algorithm>
#include <limits>

namespace sculptcore::subdiv {

using namespace litestl;
using litestl::math::float3;
using litestl::util::Assert;

/** The mesh tree's AABB padding rule (spatial.cc calc_eps_float3). */
static float3 boundsEps(const float3 &size)
{
  float3 eps = size * 0.001f;
  for (int i = 0; i < 3; i++) {
    eps[i] = std::min(eps[i], 0.00001f);
  }
  return eps;
}

void GridTree::build(GridLevelDomain &d, int leafVertTarget)
{
  d_ = &d;
  if (leafVertTarget <= 0) {
    leafVertTarget = kDefaultLeafVertTarget;
  }
  const GridsStore &store = d.multires()->store;
  int G = d.gridCount();
  int w = d.gridSide() + 1;
  int vertsPerGrid = w * w;

  // Same-face grid groups: RIGHT links cycle through a cage face's corners.
  Vector<int> faceOf;
  faceOf.resize(G);
  for (int g = 0; g < G; g++) {
    faceOf[g] = -1;
  }
  // Inline 8 per face: a cage face's corner cycle and its cross-edge neighbour
  // list are both face-valence sized, so ngons are the only heap case.
  using FaceList = litestl::util::Vector<int, 8>;
  Vector<FaceList> faceGrids;
  faceGrids.ensure_capacity(G);
  for (int g = 0; g < G; g++) {
    if (faceOf[g] >= 0) {
      continue;
    }
    int fid = int(faceGrids.size());
    FaceList cycle;
    int cur = g;
    do {
      faceOf[cur] = fid;
      cycle.append(cur);
      cur = store.link(cur, GRID_SIDE_RIGHT).grid;
    } while (cur >= 0 && cur != g && faceOf[cur] < 0);
    faceGrids.append(std::move(cycle));
  }
  int F = int(faceGrids.size());

  // Face adjacency through the cross-cage-edge links (LEFT/BOTTOM and their
  // mirror images), for cluster growth.
  Vector<FaceList> faceNbrs;
  faceNbrs.resize(F);
  for (int g = 0; g < G; g++) {
    for (int side = 0; side < 4; side++) {
      int og = store.link(g, side).grid;
      if (og < 0 || faceOf[og] == faceOf[g]) {
        continue;
      }
      faceNbrs[faceOf[g]].append_once(faceOf[og]);
    }
  }

  // Greedy face-BFS clustering: seed at the smallest unassigned face, grow
  // through adjacent faces until the vert target is met. Deterministic — the
  // frontier always pops its smallest face id.
  leaves.clear();
  leafOfGrid.resize(G);
  Vector<bool> assigned;
  assigned.resize(F);
  for (int f = 0; f < F; f++) {
    assigned[f] = false;
  }
  litestl::util::Vector<int, 64> frontier;
  for (int seed = 0; seed < F; seed++) {
    if (assigned[seed]) {
      continue;
    }
    Leaf leaf;
    leaf.grids.ensure_capacity(size_t(leafVertTarget / vertsPerGrid) + 1);
    leaf.ownedVerts.ensure_capacity(size_t(leafVertTarget));
    int leafVerts = 0;
    frontier.clear();
    frontier.append(seed);
    while (frontier.size() > 0 && leafVerts < leafVertTarget) {
      int best = 0;
      for (int i = 1; i < int(frontier.size()); i++) {
        if (frontier[i] < frontier[best]) {
          best = i;
        }
      }
      int f = frontier[best];
      frontier.remove_at(best);
      if (assigned[f]) {
        continue;
      }
      assigned[f] = true;
      for (int g : faceGrids[f]) {
        leafOfGrid[g] = int(leaves.size());
        leaf.grids.append(g);
      }
      leafVerts += int(faceGrids[f].size()) * vertsPerGrid;
      for (int nb : faceNbrs[f]) {
        if (!assigned[nb]) {
          frontier.append(nb);
        }
      }
    }
    leaves.append(std::move(leaf));
  }

  // Owned-vert partition: a vert belongs to the leaf holding its canonical
  // owning grid.
  leafOfVert.resize(d.vertCount());
  for (int v = 0; v < d.vertCount(); v++) {
    int li = leafOfGrid[d.vertGrid[v * 3]];
    leafOfVert[v] = li;
    leaves[li].ownedVerts.append(v);
  }

  refreshAllBounds();
}

void GridTree::leafBounds(Leaf &leaf)
{
  const auto &pos = d_->pos();
  int w = d_->gridSide() + 1;
  leaf.aabb.reset();
  for (int g : leaf.grids) {
    const int *gv = d_->gridVerts(g);
    for (int i = 0; i < w * w; i++) {
      leaf.aabb.add(pos[gv[i]]);
    }
  }
  if (!leaf.aabb.isEmpty()) {
    float3 eps = boundsEps(leaf.aabb.max - leaf.aabb.min);
    leaf.aabb.min -= eps;
    leaf.aabb.max += eps;
  }
}

void GridTree::refreshAllBounds()
{
  for (Leaf &leaf : leaves) {
    leafBounds(leaf);
  }
}

void GridTree::refreshBounds(std::span<const int> leafIds)
{
  for (int li : leafIds) {
    leafBounds(leaves[li]);
  }
}

bool GridTree::query(const float3 &co, float radius, Vector<int> &out) const
{
  bool any = false;
  for (int i = 0; i < int(leaves.size()); i++) {
    if (math::aabbSphereIsect(co, radius, leaves[i].aabb)) {
      out.append(i);
      any = true;
    }
  }
  return any;
}

bool GridTree::castRay(const float3 &orig, const float3 &dir, GridRayHit &out) const
{
  const auto &pos = d_->pos();
  int S = d_->gridSide(), w = S + 1;
  out.t = std::numeric_limits<float>::max();
  out.leaf = -1;

  int bestVerts[3] = {-1, -1, -1};
  for (int li = 0; li < int(leaves.size()); li++) {
    const Leaf &leaf = leaves[li];
    float tEnter;
    if (!math::aabbRayEnter(leaf.aabb, orig, dir, tEnter) || tEnter >= out.t) {
      continue;
    }
    for (int g : leaf.grids) {
      const int *gv = d_->gridVerts(g);
      for (int cv = 0; cv < S; cv++) {
        for (int cu = 0; cu < S; cu++) {
          int a = gv[cv * w + cu], b = gv[cv * w + cu + 1];
          int c = gv[(cv + 1) * w + cu + 1], dd = gv[(cv + 1) * w + cu];
          const int tris[2][3] = {{a, b, c}, {a, c, dd}};
          for (int k = 0; k < 2; k++) {
            math::RayTriIsect<float3> isect;
            if (!math::rayTriIsect(orig, dir, pos[tris[k][0]], pos[tris[k][1]],
                                   pos[tris[k][2]], isect))
            {
              continue;
            }
            if (isect.t > 0.0f && isect.t < out.t) {
              out.t = isect.t;
              out.uv = isect.uv;
              out.leaf = li;
              out.grid = g;
              out.cellU = cu;
              out.cellV = cv;
              out.cellTri = k;
              bestVerts[0] = tris[k][0];
              bestVerts[1] = tris[k][1];
              bestVerts[2] = tris[k][2];
            }
          }
        }
      }
    }
  }
  if (out.leaf < 0) {
    return false;
  }

  float w2 = 1.0f - out.uv[0] - out.uv[1];
  out.p = pos[bestVerts[0]] * out.uv[0] + pos[bestVerts[1]] * out.uv[1] +
          pos[bestVerts[2]] * w2;
  out.normal = d_->no[bestVerts[0]] * out.uv[0] + d_->no[bestVerts[1]] * out.uv[1] +
               d_->no[bestVerts[2]] * w2;
  out.normal.normalize();
  if (out.uv[0] >= out.uv[1] && out.uv[0] >= w2) {
    out.nearestVert = bestVerts[0];
  } else if (out.uv[1] >= w2) {
    out.nearestVert = bestVerts[1];
  } else {
    out.nearestVert = bestVerts[2];
  }
  return true;
}

} // namespace sculptcore::subdiv
