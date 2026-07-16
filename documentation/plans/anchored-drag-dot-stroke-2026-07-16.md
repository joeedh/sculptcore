# Anchored / Drag Dot stroke methods

2026-07-16

## Goal

Add Blender-style **Anchored** and **Drag Dot** stroke methods to the sculpt
brush system, alongside the existing continuous-path stroke (the only mode
today). Both new modes pin the brush to a single origin for the duration of
the stroke instead of walking dabs along the cursor path:

- **Anchored**: origin fixed at mouse-down; the vector from origin to the
  live cursor sets derived parameters (radius and/or direction) and the
  brush is *re-applied* at the origin every update using those live values.
  Natural fit for Grab / Snake Hook / Rotate / Elastic Deform / Pose / Draw
  Sharp — directional brushes.
- **Drag Dot**: origin follows the live cursor as a *positioning preview*
  (no mesh mutation), and exactly **one** dab is applied, at release, using
  the final cursor position. Fit for a single textured/alpha stamp (Mask,
  textured Draw, IMM-style placement).

## Current architecture (grounding)

- `scripts/editors/view3d/tools/stroke_driver.ts` — `BrushStrokeDriver`
  is the stroke-path sampler: `ingest()` buffers control points,
  `emitSegment()` fits a centripetal Catmull-Rom → Bezier per segment and
  walks it via `arcLengthWalk` at `spacingDist = spacing * 2 * radius`,
  calling `emitRaw()` once per dab. This is the *only* place dab positions
  are currently decided — C++ has no stroke-path sampling of its own.
- `scripts/editors/view3d/tools/stroke_paint_op.ts` — `StrokeDriverOp`
  (`ToolOp`) owns the modal loop: `modalStart`, `on_pointermove` (`:213`),
  `on_pointerup` (`:221`), `on_keydown` (Esc/Enter/Space, `:229`),
  `modalEnd`, a 5ms poll timer feeding `flushDriver`.
- `scripts/editors/view3d/tools/sculptcore_ops.ts` — `SculptPaintOp`
  extends it; `applyDab` (`:348`) and `makeRayCast` (`:310`) are the
  per-dab hooks. `grabAnchor` (`:96`) is existing precedent for a
  stroke-persistent fixed point, set once on first dab and reused for the
  whole stroke by Grab-family brushes.
- `scripts/editors/view3d/tools/sculptcore_gpu_stroke.ts` —
  `GpuStrokeController.dab()` (`:440`) is the parallel GPU path, invoked
  alongside/instead of the CPU `wasmExec.applyDab` call
  (`sculptcore_ops.ts:606-628`). Any new stroke method must feed both.
- `scripts/brush/brush.ts` — `SculptBrush` fields (`spacing`,
  `spacingMode`, etc.) registered via `bst.*` `defineAPI` calls
  (`:174-195`); no stroke-method concept exists yet.
  `scripts/brush/brush_base.ts:26-29` has `BrushSpacingModes = {NONE, EVEN}`
  only — a spacing knob, not a Blender-style stroke-method enum.
- C++ (`sculptcore/source/brush/brush_executor.h`):
  - `beginStep(bool hasDyntopo)` (`:1663`) / `endStep()` (`:1683`) already
    bracket a whole stroke (reset `isFirstOfStep`, `resetStrokePath()`,
    forward to `MeshLog::beginStep/endStep`) — called from JS at
    `sculptcore_ops.ts:882` / `:1033,1042`.
  - `applyDab` (`:1567` `BrushProgram*` overload, `:1606` enum overload) is
    the sole per-dab entry; **JS decides where each dab goes** (position,
    radius, normal) and calls it once per dab — C++ never samples the path.
  - `Brush::StrokePath` (per `documentation/brush.md:104-107`) is a ring
    buffer `execBrush` appends each dab center to — reusable for rendering
    the Anchored "aim line" / Drag Dot preview overlay.
  - `CommandExecutor::defineBindings()` (`:255-296`) is the sole JS↔C++
    binding surface (litestl `BIND_STRUCT_METHOD*` macros).

**Key implication**: since JS fully owns dab placement and C++ has no
stroke-path sampling, Anchored/Drag Dot are almost entirely a **TS-side
stroke-generation change** — a new policy layered above/instead of
`BrushStrokeDriver`'s arc-length walk, feeding the *same* `applyDab`/GPU
`dab()` calls unchanged. No new C++ dab-application logic is needed. The
only C++-adjacent work is a small addition for the aim-line overlay data
and (optionally) exposing `StrokePath`'s anchor entry for GPU-path reuse
— **except for undo, see below, which does need new C++ machinery.**

### Undo model (critical constraint)

