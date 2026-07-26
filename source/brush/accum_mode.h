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
 * - Additive: Blender "Accumulate off". The dab's displacement, measured from the
 *             frozen stroke-start base (`want - base`), is added to the live
 *             position; the footprint stays pinned to the original surface and
 *             repeated coverage sums with no height cap.
 * - Grab:  grab-class symmetry write-back (#35). The first image to write a vert
 *          in a dab re-bases it from orig (`live = want`); later images of the
 *          SAME dab add (`live += want - base`). First-touch is keyed on the
 *          per-dab stamp (ctx.dabGen / ctx.curDabGen) via grabClaimFirstTouch,
 *          so mirror-only verts re-base every dab (no cross-dab accumulation)
 *          while shared seam verts still get orig + Σ disp_i within one dab. */
enum class AccumKind { Live, Additive, Grab };

/** Accumulate (Blender "Accumulate on"): reads and writes the live position;
 * neighbors read the Jacobi snapshot. This is the historical behavior. */
struct AccumLive {
  static constexpr bool is_accum_mode = true;
  static constexpr bool reads_base = false;
  static constexpr AccumKind kind = AccumKind::Live;
  template <class Ctx> static float3 neighborCo(Ctx &ctx, int nb)
  {
    return (*ctx.co_prev)[nb];
  }
};

/** Neighbor lookup shared by the from-base modes. Returns by value because the
 * displacement path *derives* the base; see the CoProxy note below.
 *
 * Displacement path (`.brush.disp.*`): `co_prev[nb] - disp[nb]`, the Jacobi
 * snapshot minus what the brush put there. The Jacobi snapshot, not live `co`,
 * keeps the stencil order-independent and GPU-parity-exact. An untouched
 * neighbor has `disp == 0`, so this needs no fallback branch.
 *
 * Legacy path (`.brush.orig.co`, valid iff origGen[nb] == strokeGen): the
 * absolute stroke-start position, falling back to the Jacobi pos for verts not
 * stamped this stroke — which mixes frozen and live positions in one stencil. */
struct OrigNbrBase {
  template <class Ctx> static float3 neighborCo(Ctx &ctx, int nb)
  {
    // The two paths are exclusive: a non-null dispVec means the whole command
    // is on the displacement base, so never mix in an absolute orig snapshot.
    if (ctx.dispVec) {
      if (ctx.dispGen->safe_get(nb) == int(ctx.strokeGen)) {
        return (*ctx.co_prev)[nb] - ctx.dispVec->safe_get(nb);
      }
      return (*ctx.co_prev)[nb];
    }
    if (ctx.origGen && ctx.origCo &&
        ctx.origGen->safe_get(nb) == int(ctx.strokeGen)) {
      return (*ctx.origCo)[nb];
    }
    return (*ctx.co_prev)[nb];
  }
};

/** Non-accumulate (Blender "Accumulate off"): from-original with additive
 * write-back. Displacement is measured from the frozen stroke-start position and
 * added to the live position each dab, so the brush footprint stays pinned to the
 * original surface and repeated coverage sums without a height cap. */
struct AccumOrig : OrigNbrBase {
  static constexpr bool is_accum_mode = true;
  static constexpr bool reads_base = true;
  static constexpr AccumKind kind = AccumKind::Additive;
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
/** Grab-class per-dab first-touch arbitration (AccumKind::Grab). Returns true and
 * stamps `ctx.curDabGen` onto vert `v` if this is the first image to write `v`
 * this dab (so it re-bases absolutely); returns false if `v` was already written
 * this dab (a later image must add). Defined inline in brush_executor.h. Always
 * true when the dab-gen stamp is absent (single-image strokes). */
bool grabClaimFirstTouch(const CommandExecutor &exec, int v);

/** Read-base / write-live proxy standing in for a vertex's `v.co` inside a
 * generated kernel. Under AccumOrig, reads return the base position until the
 * first write; a write computes from that base and latches to live, so
 * subsequent reads in the same kernel see the new value. Under AccumLive it is
 * a thin reference to the live position. The explicit LHS arithmetic operators
 * are required because litestl's Vec arithmetic operators are members (so
 * `proxy - float3` would otherwise need an implicit conversion the compiler
 * won't chain through the member operator).
 *
 * `base` is held **by value**: on the displacement path it is derived
 * (`co - disp`), not stored, so there is no pointer to hold. Caching it across
 * the dab is exact rather than approximate — every write moves `live` and
 * `disp` by the same delta, so their difference is invariant.
 *
 * The write-back depends on AccMode::kind (see AccumKind). Additive (AccumOrig)
 * matches Blender's "Accumulate off": the dab's displacement (`want - base`) is
 * added to the live position, so the footprint stays pinned to the base surface
 * and repeated coverage sums with no height cap. Grab (AccumOrigGrab) writes
 * `live = want` on the first image to touch a vert this dab (re-base, follows
 * the cursor) and `live += want - base` on later images of the same dab
 * (symmetry summation), arbitrated by grabClaimFirstTouch. Mirrored in WGSL by
 * emit_wgsl.cc's write-back.
 *
 * When `disp` is set (the displacement path), the same delta that moves `live`
 * is accumulated into it — the §2 invariant: whoever moves a vertex as brush
 * displacement adds the same delta to `disp`. */
template <class AccMode> struct CoProxy {
  float3 &live;
  float3 base;
  const CommandExecutor *exec = nullptr;
  mesh::AttrData<float3> *disp = nullptr;
  int v = -1;
  bool written = false;

  float3 cur() const { return (AccMode::reads_base && !written) ? base : live; }
  operator float3() const { return cur(); }

  void commit(const float3 &want)
  {
    if constexpr (AccMode::kind == AccumKind::Grab) {
      // First image to write this vert this dab re-bases it (follows the
      // cursor); a later image of the same dab adds its displacement-from-base
      // (`want - base`, cur() returns base pre-write) so seam verts sum. The
      // re-base is written as a delta so `disp` stays in step with `live`.
      // CLAUDENOTE(M5): first-touch is `want - live` (i.e. exactly the old
      // `live = want`) to keep grab bit-identical while it stays on the legacy
      // orig path. Plan §4.2 wants `want - base` once grab moves onto disp;
      // the two agree only when live == base, so flipping it is M5's job.
      float3 d = (exec && grabClaimFirstTouch(*exec, v)) ? want - live : want - base;
      live += d;
      if (disp) {
        (*disp)[v] += d;
      }
    } else if constexpr (AccMode::kind == AccumKind::Additive) {
      // Blender "Accumulate off": add the dab's displacement, measured from the
      // base (`want - base`, since cur() returns base until the first write), to
      // the live position. The footprint stays pinned to the base surface;
      // repeated coverage sums with no height cap. A no-write kernel path leaves
      // want == base, so the added delta is zero.
      float3 d = want - base;
      live += d;
      if (disp) {
        (*disp)[v] += d;
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
