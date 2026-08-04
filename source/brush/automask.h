#pragma once

#include "brush.h"
#include "mesh/mesh.h"
#include "mesh/mesh_topo_cache.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdint>
#include <utility>

/**
 * Automasking: per-vertex 0..1 scalars that scale the *effective brush
 * strength* — Blender's `factor_get()` idea, distinct from the painted `mask`
 * attribute layer. Two contributors with deliberately different evaluation
 * models:
 *
 *  - Cavity (documentation/plans/2026-07-14-2007-cavity-automasking.md) is
 *    CACHED per vertex per stroke in `.brush.automask.cavity` (keyed by
 *    `strokeGen` via `.brush.automask.gen`, first dab to reach a vertex wins;
 *    freshly split dyntopo verts miss the stamp and fill on first touch). The
 *    BFS ring-blur is too expensive per dab, and freezing the estimate at
 *    first contact keeps the mask from chasing the surface it is deforming.
 *    The BFS needs the ring1 CSR, ensured live before the per-dab topology
 *    freeze; a frozen-topo dab that cannot get one simply sits cavity out.
 *  - View normal (documentation/plans/2026-07-25-1138-view-normal-automasking.md)
 *    is DYNAMIC: fade geometry whose normal turns edge-on to the camera
 *    (optionally culling back faces). viewNormalFactor is a few flops, so
 *    CommandCtx::strength evaluates it fresh against each vertex's LIVE normal
 *    on every call, from ViewNormalParams resolved once per dab — no cache, no
 *    stamp ordering, no per-vertex ray history to go stale. Every symmetry
 *    image shares the SAME stroke-pinned camera ray: the mask is
 *    camera-relative and the camera doesn't mirror. (The original design
 *    cached a per-stroke product with per-image reflected rays; first-image-
 *    wins stamping then mixed opposed rays in leaf-sized blocks — the
 *    node-boundary tearing.)
 *
 * GPU strokes read one packed per-vertex buffer (binding 24) holding
 * cavity x viewNormal, built by packAutomask at beginStroke. That static pack
 * matches the CPU's dynamic evaluation because a GPU stroke's normal buffer is
 * itself stroke-static (the CPU mesh syncs at stroke end).
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
 * Raw signed local-convexity estimate at vertex `v`, generic over the geometry
 * source (grids-native brush path): `src` supplies `co(v)` / `no(v)` reads, a
 * `neighbors(v)` 1-ring span, and `vertCap()`. BFS-walks the ring out to
 * `blur_steps + 1` rings, accumulating a wide average (all visited) and an
 * inner average (depth <= blur_steps) of position and normal; returns
 * `dot(wideCo - innerCo, normalize(innerNo)) / meanRadius`. Sign encodes
 * convex/concave. Returns 0 (flat/neutral) for degenerate neighborhoods.
 */