A whole stroke is one undo step today, **not** one undo step per dab:
`SculptPaintOp.undoPre` opens the meshlog step once via
`exec.beginStep(hasDyntopo)` (`sculptcore_ops.ts:189`, before any pointer
input), and `SculptPaintOp.undo()`/`redo()` (`:206-234`) call
`MeshLog::undo`/`redo` (`meshlog_base.h:2213-2252`) exactly once per
`ToolOp` instance — i.e. once per stroke, replaying every chunk pushed
between `beginStep`/`endStep` in one shot. `pushTopoChunk()`
(`meshlog_base.h:1569-1587`) seals one fresh, append-only chunk per dab
inside that step; chunks are keyed by element id, not path position, so
repeated dabs at a fixed anchor (Anchored) are captured no differently
than dabs that move (confirmed no positional/monotonicity assumption
anywhere in the topo log).

**Requirement**: Anchored's repeated re-application at the anchor as
radius/angle change live, and Drag Dot's repositioned preview, must
**undo the previous live-preview dab before applying the next one** —
otherwise every intermediate preview dab compounds into the mesh instead
of only the current live state being visible. This is a hard constraint,
not an optimization.

**Gap**: this capability does not exist today. Confirmed by direct
inspection of `sculptcore/source/meshlog/`: there is no `undoLastChunk` /
`popChunk` / "undo within an open step" primitive anywhere.
`LogChunkTopo::undo()` (`meshlog_base.h:920-983`) does real, one-shot
element release + spatial-tree bookkeeping and is only ever invoked from
the whole-step `MeshLog::undo` loop; `pushTopoChunk()` explicitly
*finalizes and seals* the outgoing chunk before allocating the next one,
and the per-step attribute chunk (`LogChunkElems`) is one chunk **per
domain per whole step**, not per-dab, so it has no per-dab granularity to
roll back either. No existing caller (brush preview, symmetry preview,
dyntopo retry) does anything like this. **New C++ machinery is required
as a prerequisite** — see step 2a below — before the TS driver changes in
step 2 can be built on top of it.

## Non-goals

- No changes to the compiled sbrush kernels — Anchored/Drag Dot are a
  stroke-generation policy, not a brush-effect change.
- No change to existing spacing-based (Dots/Space/Airbrush-equivalent)
  behavior; it must remain the default and bit-identical.
- Not implementing Blender's full stroke-method set (Line, Curve) — only
  Anchored and Drag Dot, per the ask.

## Plan

### 1. Data model: `StrokeMethod` enum + brush property

- Add `StrokeMethod` enum (`Path` = current default behavior, `Anchored`,
  `DragDot`) to `scripts/brush/brush_base.ts` alongside `BrushSpacingModes`.
- Add `strokeMethod: StrokeMethod` field to `SculptBrush`
  (`scripts/brush/brush.ts`), registered via `bst.enum(...)` in
  `defineAPI` next to `spacingMode` (`:174-195`), with icons if the enum
  UI convention requires them (see `documentation/datapath-bindings.md`).
- Run `pnpm gen:paths` after the `defineAPI` change (per root CLAUDE.md);
  surface the diff to the user before accepting per the
  `feedback_generated_ts_diff` memory convention.
- Add a brush-panel UI control (dropdown) next to the existing spacing
  control in `scripts/editors/view3d/tools/sculptcore.ts` (`:147,262,396`
  are the existing `path + '.brush.spacing'` read sites — add the sibling
  `.brush.strokeMethod` control there).

### 2. `BrushStrokeDriver`: branch stroke-generation strategy

`stroke_driver.ts` currently always does the arc-length walk. Refactor so
`ingest()`/`emitSegment()` are only used when `strokeMethod === Path`;
add two new code paths gated on the brush's `strokeMethod`:

- **Anchored**:
  - On the *first* `push()` of a stroke, record `anchorOrigin` (screen +
    world + normal, mirroring the existing `grabAnchor` pattern in
    `sculptcore_ops.ts:96`) and emit one dab at it via `emitRaw()`.
  - On every subsequent `push()` (mouse move), do **not** append a new
    path segment. Instead recompute a "live vector" = current cursor −
    anchorOrigin (screen space), derive:
    - `liveRadius` = `|vector|` when the brush's Anchored mode uses
      drag-to-size (configurable per brush, or a fixed sub-mode for now),
      and/or
    - `liveAngle` = `atan2(vector)` for directional brushes.
  - Before re-emitting, call the new mid-step rollback primitive (step 2a)
    to undo the *previous* live-preview dab's effect, then emit a fresh
    dab at `anchorOrigin` with the *updated* radius/angle — each poll tick
    (reuse the existing 5ms `flushDriver` timer in `stroke_paint_op.ts` —
    no new timing infrastructure needed) is therefore
    "rollback-then-reapply", never "stack on top of."
  - `on_pointerup` finalizes the last live dab as-is — no different from
    today's `end()`.
