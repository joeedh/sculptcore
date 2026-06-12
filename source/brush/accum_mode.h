#pragma once

#include "litestl/math/vector.h"
#include "mesh/attribute.h"
#include <cmath>
#include <concepts>

// Non-accumulate brush mode (see plans/nonAccumMode.md). Within one stroke,
// non-accumulate brushes measure deformation from each vertex's stroke-start
// position rather than its current (already-deformed) position, so repeated
// passes converge instead of stacking. The mode is selected as a policy
// template (AccumLive / AccumOrig) chosen once per command instantiation —
// no per-vertex branch — mirroring the NbrSource policy in brush_concepts.h.

namespace sculptcore::brush {
using litestl::math::float3;

template <typename T>
concept AccumMode = requires {
  { T::is_accum_mode } -> std::convertible_to<bool>;
};

// Accumulate (Blender "Accumulate on"): reads and writes the live position;
// neighbors read the Jacobi snapshot. This is the historical behavior.
struct AccumLive {
  static constexpr bool is_accum_mode = true;
  static constexpr bool reads_base = false;
  template <class Ctx> static const float3 &neighborCo(Ctx &ctx, int nb)
  {
    return (*ctx.co_prev)[nb];
  }
};

// Non-accumulate: reads the stroke-start position cached in `.brush.orig.co`
// (valid iff origGen[v] == strokeGen), falling back to the live/Jacobi pos for
// verts not stamped this stroke (e.g. neighbors outside the brush region).
struct AccumOrig {
  static constexpr bool is_accum_mode = true;
  static constexpr bool reads_base = true;
  template <class Ctx> static const float3 &neighborCo(Ctx &ctx, int nb)
  {
    if (ctx.origGen && ctx.origCo &&
        ctx.origGen->safe_get(nb) == int(ctx.strokeGen)) {
      return (*ctx.origCo)[nb];
    }
    return (*ctx.co_prev)[nb];
  }
};

struct CommandExecutor;
// Falloff *fraction* of the active dab at `co` (falloffEval only — no
// strength/mask/texture). Defined inline in brush_executor.h.
float dabFalloffFraction(const CommandExecutor &exec, const float3 &co);

// Read-base / write-live proxy standing in for a vertex's `v.co` inside a
// generated kernel. Under AccumOrig, reads return the stroke-start position
// until the first write; a write computes from that base and latches to live,
// so subsequent reads in the same kernel see the new value. Under AccumLive it
// is a thin reference to the live position. The explicit LHS arithmetic
// operators are required because litestl's Vec arithmetic operators are members
// (so `proxy - float3` would otherwise need an implicit conversion the compiler
// won't chain through the member operator).
//
// AccumOrig writes accumulate like Blender's Layer brush: each dab's delta is
// *added* to the displacement already applied (live - base, valid because
// dyntopo coherence moves orig_co in lockstep), and the total is clamped to
// the dab's no-falloff displacement |delta|/w (w = falloff fraction at base).
// Falloff thus controls build-up *rate*, not final height, so a scrubbed
// stroke builds a uniform layer instead of a falloff-shaped dome — and a
// moving stroke's trailing edge can never snap back toward base. The cap
// keeps strength/mask/texture (they scale |delta| but not w), so a mask
// proportionally lowers the layer height. Mirrored in WGSL by emit_wgsl.cc's
// write-back.
template <class AccMode> struct CoProxy {
  float3 &live;
  const float3 *basePtr;
  const CommandExecutor *exec = nullptr;
  bool written = false;

  float3 cur() const { return (AccMode::reads_base && !written) ? *basePtr : live; }
  operator float3() const { return cur(); }

  void commit(const float3 &want)
  {
    if constexpr (AccMode::reads_base) {
      const float3 d_cand = want - cur();
      const float candSq = d_cand.dot(d_cand);
      if (candSq != 0.0f) {
        const float3 d_prev = live - *basePtr;
        float3 acc = d_prev + d_cand;
        const float prevSq = d_prev.dot(d_prev);
        // Cap at the no-falloff displacement; never below what's already
        // applied (a weak or no-op later dab must not erode the layer).
        float capSq = prevSq;
        const float w = exec ? dabFalloffFraction(*exec, *basePtr) : 0.0f;
        if (w > 1e-6f) {
          capSq = std::fmax(candSq / (w * w), prevSq);
        }
        const float accSq = acc.dot(acc);
        if (accSq > capSq) {
          acc *= std::sqrt(capSq / accSq);
        }
        live = *basePtr + acc;
      }
    } else {
      live = want;
    }
    written = true;
  }

  CoProxy &operator=(const float3 &r)
  {
    commit(r);
    return *this;
  }
  CoProxy &operator+=(const float3 &r)
  {
    commit(cur() + r);
    return *this;
  }
  CoProxy &operator-=(const float3 &r)
  {
    commit(cur() - r);
    return *this;
  }

  float3 operator+(const float3 &r) const { return cur() + r; }
  float3 operator-(const float3 &r) const { return cur() - r; }
  float3 operator*(const float3 &r) const { return cur() * r; }
  float3 operator/(const float3 &r) const { return cur() / r; }
  float3 operator*(float r) const { return cur() * r; }
  float3 operator/(float r) const { return cur() / r; }
};

} // namespace sculptcore::brush
