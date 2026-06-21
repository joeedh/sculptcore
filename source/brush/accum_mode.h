#pragma once

#include "litestl/math/vector.h"
#include "mesh/attribute.h"
#include <cmath>
#include <concepts>

/** Non-accumulate brush mode (see plans/nonAccumMode.md). Within one stroke,
 * non-accumulate brushes measure deformation from each vertex's stroke-start
 * position rather than its current (already-deformed) position, so repeated
 * passes converge instead of stacking. The mode is selected as a policy
 * template (AccumLive / AccumOrig) chosen once per command instantiation —
 * no per-vertex branch — mirroring the NbrSource policy in brush_concepts.h. */

namespace sculptcore::brush {
using litestl::math::float3;

template <typename T>
concept AccumMode = requires {
  { T::is_accum_mode } -> std::convertible_to<bool>;
};

/** Write-back policy for a `reads_base` (from-original) accum mode. Selects how
 * CoProxy::commit turns the kernel's wanted position into the live position:
 * - Layer: Blender Layer-brush accumulation, capped at the no-falloff height
 *          (the historical non-accumulate behavior).
 * - Grab:  grab-class symmetry write-back (#35). The first image to write a vert
 *          in a dab re-bases it from orig (`live = want`); later images of the
 *          SAME dab add (`live += want - base`). First-touch is keyed on the
 *          per-dab stamp (ctx.dabGen / ctx.curDabGen) via grabClaimFirstTouch,
 *          so mirror-only verts re-base every dab (no cross-dab accumulation)
 *          while shared seam verts still get orig + Σ disp_i within one dab. */
enum class AccumKind { Live, Layer, Grab };

/** Accumulate (Blender "Accumulate on"): reads and writes the live position;
 * neighbors read the Jacobi snapshot. This is the historical behavior. */
struct AccumLive {
  static constexpr bool is_accum_mode = true;
  static constexpr bool reads_base = false;
  static constexpr AccumKind kind = AccumKind::Live;
  template <class Ctx> static const float3 &neighborCo(Ctx &ctx, int nb)
  {
    return (*ctx.co_prev)[nb];
  }
};

/** Neighbor lookup shared by the from-original modes: read the stroke-start
 * position cached in `.brush.orig.co` (valid iff origGen[v] == strokeGen),
 * falling back to the live/Jacobi pos for verts not stamped this stroke (e.g.
 * neighbors outside the brush region). */
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

/** Non-accumulate: from-original with Layer-brush write-back (capped accumulation). */
struct AccumOrig : OrigNbrBase {
  static constexpr bool is_accum_mode = true;
  static constexpr bool reads_base = true;
  static constexpr AccumKind kind = AccumKind::Layer;
};

/** Grab-class write-back (grab / kelvinlet, all symmetry images): from-original
 * with per-dab first-touch arbitration. The first image to write a vert in a dab
 * re-bases it absolutely from orig (`live = want`, follows the cursor); later
 * images of the same dab add their displacement-from-orig onto it, so a shared
 * seam vert gets orig + Σ disp_i and the mirror side never accumulates across
 * dabs. See AccumKind::Grab and grabClaimFirstTouch (#35). */
struct AccumOrigGrab : OrigNbrBase {
  static constexpr bool is_accum_mode = true;
  static constexpr bool reads_base = true;
  static constexpr AccumKind kind = AccumKind::Grab;
};

struct CommandExecutor;
/** Falloff *fraction* of the active dab at `co` (falloffEval only — no
 * strength/mask/texture). Defined inline in brush_executor.h. */
float dabFalloffFraction(const CommandExecutor &exec, const float3 &co);

/** Grab-class per-dab first-touch arbitration (AccumKind::Grab). Returns true and
 * stamps `ctx.curDabGen` onto vert `v` if this is the first image to write `v`
 * this dab (so it re-bases absolutely); returns false if `v` was already written
 * this dab (a later image must add). Defined inline in brush_executor.h. Always
 * true when the dab-gen stamp is absent (single-image strokes). */
bool grabClaimFirstTouch(const CommandExecutor &exec, int v);

/** Read-base / write-live proxy standing in for a vertex's `v.co` inside a
 * generated kernel. Under AccumOrig, reads return the stroke-start position
 * until the first write; a write computes from that base and latches to live,
 * so subsequent reads in the same kernel see the new value. Under AccumLive it
 * is a thin reference to the live position. The explicit LHS arithmetic
 * operators are required because litestl's Vec arithmetic operators are members
 * (so `proxy - float3` would otherwise need an implicit conversion the compiler
 * won't chain through the member operator).
 *
 * The write-back depends on AccMode::kind (see AccumKind). Layer (AccumOrig)
 * accumulates like Blender's Layer brush: each dab's delta is *added* to the
 * displacement already applied (live - base, valid because dyntopo coherence
 * moves orig_co in lockstep), clamped to the dab's no-falloff displacement
 * |delta|/w (w = falloff fraction at base) — falloff controls build-up *rate*,
 * not final height. Grab (AccumOrigGrab) writes `live = want` on the first image
 * to touch a vert this dab (re-base from orig, follows the cursor) and
 * `live += want - base` on later images of the same dab (symmetry summation),
 * arbitrated by grabClaimFirstTouch. Mirrored in WGSL by emit_wgsl.cc's
 * write-back. */
template <class AccMode> struct CoProxy {
  float3 &live;
  const float3 *basePtr;
  const CommandExecutor *exec = nullptr;
  int v = -1;
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
    } else if constexpr (AccMode::kind == AccumKind::Grab) {
      // First image to write this vert this dab re-bases it from orig (follows
      // the cursor); a later image of the same dab adds its displacement-from-
      // orig (`want - base`, cur() returns base pre-write) so seam verts sum.
      if (exec && grabClaimFirstTouch(*exec, v)) {
        live = want;
      } else {
        live += want - *basePtr;
      }
    } else {
      // Live: write the wanted position directly.
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
