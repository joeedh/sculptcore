#pragma once

#include "mesh/mesh.h"
#include "mesh/mesh_topo_cache.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdint>
#include <utility>

/**
 * Cavity automasking (documentation/plans/2026-07-14-2007-cavity-automasking.md).
 *
 * A per-vertex, per-stroke scalar derived from local surface convexity that
 * scales the *effective brush strength* — Blender's `factor_get()` idea, distinct
 * from the painted `mask` attribute layer. The raw estimate is a cheap
 * curvature-ish heuristic: BFS-blur the 1-ring adjacency into an inner-ring and a
 * wider-ring averaged position/normal and diff them. It is computed on the host,
 * cached per vertex per stroke, and both the CPU and GPU strength paths read the
 * cached value, so the two backends stay bit-for-bit equal.
 */
namespace sculptcore::brush {
using litestl::math::float3;

/** Cavity settings resolved from the brush once per stroke. */
inline constexpr int kCavityCurveSize = 256;

struct CavityParams {
  bool enabled = false;
  // Number of BFS blur rings; the walk reaches `blur_steps + 1` rings so the
  // wide average includes one ring beyond the inner subset.
  int blur_steps = 2;
  // User strength; scaled by an arbitrary 50x in the remap (Blender parity).
  float factor = 1.0f;
  // Flip the mask (mask concavities instead of convexities).
  bool inverted = false;
  // Optional curve remap: when `use_curve`, the linear 0..1 factor is reshaped
  // through `curve_lut` (a `kCavityCurveSize`-entry LUT, e.g. Brush::cavity_curve),
  // applied before the inversion. Null LUT disables it. Because the GPU path packs
  // the same cavityFactor host-side, the curve applies identically on both
  // backends and parity holds.
  bool use_curve = false;
  const float *curve_lut = nullptr;
};

/** Clamped linear-interpolated sample of a `kCavityCurveSize` LUT at t in 0..1.
 * Mirrors Brush::falloffEval's Curve branch. */
inline float sampleCavityCurve(const float *lut, float t)
{
  float clamped = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  float scaled = clamped * float(kCavityCurveSize - 1);
  int i0 = int(scaled);
  if (i0 >= kCavityCurveSize - 1) {
    return lut[kCavityCurveSize - 1];
  }
  float frac = scaled - float(i0);
  return lut[i0] * (1.0f - frac) + lut[i0 + 1] * frac;
}

/** Reusable BFS scratch so the per-vertex fill allocates once per stroke, not
 * once per vertex. `visitStamp` is a monotonic-token visited set sized to the
 * vertex capacity; bumping `token` logically clears it in O(1). */
struct CavityScratch {
  litestl::util::Vector<uint32_t> visitStamp;
  uint32_t token = 0;
  litestl::util::Vector<int> frontier;
  litestl::util::Vector<int> nextFrontier;

  void ensure(int vertCap)
  {
    if (int(visitStamp.size()) < vertCap) {
      visitStamp.resize(vertCap);
      for (int i = 0; i < vertCap; i++) {
        visitStamp[i] = 0;
      }
    }
  }
};

/**
 * Raw signed local-convexity estimate at vertex `v`. BFS-walks the ring1 CSR out
 * to `blur_steps + 1` rings, accumulating a wide average (all visited) and an
 * inner average (depth <= blur_steps) of position and normal; returns
 * `dot(wideCo - innerCo, normalize(innerNo)) / meanRadius`. Sign encodes
 * convex/concave. Requires `m->topo_cache.ring1` current (caller ensures it) and
 * live `v.co` / `v.no`. Returns 0 (flat/neutral) for degenerate neighborhoods.
 */
inline float cavityRaw(mesh::Mesh *m, int v, int blur_steps, CavityScratch &scr)
{
  const mesh::VertNbrCSR &csr = m->topo_cache.ring1;
  if (int(csr.offsets.size()) <= v + 1) {
    return 0.0f;
  }

  scr.ensure(m->v.count);
  scr.token++;
  const uint32_t tok = scr.token;

  const float3 originCo = m->v.co[v];

  float3 wideCo{0, 0, 0}, wideNo{0, 0, 0};
  float3 innerCo{0, 0, 0}, innerNo{0, 0, 0};
  int nWide = 0, nInner = 0;
  float lenSum = 0.0f;

  auto visit = [&](int idx, int depth) {
    const float3 co = m->v.co[idx];
    wideCo += co;
    wideNo += m->v.no[idx];
    nWide++;
    lenSum += (co - originCo).length();
    if (depth <= blur_steps) {
      innerCo += co;
      innerNo += m->v.no[idx];
      nInner++;
    }
  };

  scr.frontier.clear();
  scr.visitStamp[v] = tok;
  scr.frontier.append(v);
  visit(v, 0);

  const int maxDepth = blur_steps + 1;
  for (int depth = 1; depth <= maxDepth; depth++) {
    scr.nextFrontier.clear();
    for (int u : scr.frontier) {
      uint32_t off = csr.offsets[u];
      uint32_t end = csr.offsets[u + 1];
      for (uint32_t k = off; k < end; k++) {
        int nb = csr.nbr_verts[k];
        if (scr.visitStamp[nb] == tok) {
          continue;
        }
        scr.visitStamp[nb] = tok;
        scr.nextFrontier.append(nb);
        visit(nb, depth);
      }
    }
    std::swap(scr.frontier, scr.nextFrontier);
    if (scr.frontier.size() == 0) {
      break;
    }
  }

  if (nWide == 0 || nInner == 0) {
    return 0.0f;
  }
  const float meanLen = lenSum / float(nWide);
  if (meanLen <= 1e-9f) {
    return 0.0f;
  }

  wideCo /= float(nWide);
  innerCo /= float(nInner);
  innerNo /= float(nInner);

  float3 nrm = innerNo;
  const float nlen = nrm.length();
  // Opposing normals can average to ~0; fall back to the vertex normal so the
  // projection stays well-defined.
  nrm = nlen > 1e-6f ? nrm / nlen : m->v.no[v];

  return (wideCo - innerCo).dot(nrm) / meanLen;
}

/**
 * Remap a raw signed cavity estimate to a 0..1 mask factor. Ported from Blender's
 * `calc_cavity_factor`: convex pushes toward 1, concave toward 0 (flipped when
 * inverted), centered at 0.5. The 50x is Blender's arbitrary strength scale.
 */
inline float cavityRemap(const CavityParams &p, float raw)
{
  float sign = raw < 0.0f ? -1.0f : 1.0f;
  float factor = std::fabs(raw) * p.factor * 50.0f;
  factor = factor * sign * 0.5f + 0.5f;
  factor = factor < 0.0f ? 0.0f : (factor > 1.0f ? 1.0f : factor);
  // Optional user curve reshapes the linear factor before the inversion, so the
  // curve is authored in un-inverted space (matches Blender's CAVITY_USE_CURVE).
  if (p.use_curve && p.curve_lut) {
    factor = sampleCavityCurve(p.curve_lut, factor);
  }
  return p.inverted ? 1.0f - factor : factor;
}

/** Full cavity factor at vertex `v`: raw estimate through the remap. */
inline float cavityFactor(mesh::Mesh *m, int v, const CavityParams &p, CavityScratch &scr)
{
  return cavityRemap(p, cavityRaw(m, v, p.blur_steps, scr));
}

} // namespace sculptcore::brush
