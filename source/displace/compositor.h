#pragma once

/** Sculpt-layer compositor (displacementAndSubSurf plan, workstream F1).
 *
 * Evaluates the layer stack — base surface + ordered DELTA vertex layers —
 * into the positions the spatial tree / draw / meshlog consume, which is
 * `v.co` itself: evaluated positions are authoritative, and the base is
 * implicit (`base(v) == co(v) − Σ enabled wᵢ·dᵢ(v)`). Storing the base that
 * way (rather than as a column) keeps every consumer of `v.co` unchanged and
 * makes undo unbreakable: meshlog restores co + layer columns atomically and
 * there is no third copy to desync. DELTA contributions are linear, so
 * dyntopo's attribute lerp keeps co/layers exactly consistent on split and
 * collapse, and stack order is immaterial (deltas commute).
 *
 * All mutations therefore maintain co incrementally (dab-region-scoped for
 * strokes via LayerEditScope, whole-mesh for settings changes) — the plan's
 * "incremental re-evaluation". TANGENT layers (subsurf multires, workstream
 * S) and the VDM clamp/predicate state (workstream V) extend this module.
 */

#include "litestl/math/vector.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"

#include <span>

namespace sculptcore::displace {
using litestl::math::float3;
namespace util = litestl::util;

/** One resolved sculpt layer: the settings row joined to its FLOAT3 column. */
struct LayerView {
  mesh::AttrData<float3> *data = nullptr;
  float weight = 1.0f;
  bool enabled = true;
  bool frozen = false;
  int settingsIdx = -1;
};

/** Resolve every sculpt layer of @p m in stack order. Layers whose settings
 * row has no matching VERTEX FLOAT3 column are skipped. */
util::Vector<LayerView> resolveStack(mesh::Mesh &m);

/** Resolve one layer by settings index; view.data == nullptr when invalid. */
LayerView resolveLayer(mesh::Mesh &m, int settingsIdx);

/** Region-scoped edit bracket for code that writes layer deltas directly (a
 * brush kernel, a test): begin() snapshots the layer's values over @p verts,
 * end() folds the change into evaluated positions
 * (`co += w·(d_new − d_old)` per vert). A frozen layer's writes are instead
 * reverted at end() (the settings row excludes it from editing); a disabled
 * layer's writes are kept but contribute nothing until it is re-enabled. */
struct LayerEditScope {
  /** Snapshot layer `layerName` over @p verts. Returns false (inert scope)
   * when the name has no settings row or no column. */
  bool begin(mesh::Mesh &m, const util::string &layerName, std::span<const int> verts);
  bool begin(mesh::Mesh &m, int settingsIdx, std::span<const int> verts);
  void end();

private:
  mesh::Mesh *m_ = nullptr;
  LayerView view_;
  util::Vector<int> verts_;
  util::Vector<float3> old_;
};

/** Make layer @p settingsIdx the edit target (V2 implicit-active model), or
 * pass -1 to clear it. While targeted, sculpting co IS editing the layer: its
 * delta is derived (d ≡ co − rest, rest snapshotted at activation into the
 * TEMP `.slayer.rest` column) and its stored column is stale until a fold.
 * Activation enables a disabled layer and pins its weight to 1 (folding at
 * weight w would divide by w); a frozen layer cannot be the target. Switching
 * away folds the outgoing layer and drops its rest snapshot. Returns the
 * resulting target index (-1 when deactivated or @p settingsIdx is invalid/
 * frozen). Callers own undo — replaying this through the same path on
 * undo/redo is self-consistent because folds are semantically no-ops. */
int setActiveEditLayer(mesh::Mesh &m, int settingsIdx);

/** Fold the edit target's delta from evaluated positions (d = co − rest) over
 * @p verts (empty = whole mesh). Idempotent; no-op without a target. */
void foldActiveLayer(mesh::Mesh &m, std::span<const int> verts = {});

/** Settings mutations that keep evaluated positions current (co += Δ over all
 * live verts). Mutating the edit target itself first clears the target (fold +
 * weight unpin is the caller's concern); mutating another layer mirrors its co
 * adjustment into the target's rest snapshot so derived deltas stay exact.
 * These do NOT bracket meshlog — an undoable caller wraps them in its own
 * step (the V5 app ToolOps do). */
void setLayerWeight(mesh::Mesh &m, int settingsIdx, float weight);
void setLayerEnabled(mesh::Mesh &m, int settingsIdx, bool enabled);
void setLayerFrozen(mesh::Mesh &m, int settingsIdx, bool frozen);
/** Remove the layer: subtract its contribution, drop the settings row and the
 * attribute column. */
void removeLayer(mesh::Mesh &m, int settingsIdx);

} // namespace sculptcore::displace
