#include "remesh/field/density.h"
#include "remesh/field/curvature.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"

#include "litestl/math/vector.h"
#include "litestl/util/boolvector.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <algorithm>
#include <cmath>

namespace sculptcore::remesh {

using litestl::math::float2;
using litestl::math::float3;
using litestl::util::BoolVector;
using litestl::util::Vector;
using sculptcore::mesh::AttrFlag;
using sculptcore::mesh::AttrType;
using sculptcore::mesh::BuiltinAttr;
using sculptcore::mesh::Mesh;

void generateAutoDensity(Mesh &m, const DensityParams &params)
{
  // Reuse the cross field's curvature if present (already Tier-2-smoothed);
  // otherwise compute it now with the same smoothing so density tracks the same
  // denoised field rather than re-injecting noise the cross field filtered out.
  if (!m.v.attrs.has(AttrType::FLOAT2, litestl::util::string(".remesh.v.k"))) {
    computeCurvature(m, CurvatureParams{params.curvature_smooth_iters,
                                        params.curvature_smooth_lambda});
  }

  BuiltinAttr<float2, ".remesh.v.k"> kval;
  BuiltinAttr<float, ".remesh.v.density", AttrFlag::TEMP> density;
  kval.ensure(m.v.attrs);
  density.ensure(m.v.attrs);

  const float L = params.target_edge_length > 1e-12f ? params.target_edge_length
                                                     : 1e-12f;
  const float dmin = params.density_min;
  const float dmax = params.density_max;

  for (int v : m.v) {
    float2 k = kval[v];
    float kmag = std::max(std::fabs(k[0]), std::fabs(k[1]));
    // Dimensionless curvature-vs-quad-size: s = k·L is the angular span one quad
    // covers. density = s² makes quad size ≈ curvature radius (size ∝
    // 1/sqrt(density) ⇒ L/sqrt(s²) = 1/k). Asset-scale independent.
    float s = kmag * L;
    float d = s * s;
    d = d < dmin ? dmin : (d > dmax ? dmax : d);
    density[v] = d;
  }
}

void limitDensityGradation(Mesh &m, float target_edge_length, float gradation,
                           int iters, float density_min, float density_max)
{
  if (gradation <= 0.0f) {
    return;
  }
  if (!m.v.attrs.has(AttrType::FLOAT,
                     litestl::util::string(".remesh.v.density"))) {
    return;
  }

  BuiltinAttr<float, ".remesh.v.density"> density;
  density.ensure(m.v.attrs);

  const float L = target_edge_length > 1e-12f ? target_edge_length : 1e-12f;
  /* Per-edge-hop ratio bound: across any single edge the goal length may grow by
   * at most beta, regardless of the edge's current world length. World-distance
   * (Alauzet) gradation is the wrong currency here — on a coarse input one long
   * edge legally spans a huge size step, so a per-edge cliff survives and the BK
   * band overlaps pathologically across it. */
  const float beta = 1.0f + gradation;

  const int vcap = int(m.v.capacity());
  Vector<float> h;
  h.resize(vcap);
  for (int v : m.v) {
    float d = density[v] > 1e-12f ? density[v] : 1e-12f;
    h[v] = L / std::sqrt(d);
  }

  // Worklist min-propagation: expand the edge set outward from every vertex,
  // relaxing each neighbor to h[v]·beta and re-queueing the ones that tightened.
  // Fine regions spread ring by ring with geometrically relaxed goals; only ever
  // lowers h (raises density). Terminates at the per-hop-Lipschitz envelope;
  // `iters` only caps total work (pops per vertex) as a safety valve.
  BoolVector<> queued;
  queued.resize(vcap);
  Vector<int> queue;
  queue.ensure_capacity(m.v.count);
  for (int v : m.v) {
    queue.append(v);
    queued.set(v, true);
  }
  size_t pop_cap = size_t(m.v.count) * size_t(iters > 0 ? iters : 10);
  for (size_t head = 0; head < queue.size() && pop_cap > 0; head++, pop_cap--) {
    int v = queue[head];
    queued.set(v, false);
    int e0 = m.v.e[v];
    if (e0 == ELEM_NONE) {
      continue;
    }
    float cand = h[v] * beta;
    int ec = e0;
    do {
      int vn = m.e.vs[ec][0] == v ? m.e.vs[ec][1] : m.e.vs[ec][0];
      if (cand < h[vn]) {
        h[vn] = cand;
        if (!queued.set(vn, true)) {
          queue.append(vn);
        }
      }
      int side = m.e.vs[ec][0] == v ? 0 : 1;
      ec = m.e.disk[ec][side * 2 + 1];
    } while (ec != e0);
  }

  for (int v : m.v) {
    float hv = h[v] > 1e-12f ? h[v] : 1e-12f;
    float d = (L / hv) * (L / hv);
    d = d < density_min ? density_min : (d > density_max ? density_max : d);
    density[v] = d;
  }
}

} // namespace sculptcore::remesh
