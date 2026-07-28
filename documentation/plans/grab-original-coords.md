# Grab / Kelvinlet from original coordinates (ImmediateTODOs #35)

## Goal

The grab and kelvinlet brushes should behave like Blender's Grab / Elastic
Deform: they **grab a fixed set of vertices** at stroke start and pull them
**from their original (pre-stroke) positions**, so the deformation is a coherent
elastic pull that the user can drag around (and that springs back smoothly),
not a per-dab smear that traces the cursor. Today both apply a per-dab delta to
the *current* positions with `grabFrom` tracking the moving cursor, so kelvinlet
in particular traces a thin protrusion and "feels like the snake hook" (#35).

Concretely the user-visible contract is:

1. **From original coordinates** — each affected vert's new position is
   `orig.co + displacement(orig.co)`, recomputed each dab from the stroke-start
   position, not accumulated onto the live (already-deformed) position.
2. **Fixed vertex set / fixed falloff** — the affected region and its falloff
   weights are fixed at stroke start (keyed off original positions), so the set
   doesn't drift or shed verts as the surface bulges away.
3. **Ray-cast the pre-stroke surface** — the grab point under the cursor stays
   anchored to the *original* surface as it deforms, so dragging feels like
   moving a fixed handle rather than chasing the moving geometry.

Snakehook is intentionally excluded — its per-dab drag-plus-gather *is* the
desired behavior (it should keep tracing a hook).

## Existing infrastructure to build on

- **Non-accumulate mode** (`source/brush/accum_mode.h`, `plans/nonAccumMode.md`):
  `executor.nonAccum` + per-command `def.accumulable` select an `AccumOrig`
  kernel instantiation. `AccumOrig` reads each vert's stroke-start position from
  the `.brush.orig.co` TEMP attr (keyed by `strokeGen`); the `CoProxy` makes a
  kernel's `v.co` *read* the orig base and *write* live. The executor stamps the
  in-region verts' `.brush.orig.*` before the deform (brush_executor.h ~537), and
  — since #37 — `applyDynTopoDab` stamps the seed region before the remesh too.
- **`.brush.orig.co` is already TEMP + NOCOPY** and interpolated onto dyntopo-
  created verts, so original positions survive mid-stroke remeshing.
- **Spatial tree**: `SpatialNode` has one `AABB aabb` (node.h:118) over *current*
  positions; `SpatialTree::castRay` (spatial.h ~203) walks it; `regen_node_bounds`
  tightens it from current `f.no`/`v.co`. Dyntopo keeps the tree current
  incrementally (M7.6).
- The interactive grab dispatch already projects each dab's view ray onto a
  fixed **anchor plane** (first dab's surface point + view normal), so the grab
  point already moves in a stable plane rather than re-casting the live surface
  (sculptcore_ops.ts `applyDabOne`, #18/#19/#34). Phase 2 replaces/augments the
  flat plane with the real pre-stroke surface.

A throwaway interim fix (pin `grabFrom` to the anchor for kelvinlet, in
`applyDabOne` + `runSculptcoreStroke`) is currently uncommitted; Phase 1
supersedes it — drop it when Phase 1 lands.

## Phase 1 — brush from-orig + fixed region (brush layer only)

Scope: `source/brush/kernels/{grab,kelvinlet}.sbrush`, the accum-mode wiring,
and the grab dispatch in `scripts/editors/view3d/tools/sculptcore_ops.ts`
(`applyDabOne` + `runSculptcoreStroke`). No spatial-tree changes.

1. **Make grab + kelvinlet consume original positions.**
   - GRAB is already `def.accumulable = true`; KELVINLET is `accumulable = false`
     (kelvinlet.brush.gen.h:138) so it never gets an `AccumOrig` instantiation —
     flip it on in `kelvinlet.sbrush` (and verify snakehook stays as-is).
   - `AccumOrig` today has **Layer-brush accumulation** semantics (delta added to
     the running displacement, clamped to the no-falloff height — see the
     `CoProxy` doc comment). *(Stale as of `b106862`, 2026-07-20: the height cap
     was removed, so `AccumOrig` is now plain additive. The argument here is
     unaffected — additive-without-cap still isn't a grab.)* That is *not* a
     grab: a grab wants the absolute
     `orig.co + disp(orig.co)` each dab. Decide between:
     - (a) a new accum policy `AccumOrigAbsolute` (reads orig, writes
       `orig + delta` with no layer cap), selected for grab-class brushes; or
     - (b) keep `AccumOrig` but have the grab/kelvinlet kernel write the absolute
       displacement explicitly (compute from the orig base the proxy exposes).
     (a) is cleaner and keeps the kernels declarative; (b) is less infra.
2. **Cumulative grab vector.** `grabTo` becomes the **cumulative** displacement
   from the anchor to the current (plane- or surface-projected) cursor, not the
   per-dab step. `grabFrom` is the fixed anchor (object-local stroke-start point).
   The kelvinlet/grab kernel then computes `disp` from `orig.co - grabFrom` and
   the cumulative `grabTo`, and writes `co = orig.co + disp * falloff`.
3. **Fixed falloff / region.** The kernel's falloff (`strength(co)`) must evaluate
   at the *original* position so the weight set is fixed — under `AccumOrig` the
   proxy read already yields orig, but confirm `strength()`/`dabFalloffFraction`
   see the orig position, and that the region (seed verts) is taken once at stroke
   start (not refiltered per dab as geometry moves out of the live AABB).
4. **Dispatch.** In `applyDabOne`/`runSculptcoreStroke`, for GRAB + KELVINLET set
   `grabFrom = anchor`, `grabTo = cumulative` (drop the per-dab-step + interim
   pin). Snakehook keeps the current per-dab `grabFrom = cursor`, `grabTo = step`.

Phase-1 result: dragging a grab/kelvinlet pulls a fixed region from its rest
shape; releasing/over-dragging behaves elastically. The grab point still moves in
the anchor *plane* (good enough on a roughly-flat region; Phase 2 fixes curvature).

## Phase 2 — pre-stroke ray-cast (spatial tree + dyntopo)

Scope: `source/spatial/` (node AABB + cast), `source/dyntopo/` (maintenance),
and the dab-center query in the dispatch. Higher risk — touches the hot path.

1. **Per-node original AABB.** Add `AABB origAabb` to `SpatialNode`, valid only
   during a stroke. Build it at stroke start from each owned vert's `.brush.orig.co`
   (same verts as `aabb`, orig positions). Gate behind a stroke flag so non-grab
   strokes pay nothing.
2. **Dyntopo maintenance.** When dyntopo splits/collapses, new verts get an
   interpolated `.brush.orig.co` (already true since #37/non-accum). Update the
   touched leaves' `origAabb` from those cached orig positions the same way
   `regen_node_bounds` updates `aabb` — i.e. a parallel `regen_node_orig_bounds`
   driven from the same dirty set. Internal-node `origAabb` unions children.
3. **`castRayOrig`.** A ray-cast that descends `origAabb` and intersects triangles
   built from `orig.co` (not `v.co`). Mirrors `castRay`/`SpatialNode::castRay`.
4. **Dab center from the pre-stroke surface.** The grab dispatch casts the cursor
   ray against the pre-stroke surface via `castRayOrig` to get the world point
   the user is "holding", then `grabTo = thatPoint − anchor`. This replaces (or
   backs) the flat anchor-plane projection so a large grab on a curved surface
   tracks the original surface instead of a tangent plane.
5. **Lifecycle.** `origAabb` (and the stroke flag) are set up in `beginStep` when
   the brush is grab-class, torn down in `endStep`. No serialized state.

### Risks / open questions (Phase 2)

- Memory: a second AABB per node (cheap) but built/maintained every grab stroke.
- Must not regress the 5M/25fps dyntopo path — `origAabb` maintenance has to ride
  the existing dirty-leaf sweep, not add a second full pass.
- Does the flat anchor-plane projection (Phase 1) already feel right for typical
  grabs? If so, Phase 2 is a polish item and can wait.
- Backend parity: WASM + native must agree (the spatial cast is backend-agnostic
  via the C++ tree, so this should be free, but add a parity check).

## Test plan

- **Phase 1**: a headless `runSculptcoreStroke` grab/kelvinlet drag asserting
  affected verts move toward the cumulative grab vector from their orig positions
  (idempotent re-dab at the same cursor = same result, the from-orig signature),
  and that the affected *set* is stable across dabs. Visual: broad elastic bulge,
  no cursor-tracing spike.
- **Phase 2**: cast a ray mid-stroke after a large bulge and assert the hit point
  matches the pre-stroke surface (within tol), not the deformed one. Dyntopo +
  grab combined: `origAabb` stays consistent across remeshing (extend the
  `sculptcore_parity` / boundary harness).

## Status

**Phase 1 DONE + verified (native backend, Electron).** Grab + kelvinlet now
deform a region fixed at stroke start, from each vert's `.brush.orig.co`, pulling
toward the cumulative drag.

Implementation:

- `accum_mode.h`: added an `AccumKind` enum {Live, Layer, Absolute, Add} and two
  from-original policies — `AccumOrigAbsolute` (write `live = want`, recompute the
  absolute position from orig each dab → follows the cursor) and `AccumOrigAdd`
  (write `live += want − base`, add this image's displacement-from-orig). The
  shared `OrigNbrBase` factors the neighbor lookup. `CoProxy::commit` dispatches
  on `AccMode::kind` (Layer keeps the capped accumulation; Absolute/Live write
  directly; Add sums).
  - *Renamed since (`b106862`, 2026-07-20):* the enum is
    `AccumKind {Live, Additive, Grab}` and the two from-original policies
    collapsed to `AccumOrig` (Additive — the un-capped accumulator; Layer's cap
    was removed) and `AccumOrigGrab` (Grab — the absolute/add pair, now one
    policy that picks re-base vs add per vertex from the `.brush.dab.gen`
    first-touch stamp rather than from two separate instantiations). The
    `grabAccumAdd` executor flag still drives that choice per symmetry image.
- `brush_executor.h`: `createCommand` selects `AccumOrigAbsolute` for the primary
  symmetry pass and `AccumOrigAdd` for mirror passes, keyed by a new
  `grabAccumAdd` executor flag (bound setter `setGrabAccumAdd`). `grabMode` drives
  the `.brush.orig.*` stamp for grab-class regardless of `accumulable`/`@global`.
- `sculptcore_ops.ts` (`applyDabOne` + `runSculptcoreStroke`): grabFrom = fixed
  anchor, grabTo = **cumulative** drag (q − anchor), dab centered on the anchor,
  node filter widened by the cumulative drag, off-mesh continuation in the anchor
  plane, and `setGrabAccumAdd(mirrorIdx > 0)` per symmetry image.

Why primary-reset / mirror-add (the crux): plain `AccumOrigAbsolute` on every pass
makes each symmetry pass **overwrite** shared verts (only the mirror side shows);
plain Layer accumulation caps a grab to ~one dab-step (no deformation). Resetting
on the primary pass and adding on mirror passes gives shared verts `orig + Σ disp_i`.

Verification (`_testSculptcoreStroke`, native, reading C++ positions via
`dumpVertCo`): single-image kelvinlet grab moves a broad ~900-vert region by
strength×cumulative-drag in the drag direction, no NaN; X-mirror grab moves +X and
−X **identically** (926 verts each, equal avg displacement, no cross-dab drift);
an on-plane apex grab with X-mirror moves 1.29× the single-image amount (the Add
pass sums, doesn't overwrite). Snakehook is unchanged (excluded from grab-class).

Follow-up: the WGSL write-back (`emit_wgsl.cc`) still mirrors only the Layer kind,
so the GPU dispatch of grab/kelvinlet would not match Absolute/Add — fine today
(the live LiteMesh sculpt grab runs on the CPU executor), but needed if grab ever
moves to the GPU path.

Phase 2 (per-node original AABB + dyntopo maintenance + `castRayOrig`) unchanged.