template <class Src>
inline float cavityRawT(const Src &src, int v, int blur_steps, CavityScratch &scr)
{
  scr.ensure(src.vertCap());
  scr.token++;
  const uint32_t tok = scr.token;

  const float3 originCo = src.co(v);

  float3 wideCo{0, 0, 0}, wideNo{0, 0, 0};
  float3 innerCo{0, 0, 0}, innerNo{0, 0, 0};
  int nWide = 0, nInner = 0;
  float lenSum = 0.0f;

  auto visit = [&](int idx, int depth) {
    const float3 co = src.co(idx);
    wideCo += co;
    wideNo += src.no(idx);
    nWide++;
    lenSum += (co - originCo).length();
    if (depth <= blur_steps) {
      innerCo += co;
      innerNo += src.no(idx);
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
      for (int nb : src.neighbors(u)) {
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
  nrm = nlen > 1e-6f ? nrm / nlen : src.no(v);

  return (wideCo - innerCo).dot(nrm) / meanLen;
}

/** cavityRawT source over a mesh's topo-cache ring1 + live co/no. */
struct MeshCavitySrc {
  mesh::Mesh *m;
  int vertCap() const
  {
    return m->v.count;
  }
  float3 co(int v) const
  {
    return m->v.co[v];
  }
  float3 no(int v) const
  {
    return m->v.no[v];
  }
  std::span<const int> neighbors(int v) const
  {
    // Non-const data(): litestl Vector has no const accessor; read-only here.
    mesh::VertNbrCSR &csr = m->topo_cache.ring1;
    uint32_t off = csr.offsets[v];
    return std::span<const int>(csr.nbr_verts.data() + off, csr.offsets[v + 1] - off);
  }
};

/** The historical mesh entry: cavityRawT over the topo-cache ring1. Requires
 * `m->topo_cache.ring1` current (caller ensures it) and live `v.co` / `v.no`. */
inline float cavityRaw(mesh::Mesh *m, int v, int blur_steps, CavityScratch &scr)
{
  if (int(m->topo_cache.ring1.offsets.size()) <= v + 1) {
    return 0.0f;
  }
  return cavityRawT(MeshCavitySrc{m}, v, blur_steps, scr);
}

/**
 * Remap a raw signed cavity estimate to a 0..1 mask factor. Ported from Blender's
 * `calc_cavity_factor`: concave pushes toward 1 and convex toward 0, so the brush
 * effect stays in cavities (flipped when inverted). Flat surfaces read exactly the
 * 0.5 center. The 50x is Blender's arbitrary strength scale.
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

/** View-normal settings resolved from the brush once per stroke.
 * Defaults are brush.h's kViewNormal*Default (90° limit, 25° ramp). */
struct ViewNormalParams {
  bool enabled = false;
  // Unit eye->surface ray in *object* space — the same space as Mesh::v.no.
  // Host-set per dab from PaintSample::viewvec, reflected under symmetry.
  float3 view_dir{0, 0, -1};
  // Also zero anything facing away from the view, instead of fading back faces
  // symmetrically with front ones.
  bool cull_backfaces = false;
  // Angle off head-on at which the factor hits 0, and the width of the ramp
  // leading up to it. Both radians; falloff <= 0 makes it a hard cutoff.
  float limit = kViewNormalLimitDefault;
  float falloff = kViewNormalFalloffDefault;
};

/**
 * View-normal mask factor for a vertex normal `no`. Returns 1 head-on, ramping
 * to 0 as the normal turns `p.limit` off the view ray. With `cull_backfaces`
 * off the sign of the dot is discarded, so a back face fades exactly like the
 * front face at the same angle; with it on the sign is kept, which puts every
 * away-facing normal past `limit` and reads 0 — culling falls out of the same
 * expression. Neither `no` nor `p.view_dir` need be unit length; a degenerate
 * one disables the mask (returns 1) instead of masking the whole stroke out.
 */
inline float viewNormalFactor(const float3 &no, const ViewNormalParams &p)
{
  const float nlen = no.length();
  const float vlen = p.view_dir.length();
  if (nlen <= 1e-9f || vlen <= 1e-9f) {
    return 1.0f;
  }
  float d = -no.dot(p.view_dir) / (nlen * vlen);
  if (!p.cull_backfaces) {
    d = std::fabs(d);
  }
  d = d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d);

  const float angle = std::acos(d);
  if (angle >= p.limit) {
    return 0.0f;
  }
  if (p.falloff <= 1e-6f) {
    return 1.0f;
  }
  const float rampStart = p.limit - p.falloff;
  if (angle <= rampStart) {
    return 1.0f;
  }
  return (p.limit - angle) / p.falloff;
}

/** Resolve a brush's view-normal settings. The one place the CPU executor and
 * `packAutomask` share, so the two backends can't drift. */
inline ViewNormalParams viewNormalParamsFor(const Brush &brush)
{
  ViewNormalParams p;
  p.enabled = brush.automask_view_normal;
  p.view_dir = brush.viewDir;
  p.cull_backfaces = brush.cull_backfaces;
  p.limit = brush.view_normal_limit;
  p.falloff = brush.view_normal_falloff;
  return p;
}

} // namespace sculptcore::brush
