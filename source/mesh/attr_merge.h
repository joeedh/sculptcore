#pragma once

/* Per-layer merge policy for the topological operators that create or merge an
 * element from two sources (edge split's midpoint, edge collapse's survivor).
 *
 * The generic rule — lerp floats, copy src0 otherwise — is wrong for any layer
 * whose value is only meaningful together with a sibling column, the sculpt
 * brushes' lazily-materialized, generation-guarded stroke fields being the
 * motivating case: blending `.brush.disp.vec` against an *unstamped* endpoint
 * mixes in that endpoint's unmaterialized page default while `.brush.disp.gen`
 * is copied from the other one, so the result reads as a valid displacement the
 * surface never had. Such layers declare AttrMerge::CUSTOM and get
 * a handler that owns dst and its guard column together.
 *
 * Policies are keyed by layer NAME (resolveMergePolicy), stamped onto the
 * AttrRef by AttrGroup::ensure. Nothing is serialized: the loader rebuilds each
 * domain through ensure(), so the policy comes back on its own.
 */

#include "attribute.h"

#include "litestl/math/vector.h"

namespace sculptcore::mesh {
struct Mesh;

/** What a CUSTOM handler is told about the merge in progress. `merged_co` is the
 * position the operator will place dst at when that differs from the lerp of the
 * sources (edge collapse's placement); null for edge split, where dst's position
 * IS the lerp. `mesh` is null when the caller has no mesh to offer, in which case
 * a handler that needs live element data must fall back to defaultMerge. */
struct AttrMergeCtx {
  Mesh *mesh = nullptr;
  AttrGroup *grp = nullptr;
  int dst = 0;
  int src0 = 0;
  int src1 = 0;
  float t = 0.0f;
  const math::float3 *merged_co = nullptr;
  /* The sources' live position/normal, captured before the merge loop runs:
   * a collapse has dst == src0, so reading m.v.co in a handler could pick up a
   * value the loop's own "positions" layer already overwrote. Valid iff
   * `have_live` (i.e. iff `mesh` was supplied). */
  math::float3 src_co[2] = {};
  math::float3 src_no[2] = {};
  bool have_live = false;
};

/** Resolve a sibling layer in the same group, memoizing its index on `attr`
 * (revalidated by name). Null if the group has no such layer. */
AttrDataBase *
siblingLayer(AttrRef &attr, const AttrMergeCtx &ctx, AttrType type, const char *name);

/** Apply the DEFAULT/COPY_SRC0 rule to one layer. Exposed so a CUSTOM handler
 * can defer to it on the cases it does not special-case. */
void defaultMerge(AttrRef &attr, const AttrMergeCtx &ctx, bool copy_src0 = false);

} // namespace sculptcore::mesh