- **Drag Dot**:
  - On `push()`, update a `previewOrigin` (live cursor). If a preview dab
    was already applied at the old `previewOrigin` (see below), roll it
    back via the same mid-step primitive first.
  - Two implementation options, pick one and confirm with the user
    (affects step 2a's design):
    (a) apply a real rollback-able preview dab on every `push()` so the
    user sees the actual brush effect while dragging, or
    (b) render a pure-UI footprint preview (step 4) with **zero** mesh
    mutation until release, which sidesteps the rollback requirement
    entirely for Drag Dot (only Anchored would need step 2a). Option (b)
    is simpler and matches Blender's actual Drag Dot behavior for most
    brushes (the mesh isn't touched until you let go) — prefer (b) unless
    the user wants a live-mutating preview.
  - Track `previewOrigin` for the viewport overlay (see step 4).
  - On `on_pointerup` (`stroke_paint_op.ts:221`) — or `end()` — roll back
    any live preview dab (if using option (a)) and emit exactly one
    final, non-preview dab at the final `previewOrigin`.
  - If the pointer never moves (a plain click), this degenerates to a
    single dab at the click point — same as today's single-click stroke,
    so no special-case needed there.

### 2a. New C++ primitive: mid-step (open-step) dab rollback

This is a **prerequisite** for step 2's Anchored behavior (and Drag Dot
option (a), if chosen) — confirmed missing from `sculptcore/source/meshlog/`
by direct inspection (no `undoLastChunk`/`popChunk`/equivalent anywhere).

- Add a new `MeshLog` method, e.g. `undoLastChunk(mesh, spatial)` /
  `rollbackLastDab()`, that undoes only the most recently pushed chunk(s)
  for the *current, still-open* step — without decrementing `curStep_` or
  closing the step (unlike `MeshLog::undo`, `meshlog_base.h:2213-2235`,
  which does both and only operates step-boundary-to-step-boundary).
- `LogChunkTopo::undo()` (`meshlog_base.h:920-983`) already contains the
  element-release + spatial-tree bookkeeping logic needed to reverse a
  topo chunk — the new primitive reuses that per-chunk `undo()` logic but
  must pop the chunk from `LogEntry::chunks` (`meshlog_base.h:1287`)
  afterward and adjust whatever running/incremental state
  `pushTopoChunk()`'s finalize-and-seal step (`:1569-1587`) currently
  assumes is monotonic (e.g. `origIndex` counters, any "last chunk" cache)
  — audit `pushTopoChunk()` line-by-line for state that must be rewound.
- Attribute-side rollback is a separate problem: `LogChunkElems`
  (`meshlog_base.h:382-450`) is **one chunk per domain per whole step**,
  not per-dab (`elemStore()`, `:1601-1619`), so a preview dab's attribute
  edits (positions, etc.) are commingled with every other dab's edits in
  the same domain chunk with no per-dab boundary to roll back to. This
  needs either (i) a lightweight before/after position snapshot taken by
  the *caller* (TS or a thin C++ helper) around each preview `applyDab`
  call, restored directly (bypassing meshlog) before the next preview
  dab — simplest, and mirrors the existing `save_pos`/`assert_pos`
  debug-app pattern — or (ii) teaching `LogChunkElems` per-dab sub-chunk
  boundaries, which is a much larger change. **Recommend (i)**: it needs
  no meshlog format change, only touches the live mesh buffers already
  read by `applyDab`, and confines "rollback" to exactly the vertex/attr
  set the previous preview dab touched (tracked via the existing
  dirty-range/touched-vert bookkeeping brush execution already
  maintains).
- Regardless of which path is chosen, the *topology* half (created/killed
  elements from a preview dyntopo dab) still needs the `LogChunkTopo`-based
  pop-and-undo described above, since a positions-only snapshot can't
  undo a split/collapse. This makes (i)+topo-pop a hybrid: snapshot
  positions/attributes, pop-and-undo the topo chunk, in that order (undo
  topo before restoring positions, mirroring the order `MeshLog::undo`
  already uses for a whole step).
- This primitive is pure new C++ engine work, gated behind
  `CommandExecutor::defineBindings()` (`:255-296`) like every other
  JS-callable entry point — needs a binding, a debug-app verb for testing
  it in isolation, and its own focused test before the TS driver work in
  step 2 depends on it.

### 3. `SculptPaintOp` / GPU path integration

- `applyDab` (`sculptcore_ops.ts:348`) and the GPU `dab()` call
  (`:606-628`) are invoked by the driver exactly as today — Anchored and
  Drag Dot only change *when* and *with what parameters* the driver calls
  them, not the call sites themselves. Confirm `grabAnchor`-based brushes
  (Grab/Snake Hook/etc.) don't double-apply their own anchor logic on top
  of the new Anchored stroke method — likely make those brushes default
  their `strokeMethod` to `Anchored` and **remove** the bespoke
  `grabAnchor` field once the general mechanism covers it (avoids two
  parallel anchor implementations doing the same job).
- `beginStep`/`endStep` (`brush_executor.h:1663,1683`) already bracket a
  whole stroke — no change needed there; Anchored/Drag Dot just call
  `applyDab` a different number of times with different args between
  those two calls.

### 4. Viewport overlay (aim line / stamp preview)

- **Anchored**: draw a 2D line from `anchorOrigin` to the live cursor in
  the viewport overlay pass (find the existing brush-cursor overlay —
  likely near wherever the circle brush cursor is drawn in
  `scripts/editors/view3d/` — grep for the existing brush-radius circle
  draw call as the attachment point).
- **Drag Dot**: draw the brush's actual dab footprint (radius circle +
  texture/alpha preview if present) at the live `previewOrigin`, updating
  every pointer move, vanishing on release once the real dab commits.
