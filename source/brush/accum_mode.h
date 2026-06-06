#pragma once

#include "litestl/math/vector.h"
#include "mesh/attribute.h"
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

// Read-base / write-live proxy standing in for a vertex's `v.co` inside a
// generated kernel. Under AccumOrig, reads return the stroke-start position
// until the first write; a write computes from that base and latches to live,
// so subsequent reads in the same kernel see the new value. Under AccumLive it
// is a thin reference to the live position. The explicit LHS arithmetic
// operators are required because litestl's Vec arithmetic operators are members
// (so `proxy - float3` would otherwise need an implicit conversion the compiler
// won't chain through the member operator).
template <class AccMode> struct CoProxy {
  float3 &live;
  const float3 *basePtr;
  bool written = false;

  float3 cur() const { return (AccMode::reads_base && !written) ? *basePtr : live; }
  operator float3() const { return cur(); }

  CoProxy &operator=(const float3 &r)
  {
    live = r;
    written = true;
    return *this;
  }
  CoProxy &operator+=(const float3 &r)
  {
    live = cur() + r;
    written = true;
    return *this;
  }
  CoProxy &operator-=(const float3 &r)
  {
    live = cur() - r;
    written = true;
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
