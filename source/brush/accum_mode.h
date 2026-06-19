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

// Write-back policy for a `reads_base` (from-original) accum mode. Selects how
// CoProxy::commit turns the kernel's wanted position into the live position:
// - Layer:    Blender Layer-brush accumulation, capped at the no-falloff height
//             (the historical non-accumulate behavior).
// - Absolute: `live = want` — recompute the absolute position from orig each dab
//             (a grab follows the cursor). Resets prior displacement; used for
//             the primary symmetry pass so it re-bases every touched vert.
// - Add:      `live += want - base` — add this image's displacement-from-orig
//             onto the live value the primary pass already re-based; used for
//             mirror symmetry passes so shared verts get orig + Σ disp_i (#35).
enum class AccumKind { Live, Layer, Absolute, Add };

// Accumulate (Blender "Accumulate on"): reads and writes the live position;
// neighbors read the Jacobi snapshot. This is the historical behavior.
struct AccumLive {
  static constexpr bool is_accum_mode = true;
  static constexpr bool reads_base = false;
  static constexpr AccumKind kind = AccumKind::Live;
  template <class Ctx> static const float3 &neighborCo(Ctx &ctx, int nb)
  {
    return (*ctx.co_prev)[nb];
  }
};

// Neighbor lookup shared by the from-original modes: read the stroke-start
// position cached in `.brush.orig.co` (valid iff origGen[v] == strokeGen),
// falling back to the live/Jacobi pos for verts not stamped this stroke (e.g.
// neighbors outside the brush region).
struct OrigNbrBase {
  template <class Ctx> static const float3 &neighborCo(Ctx &ctx, int nb)
  {
    if (ctx.origGen && ctx.origCo &&
        ctx.origGen->safe_get(nb) == int(ctx.strokeGen)) {
      return (*ctx.origCo)[nb];
    }
    return (*ctx.co_prev)[nb];
  }
};

// Non-accumulate: from-original with Layer-brush write-back (capped accumulation).
struct AccumOrig : OrigNbrBase {
  static constexpr bool is_accum_mode = true;
  static constexpr bool reads_base = true;
  static constexpr AccumKind kind = AccumKind::Layer;
};

// Grab primary pass: from-original, absolute write (`live = want`). Each dab
// recomputes the full position from orig, so the deformation follows the cursor.
struct AccumOrigAbsolute : OrigNbrBase {
  static constexpr bool is_accum_mode = true;
  static constexpr bool reads_base = true;
  static constexpr AccumKind kind = AccumKind::Absolute;
};

// Grab mirror pass: from-original, additive write (`live += want - base`). Adds
// this image's displacement-from-orig onto the value the primary pass re-based.
struct AccumOrigAdd : OrigNbrBase {
  static constexpr bool is_accum_mode = true;
  static constexpr bool reads_base = true;
  static constexpr AccumKind kind = AccumKind::Add;
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
// The write-back depends on AccMode::kind (see AccumKind). Layer (AccumOrig)
// accumulates like Blender's Layer brush: each dab's delta is *added* to the
// displacement already applied (live - base, valid because dyntopo coherence
// moves orig_co in lockstep), clamped to the dab's no-falloff displacement
// |delta|/w (w = falloff fraction at base) — falloff controls build-up *rate*,
// not final height. Absolute (grab primary) writes `live = want`, recomputing
// the absolute position from orig each dab. Add (grab mirror) writes
// `live += want - base`, summing symmetry passes onto the re-based primary.
// Mirrored in WGSL by emit_wgsl.cc's write-back.
template <class AccMode> struct CoProxy {
  float3 &live;
  const float3 *basePtr;
  const CommandExecutor *exec = nullptr;
  bool written = false;

  float3 cur() const { return (AccMode::reads_base && !written) ? *basePtr : live; }
  operator float3() const { return cur(); }

  void commit(const float3 &want)
  {
    if constexpr (AccMode::kind == AccumKind::Layer) {
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
    } else if constexpr (AccMode::kind == AccumKind::Add) {
      // `want` was computed from the orig base (cur() returns base pre-write),
      // so `want - base` is this image's displacement-from-orig — add it onto
      // the value the primary pass already re-based to.
      live += want - *basePtr;
    } else {
      // Absolute (grab primary) and Live: write the wanted position directly.
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
