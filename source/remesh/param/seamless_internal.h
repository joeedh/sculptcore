#pragma once

/* Shared scaffolding for the seamless solve (M4) and the integer quantization
 * (M5). Both need the same cut graph, per-face gauge rotations, corner classes,
 * and base cotangent FEM system; this builds them once so the two passes stay in
 * lock-step (identical classes, gauge, pins -> identical (u,v) frame). Eigen is
 * used here, so this header is internal (included only by .cc files). */

#include "mesh/mesh.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include "eigen/include/eigen5/Eigen/Sparse"

#include <vector>

namespace sculptcore::remesh {

struct SeamlessParamParams;

// Path-compressing union-find over a fixed [0, n) index range.
struct UnionFind {
  litestl::util::Vector<int> parent;
  void init(int n)
  {
    parent.resize(n);
    for (int i = 0; i < n; i++) {
      parent[i] = i;
    }
  }
  int find(int x)
  {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  }
  void unite(int a, int b)
  {
    int ra = find(a), rb = find(b);
    if (ra != rb) {
      parent[ra] = rb;
    }
  }
};

// True iff e is a manifold interior edge; fa = face(e.c), fb = the radial face.
inline bool interiorEdge(mesh::Mesh &m, int e, int &fa, int &fb)
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

struct SeamlessSystem {
  int M = 0;           // number of corner classes (independent (u,v) variables)
  int num_corners = 0; // live corners visited

  // Per-element scratch (sized to capacity; indexed by raw element id).
  litestl::util::Vector<int> cornerClass;          // [ccap] class id or -1
  litestl::util::Vector<int> gauge;                // [fcap] per-face rotation R_f
  litestl::util::Vector<int> periodEC;             // [ecap] period jump fa->fb
  litestl::util::Vector<litestl::math::float3> FX; // [fcap] face frame axes
  litestl::util::Vector<litestl::math::float3> FY;
  litestl::util::Vector<litestl::math::float3> FN;

  // Base Dirichlet (cotan FEM) system over the M classes, BEFORE pins / eps:
  // the scalar Laplacian as triplets and the two target-gradient right-hand
  // sides. u and v share L; quantization couples them, so it re-expands L into a
  // 2M block system rather than solving twice.
  std::vector<Eigen::Triplet<double>> baseTrips;
  Eigen::VectorXd bu;
  Eigen::VectorXd bv;

  // One representative class per connected component of the cut mesh; pinning
  // these removes the per-component constant null space. Shared so M4 and M5 pin
  // identically.
  litestl::util::Vector<int> pinClass;
};

/* Ensure the cross field (computeCrossField if no .remesh.f.theta), build the cut
 * graph, gauge, corner classes, and the base FEM system into `out`. Thaws
 * topology + recomputes normals. Writes .remesh.e.period and .remesh.e.is_cut
 * (both TEMP). Returns false if the mesh is degenerate (no faces / no classes). */
bool buildSeamlessSystem(mesh::Mesh &m,
                         const SeamlessParamParams &params,
                         SeamlessSystem &out);

} // namespace sculptcore::remesh
