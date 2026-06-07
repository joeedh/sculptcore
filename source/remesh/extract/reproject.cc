#include "remesh/extract/reproject.h"

#include "mesh/utils/closest_point.h"
#include "spatial/spatial.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

namespace sculptcore::remesh {

using litestl::math::float3;
using litestl::util::Vector;
using sculptcore::mesh::ClosestPointResult;
using sculptcore::mesh::findClosestPoint;
using sculptcore::mesh::Mesh;

ReprojectStats reprojectToSurface(Mesh &out, Mesh &input, const ReprojectParams &params)
{
  ReprojectStats stats;
  out.thawTopo();
  if (out.v.count == 0) return stats;

  spatial::SpatialTree tree(&input);
  tree.buildAll();

  const bool smoothing = params.smooth_iterations > 0 && params.smooth_lambda > 0.0f;

  int cap = int(out.v.capacity());
  Vector<Vector<int>> nbrs;  // output-mesh 1-ring per vertex (smoothing only)
  Vector<char> isBndV;       // boundary-loop vertex flag (pinned while smoothing)
  Vector<float3> np;         // Jacobi scratch for a smoothing pass
  if (smoothing) {
    nbrs.resize(cap);
    isBndV.resize(cap);
    np.resize(cap);
    for (int i = 0; i < cap; i++) isBndV[i] = 0;
    if (params.pin_boundary) {
      auto isBndE = [&](int e) {
        int c0 = out.e.c[e];
        if (c0 == ELEM_NONE) return true;
        int r = 0, cc = c0;
        do { r++; cc = out.c.radial_next[cc]; } while (cc != c0 && r < 100);
        return r == 1;
      };
      for (int e : out.e)
        if (isBndE(e)) { isBndV[out.e.vs[e][0]] = 1; isBndV[out.e.vs[e][1]] = 1; }
    }
    for (int e : out.e) {
      nbrs[out.e.vs[e][0]].append(out.e.vs[e][1]);
      nbrs[out.e.vs[e][1]].append(out.e.vs[e][0]);
    }
  }

  int iters = params.iterations < 1 ? 1 : params.iterations;
  for (int it = 0; it < iters; it++) {
    if (smoothing) {
      for (int s = 0; s < params.smooth_iterations; s++) {
        for (int v : out.v) np[v] = out.v.co[v];
        for (int v : out.v) {
          if (isBndV[v] || nbrs[v].size() == 0) continue;
          float3 avg(0, 0, 0);
          for (int w : nbrs[v]) avg = avg + out.v.co[w];
          avg = avg * (1.0f / float(nbrs[v].size()));
          float3 cur = out.v.co[v];
          np[v] = cur + (avg - cur) * params.smooth_lambda;
        }
        for (int v : out.v) out.v.co[v] = np[v];
      }
    }

    bool last = it == iters - 1;
    double sum = 0.0;
    float mx = 0.0f;
    int n = 0;
    for (int v : out.v) {
      ClosestPointResult r = findClosestPoint(tree, out.v.co[v]);
      if (!r.hit) continue;
      if (last) {
        sum += r.dist;
        if (r.dist > mx) mx = r.dist;
        n++;
      }
      out.v.co[v] = r.point;
    }
    if (last) {
      stats.num_verts = n;
      stats.max_dist = mx;
      stats.mean_dist = n ? float(sum / n) : 0.0f;
    }
  }

  out.recalc_normals();
  return stats;
}

} // namespace sculptcore::remesh