- Both overlays are pure 2D/UI draws, independent of the WGSL brush
  kernels — no `documentation/rendering.md` frame-topology changes needed.

### 5. Keyboard/modal interaction parity with Blender (optional, confirm with user)

- Blender allows holding a modifier to temporarily force Anchored-style
  aiming even for Path-method brushes (not required for MVP — flag as a
  stretch goal, do not implement unless asked).

### 6. Testing

- **Step 2a needs its own `ctest` target first** (this is new C++ kernel-
  adjacent logic, unlike the rest of the plan): a native test that pushes
  a topo-mutating dab mid-step, calls the new rollback primitive, and
  asserts the mesh is byte-identical to its pre-dab state (element counts,
  disk/radial cycles, positions) while the step remains open — then
  applies a *different* dab and confirms normal `endStep`/undo/redo still
  works afterward (rollback must not corrupt the step it stays inside of).
- Reuse the existing **undo-fidelity pattern** for the whole-stroke case:
  `save_pos`/`assert_pos` debug-app verbs (`documentation/debugApp.md:70-71,97-101`)
  bracketing a full Anchored or Drag Dot stroke + `undo`, to confirm
  step-level undo still restores every vertex exactly — this is the
  existing dyntopo-undo regression pattern, just pointed at the new
  stroke methods.
- Once 2a is verified in isolation, verify the TS driver via the debug
  app (`source/debug/`) or a headless NW.js `--eval` script driving
  `SculptPaintOp` synthetically: script a synthetic pointer-down / several
  pointer-moves / pointer-up sequence for a brush set to each
  `strokeMethod` and assert (a) dab count/positions as before, and (b) that
  intermediate live-preview states never leave a compounded mesh — i.e.
  snapshot mesh state after each simulated pointer-move and confirm it
  matches "anchor dab applied once at current live params," never the sum
  of all prior live params.
- Manual verification in NW.js (`pnpm run nwjs`): sculpt a stroke with a
  Grab brush set to Anchored and confirm parity with today's `grabAnchor`
  behavior (regression check before removing the bespoke field);
  sculpt a stroke with Mask/Draw set to Drag Dot and confirm a single
  positioned stamp, draggable before release, with **no visible or
  measurable compounding** while dragging.
- Follow the `verify` skill before considering this done — drive the
  actual sculpt-mode UI, don't rely on typecheck alone.

### 7. Rollout

- Gate behind existing per-brush `strokeMethod` property (defaults to
  `Path` for every existing brush preset) — no global feature flag needed
  since this is additive/opt-in per brush, not a global behavior change.
- Update `documentation/brush.md` (or add a short new doc) describing the
  three stroke methods and where `strokeMethod` lives in the brush data
  API, mirroring the `spacing`/`spacingMode` documentation already there.

## Open questions for the user

1. Should Anchored's live vector control **radius**, **angle**, or a
   **per-brush-configurable choice** of the two? Blender's per-tool
   defaults differ (Grab uses angle; some brushes use radius-from-drag).
2. Should the bespoke `grabAnchor` mechanism in `SculptPaintOp` actually
   be removed and replaced by the general Anchored stroke method, or kept
   separate to avoid touching Grab-family brush behavior in this change?
3. Any specific brush presets that should ship with `strokeMethod`
   defaulted to `Anchored`/`DragDot` out of the box (e.g. Mask → DragDot),
   or should everything default to `Path` and users opt in manually?
4. For Drag Dot (step 2, option (a) vs (b)): should dragging show the
   *actual* live brush effect (requiring the same rollback primitive as
   Anchored), or a pure-UI footprint preview with zero mesh mutation until
   release (simpler, no dependency on step 2a for Drag Dot specifically)?
5. For step 2a's attribute-rollback approach: any objection to the
   snapshot/restore-outside-meshlog hybrid (recommended) vs. investing in
   proper per-dab sub-chunks inside `LogChunkElems` (larger, more general,
   but not needed unless something else wants per-dab attribute undo too)?
