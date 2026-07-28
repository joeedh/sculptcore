# Writing a Stroke Driver for sculptcore

A **stroke driver** is the host-side code that turns user input (or scripted
input) into a sequence of `CommandExecutor::applyDab` calls, wrapped in one
undo step. sculptcore deliberately owns none of it: the engine has no camera,
no pointer events, no spacing policy and no symmetry — it exposes one dab
primitive and a pile of per-dab state, and expects the host to drive them in a
specific order.

This guide is the contract. It tells you what a driver must do to be *fully
functional* — not just "moves verts", but correct undo, correct symmetry,
correct non-accumulate behavior, correct grab/anchored semantics, no leaf-seam
tearing, and dyntopo that doesn't corrupt the log.

**Reference implementations** (read alongside this doc):

| Driver | Where | Notes |
|---|---|---|
| Interactive TS app | `scripts/editors/view3d/tools/sculptcore_ops.ts` (`SculptPaintOp`) | the complete one — symmetry, dyntopo, preview, GPU, undo |
| TS sampling layer | `scripts/editors/view3d/tools/stroke_driver.ts` (`BrushStrokeDriver`) | input → evenly-spaced samples, geometry-free |
| Headless world-space TS driver | `runSculptcoreStroke` (`sculptcore_ops.ts`) | no camera; the parity/test seam |
| Native interactive | `source/debug/interactive.cc` | minimal: no symmetry, no preview, `StrokeSpacer` |
| Native scripted | `source/debug/script.cc` | `stroke` verb |

The TS-side architecture write-up (layering, pointer plumbing, pen dynamics) is
[`documentation/strokeDriverReport.md`](../../documentation/strokeDriverReport.md);
this document is its engine-facing counterpart. Headless test recipes are in
[`documentation/debugStrokeGuide.md`](../../documentation/debugStrokeGuide.md).

---

## 1. The two halves

A driver splits cleanly, and you should keep the split:

```
raw input ─▶ [ sampler ] ─▶ dabs (center, normal, radius, params)
                                   │
                                   ▼
                           [ dispatcher ] ─▶ per-dab engine state ─▶ applyDab
```

- **Sampler** — geometry-free and camera-aware: spacing, spline interpolation,
  raycasting, pen-pressure resolution, stroke methods. Emits one dab per
  spacing step. Knows nothing about sculptcore. (`BrushStrokeDriver`;
  `StrokeSpacer` in `source/brush/stroke_spacing.h` is the engine-side
  minimal equivalent for world-space hosts.)
- **Dispatcher** — everything in this document: mirrors each dab, configures
  the `Brush` and `CommandExecutor`, and calls `applyDab`.

A headless driver may skip the sampler entirely and feed world-space dabs
straight to the dispatcher — but everything in §3–§7 still applies.

---

## 2. The minimal correct stroke

The smallest driver that is *not* broken:

```ts
// once per stroke ---------------------------------------------------------
const {wasmExec, wasmBrush} = builSculptcoreBrush({...})   // Brush + CommandExecutor
wasmExec.meshLog = meshLog
wasmExec.setNonAccum(nonAccum)
wasmExec.setAnchoredGrab(strokeMethod === ANCHORED)
wasmExec.setStrokeGen(++nextStrokeGen)     // must be non-zero and unique
wasmExec.beginStep(dyntopoEnabled)         // opens the undo step

// per dab -----------------------------------------------------------------
wasmBrush.strength = ps.strength
wasmBrush.radius   = worldRadius
wasmBrush.writeProps()
pushBrushDeviceInputs(wasmBrush, ps)
buildBrushProgram(prog, brushType, brush, worldRadius, mesh)
wasmExec.applyDab(prog, center, normal, filterRadius, params ?? 0, seed)
mesh.spatial.updateQueries()

// once at stroke end ------------------------------------------------------
wasmExec.commitPreviewDab()
wasmExec.endDynTopoStroke()
wasmExec.endStep()                         // closes the undo step
```

Every remaining section is a rule about one of those lines.

### Ordering invariants (non-negotiable)

1. `beginStep` before the first dab, `endStep` after the last. Anything that
   mutates the mesh between them lands in one undo step.
2. `endDynTopoStroke()` **before** `endStep()` on a dyntopo stroke. It releases
   the stroke-long topology thaw *and* folds the final dab's pending boundary
   marks in while links are still live.
3. `commitPreviewDab()` before `endStep()` if you ever opened a preview.
4. `writeProps()` after mutating any props-backed brush scalar and before the
   dab that should see it — `loadProps` inside the kernel reads the props
   table, not the members.
5. Dyntopo params are configured **before** `applyDab`; the executor runs the
   remesh pre-pass first so the brush deforms the freshly refined geometry.

---

## 3. Stroke-level state (set once, not per dab)

| Call | Meaning | Getting it wrong |
|---|---|---|
| `setStrokeGen(gen)` | Monotonic, non-zero stamp keying every vertex's `.brush.disp.*` stroke-start snapshot. | `0` = "not stamped": non-accumulate silently degrades to accumulate. Reusing a gen across strokes leaks the previous stroke's base. |
| `setNonAccum(bool)` | Blender "Accumulate off": deformation measured from the frozen stroke-start base (`co - disp`) instead of the live position. Ignored for non-deform kernels. | Repeated coverage stacks without limit; smooth passes diverge. |
| `setAnchoredGrab(bool)` | Whether **this stroke** gives `@grabmode` kernels (grab, kelvinlet) the from-orig fixed-region policy. It is a *stroke* property, not a kernel one — the same kelvinlet dragged along a path should accumulate like any other brush. Defaults `true`. | A path-mode kelvinlet re-bases every dab and never builds up. |
| `setNeighborMode(mode)` | `0` = live disk walk, `1` = cached CSR ring1. | A freshly built `LiteMesh` maintains no live disk links: a LiveDisk smooth finds no neighbors and silently no-ops. The TS bridge always sets CSR. The executor forces LiveDisk on dyntopo steps regardless (CSR would rebuild O(mesh) per dab). |
| `meshLog = <log>` | The undo sink. | No undo, and preview/rollback becomes a no-op. |
| `keepTopoThawed` | Optional: keep topology thawed across the stroke. The dyntopo path sets it itself on the first remesh dab, so you only need it to pre-empt the first-dab thaw cost. | Nothing correctness-wise; a per-dab O(mesh) thaw at worst. |

Pen dynamics (`configureBrushDynamics`) are also stroke-level: the 32-sample
device curves are baked once, when the `Brush` handle is freshly constructed.
Per-dab you only push raw device values (§4.2).

---

## 4. The per-dab sequence

Do these in order. The numbering matches `SculptPaintOp.applyDabOne`.

### 4.0 Ask the engine for the kernel's policy — never hardcode the tool

Everything a dab's shape depends on (grab discipline, unbounded field, whether
invert is meaningful, which attribute layers to retarget) is declared by the
kernel's `sbrush` annotations and reflected out through the stateless
`BrushMetadata` handle. A driver must query it, not keep a tool-name list —
otherwise every new brush needs a host edit, and the copies drift (that is how
`pbvh_base` ended up silently omitting KELVINLET).

```ts
const meta = wasm.manager.construct('sculptcore::brush::BrushMetadata')
const flags = meta.queryBrushFlags(brushType)     // BrushDefFlags, or undefined
const n     = meta.queryAttrManifest(brushType)   // fills the query cache
const entry = meta.queriedAttrEntry(i)            // BrushAttrManifestEntry
```

| Flag | sbrush source | What the driver does with it |
|---|---|---|
| `incremental` | `@incremental` | path-style grab: `grabFrom` = live center, `grabTo` = step since the last dab; **no** filter-radius latch (§4.4) |
| `grabModeCapable` | `@grabmode` | from-orig anchored grab when the stroke declares `setAnchoredGrab(true)`; latched filter radius |
| `unbounded` | `@unbounded` | filter at `radius * unboundedExtent`, not `radius` |
| `relaxesBase` | `@relaxation` | ignore invert — an inverted relax-toward-the-mean kernel diverges |
| `accumulable` | derived | the kernel honors `setNonAccum` (the GPU marshal reads it too) |
| `needsCoPrev`, `writesMask`, `writesColor`, `faceMode`, `readsVclass` | derived | executor/GPU-marshal internals; a host rarely needs them |

`isGrab` is just `incremental || grabModeCapable` — the two disciplines in §4.5.

The answer is fixed for the life of the process (it is codegen output), so
memoize it **by `SculptBrushes` value**, not on a per-stroke `Brush` handle.
The TS bridge does this in `resolveDabPolicy` / `resolveToolDabPolicy`
(`scripts/editors/view3d/tools/sculptcore_bindings.ts`), returning one
`DabPolicy` that both exec paths and `pbvh_base` share.

### 4.1 Resolve the dab frame

- **center / normal** — object-local. For a viewport driver: raycast the mesh
  along the sample's view ray and use the hit; for Anchored, use the driver's
  fixed anchor without re-raycasting (the dab center must never move off the
  anchor).
- **plane-family normal** — Clay/Scrape/Fill may project along the view vector
  instead of the surface normal (`resolvePlaneDabNormal`). Purely a host
  decision; the kernel just consumes `normal`.
- **radius** — the falloff radius, in world/object units, written to
  `wasmBrush.radius`. A screen-px brush must be converted at the dab
  (project the center, project center+1px, unproject, take the distance).

### 4.2 Push brush state

```
wasmBrush.strength = ...        // per-dab (pressure-modulated base)
wasmBrush.radius   = ...
wasmBrush.writeProps()          // AFTER every props-backed scalar
pushBrushDeviceInputs(brush, ps) // PRESSURE / TILTX / TILTY / TWIST
```

Only `PRESSURE` is consumed today; tilt/twist are pushed but inert until a
dynamics channel maps to them.

> Pen dynamics are applied **twice by design**: once host-side (the sampler's
> pressure→radius/strength/spacing curves, which drive the *spacing math* and
> dab size) and once engine-side (the baked 32-sample device curves, which
> modulate the deform inside `loadProps`). Do not "fix" this by removing one —
> the sampler needs a resolved radius to space dabs at all.

### 4.3 Build the program

`buildBrushProgram(prog, brushType, brush, radius, mesh)` composes:

- the main kernel command;
- one `setCommandAttrLayer(cmdIdx, attrIdx, layer)` per **retargetable** attr
  handle in the kernel's manifest (§4.0), pointing it at the mesh layer the user
  has made active for that handle's `@use` category. Walk the manifest — do not
  assume `attrIdx 0`:

  ```ts
  for (let attrIdx = 0; attrIdx < meta.queryAttrManifest(brushType); attrIdx++) {
      const e = meta.queriedAttrEntry(attrIdx)
      if (!e || e.use === 0 || e.boundName !== '') continue   // engine-internal
      const layer = mesh.activeAttrLayerIndex(e.use)
      if (layer >= 0) prog.setCommandAttrLayer(cmdIdx, attrIdx, layer)
  }
  ```

  A non-empty `boundName` (a fixed layer name in the DSL) or `use == 0` means the
  handle is engine-internal — leave it alone. Skip the retarget entirely and the
  codegen falls back to ensure-by-name, which will happily paint a *different*
  layer than the UI shows. Bind on **every** command in the program, including
  each repeated smooth pass below;
- for `autosmooth > 0`, a chained `BSMOOTH` command at strength `autosmooth`,
  never inverted;
- for the dedicated smooth tools, N repeated passes rather than one high-strength
  pass (a single >1 blend overshoots).

Rebuilding the program every dab is fine and expected — it is a small command
list, not a compilation.

Invert (Ctrl-drag) is a host concern too: suppress it for a `relaxesBase`
(`@relaxation`) kernel, and for the plane family express it by flipping the
plane rather than negating strength.

### 4.4 The filter radius (the single most bug-prone parameter)

`applyDab`'s `radius` argument is **not** the falloff radius. It is the
*node-filter* radius: which spatial leaves the dab touches. The falloff uses
`brush->radius`. They differ, and the differences are where the tearing bugs
live:

- **`unbounded` kernels** (kelvinlet) have no distance falloff of their own —
  only a C1 window over `[0.8R, R]` with `R = radius * unboundedExtent`. Filter
  at the brush radius and the field is still live where the node set stops:
  visible tearing at leaf boundaries. The engine floors the filter at
  `CommandExecutor::filterRadiusFloor(brushType)` for safety, but a host driving
  a widened region should compute `fieldRadius = radius * unboundedExtent`
  itself, gated on the queried `unbounded` flag — not on the tool being
  kelvinlet.
- **Grab-class dabs** must widen the filter by the cumulative drag length —
  the region has to cover both where verts *are* and where they move *to*:
  `filterRadius = fieldRadius + |anchorVec|`.
- **From-orig grabs must never let the filter shrink.** A from-orig dab only
  writes verts inside the filter; if the region shrinks (drag reversal), the
  verts it dropped keep their last displaced value and leave a stale ring.
  Latch a per-stroke high-water mark: `filterRadius = max(maxFilterRadius, filterRadius)`.
- **`incremental` kernels are the exception** (Snake Hook): they are not
  from-orig grabs, their dabs track the live surface, and the same radius also
  sizes the dyntopo dab. Latching one pinned at the stroke's widest collapses
  edges well outside the dab. Apply the high-water latch only when
  `grabModeCapable && !incremental`.

### 4.5 Grab-family per-dab vectors

A grab-class dab is one whose policy (§4.0) reports `incremental ||
grabModeCapable` — today grab, kelvinlet and snakehook, but the driver should
never spell that list out. Two different disciplines:

**`incremental` — Snake Hook (path-style):**
```
grabFrom = this dab's center          // live raycast surface
grabTo   = center - prevDabCenter     // step since the last dab (zero on dab 0)
```

**`grabModeCapable` — Grab / Kelvinlet (anchored, from-orig):**
```
grabFrom = the stroke anchor          // fixed for the whole stroke
grabTo   = anchor → live cursor       // absolute drag, recomputed each dab
dabCenter = the anchor                // the dab does not follow the cursor
```
plus `setAnchoredGrab(true)` at stroke level, the widened + latched filter
radius from §4.4, and `setGrabAccumAdd` per symmetry image (§5).

Keep `prevDabCenter` / the anchor **per mirror image** — each image traces its
own path, and sharing the state makes mirrored grabs pull in the primary's
direction.

### 4.6 Host-owned direction vectors

Two vectors sculptcore will *not* derive for you, because under symmetry it
cannot:

- **`strokeDir` + `strokeDirHostSet = true`** — the stroke tangent, from
  consecutive *primary* dab centers, normalized, skipping near-zero steps.
  Drives the oriented Box falloff (SQUARE brushes) and wing-scrape. Each mirror
  image reflects the primary tangent by its sign multiplier. Set
  `strokeDirHostSet = false` on the first dab (no previous center yet).
- **`viewDir`** — the object-space eye→surface ray for view-normal automasking.
  **Pin it at the stroke's first primary dab** and reflect it per image. Do not
  use the live per-dab view vector: it drifts under perspective, and because
  mask factors stamp first-touch in leaf-sized blocks, a drifting ray tears
  along spatial-node boundaries. A headless driver with no camera should set
  `automask_view_normal = false` rather than inventing a ray.

### 4.7 Dyntopo

Dyntopo runs at **its own spacing** along the stroke, not every dab:

```
dynTopoDue = (ps.strokeS - lastDynTopoS) >= dt.dynTopoSpacing
```

Rules:

- Decide `dynTopoDue` **once, on the primary dab**, and reuse the decision for
  every mirror image. Updating `lastDynTopoS` per image makes the primary
  consume the spacing budget and starves the mirror sides of remeshing.
- Resolve `l_max` / `l_min` host-side (`resolveEdgeGoal(radius, worldPerPixel)`)
  — the engine is camera-free and cannot convert a pixel edge goal.
- Pass a `DynTopoParams*` only on due dabs; `null`/`0` disables the pre-pass.
- The `seed` argument drives dyntopo's independent-set selection. Use a
  monotonically incrementing per-dab value so runs are deterministic and
  successive dabs don't select identical sets. Pass `0` when dyntopo is off.
- Dyntopo is **incompatible with multires**: the writeback assumes the level's
  fixed grid topology. Force it off whenever a multires stack is attached.
- Read `lastDynTopoStats` after each dab if you want split/collapse/flip counts.

### 4.8 The preview / rollback protocol

Required for any stroke method where one input = one dab and dabs must **not**
compound (Anchored, Drag Dot). Without it, a 200-event drag leaves 200
committed partial dabs.

Per driver tick, in order:

```
if (primary image && previewActive()) {
    rollbackPreviewDab()      // undo the whole previous tick's group
    spatial.updateQueries()
}
primary image ?  beginPreviewDab(center, filterRadius)
              :  extendPreviewDab(center, filterRadius)
applyDab(...)
```

and once at stroke end, `commitPreviewDab()` — `applyDabOne` has no "this is the
final sample" signal, so the last dab is provisional like any other; commit
keeps its effect and drops the snapshot bookkeeping. Skipping the commit leaves
`previewActive()` stale and the *next* stroke's first rollback fires against the
wrong step.

Note the symmetry shape: one tick's primary dab **begins** the session, each
mirror image **extends** it, and the whole group rolls back as one unit when the
next tick's primary lands.

### 4.9 After the dab

- `mesh.regenBounds()` — the deform moved verts.
- `spatial.updateQueries()` — the query-correctness half of the spatial update
  (split/merge, tris, bounds, normals). Do **not** run the full
  `spatial.update(gpu)` per dab in a live stroke; the GPU-buffer half belongs
  once per frame in the draw path.
- Refresh derived overlays lazily. Bumping a mesh revision that rebuilds
  wireframe/points per dab thaws topology and costs O(all edges) — do it at
  stroke end.

---

## 5. Symmetry

sculptcore has **no symmetry**. Mirroring is entirely the driver's job, and it
is plane-mirror only (X/Y/Z and their combinations — 7 reflections for X+Y+Z).
There is no radial/angular symmetry on this path.

For each dab, apply the primary image first (identity), then one image per
sign-flip multiplier. For each image:

1. Reflect the sample: positions, direction vectors, angles. If your sample
   carries a render matrix, fold `diag(mul)` into it rather than flipping the
   projected radius — that keeps the per-pixel radius flip-invariant.
2. Re-raycast to snap the mirrored center onto the surface (except Anchored,
   which uses the resolved position directly).
3. Reflect `strokeDir` and the pinned `viewDir` by the same multiplier.
4. **`setGrabAccumAdd(mirrorIdx > 0)`** before `applyDab`, for grab-class
   strokes. `false` on the primary image begins a new logical dab (bumps the
   engine's `dabGen`) and re-bases touched verts absolutely from orig; `true` on
   mirror images adds their pull onto it. A vert shared across the symmetry seam
   then gets `orig + Σ disp_i` within one dab, instead of the last image
   overwriting the others.
5. Keep per-image state separate: previous dab center, grab anchor. Share only
   the dyntopo-due decision.

---

## 6. Ending the stroke

```
commitPreviewDab()        // keep the last provisional dab
endDynTopoStroke()        // release the topo thaw + fold final boundary marks
[ optional: compactIfFragmented(spatial, ratio) ]   // folds into the open step
endStep()                 // close the undo step
multiresWriteback()       // fold the stroke into the grids store (no-op if none)
regenTreeBatch()          // rebuild draw batches
```

`compactIfFragmented` is the one thing that legitimately goes *inside* the still-
open step: undo then reverts stroke and compaction together. It reorders verts,
so never run it on a multires mesh (stale grid tables).

Dispose per-stroke handles (`BrushProgram`, `DynTopoParams`) here — they are
C++ objects, not GC'd.

---

## 7. Undo / redo

The undo payload lives in the shared C++ `MeshLog`, not in the host op:

- `meshLog.undo(mesh, spatial)` / `.redo(mesh, spatial)`.
- After either, re-run `multiresWriteback()`, `regenBounds()`, and bump whatever
  revision your overlays key on — the stroke path consumes the spatial flush the
  draw path normally keys revision bumps on.
- Report the step's real cost to your undo stack via `stepMemSize(stepId)`
  (capture `lastStepId()` right after `beginStep`), and free it with
  `freeStep(stepId)` when your stack trims the entry. Otherwise the C++ step
  outlives the host op and leaks.

---

## 8. Optional: the GPU path

A driver may route dabs to a GPU compute session instead of the CPU executor.
The decision must be made **once per stroke, on the first primary dab** — never
mid-stroke. Eligibility (`GpuStrokeController.tryBegin`): feature flag on, an
interactive (modal) stroke, dyntopo **off** and `autosmooth == 0` (both
incompatible), a supported kernel, and a live device. Any miss → stay on the CPU
path unchanged.

Two things a GPU driver must handle that a CPU one does not:

- **Failure policy**: if init fails before anything dispatched, abort the
  session and fall the *whole stroke* back to the CPU. Never mix.
- **Async finalization**: the final readback is a `mapAsync`; the stroke's
  `endStep` runs on that chain. Undo/redo arriving mid-await must be deferred
  until the completion promise resolves, or they will close/rewind a step that
  hasn't been written yet.

A shadow-verify mode (CPU authoritative, GPU run in parallel and diffed per dab)
is the recommended way to bring a new kernel onto the GPU path. See
[`documentation/gpuBrushes.md`](../../documentation/gpuBrushes.md).

---

## 9. Conformance checklist

Work down this list when writing or reviewing a driver.

**Stroke setup**
- [ ] `meshLog` assigned before the first dab
- [ ] `beginStep(hasDyntopo)` with the *resolved* dyntopo flag (multires forces off)
- [ ] unique non-zero `strokeGen`; `setNonAccum` matching the brush's accumulate flag
- [ ] `setAnchoredGrab(strokeMethod === ANCHORED)`
- [ ] neighbor mode set (CSR unless you maintain live disk links)
- [ ] per-stroke state reset: prev dab centers, anchors, dyntopo `lastS`, filter high-water, GPU decision

**Per dab**
- [ ] dab shape driven by the queried `BrushDefFlags` / attr manifest, not a tool-name conditional
- [ ] `writeProps()` after every props-backed scalar change
- [ ] device inputs pushed
- [ ] program rebuilt; every retargetable manifest attr bound on every command
- [ ] filter radius ≥ field radius when `unbounded`; widened by drag for grabs; latched monotonic for `grabModeCapable && !incremental` only
- [ ] `grabFrom`/`grabTo` per the tool's discipline, per mirror image
- [ ] `strokeDir` + `strokeDirHostSet`, and a **pinned** `viewDir` (or masking off)
- [ ] dyntopo due decided on the primary image only; params configured before `applyDab`; incrementing seed
- [ ] preview begin/extend/rollback for non-path stroke methods
- [ ] `setGrabAccumAdd(mirrorIdx > 0)` for grab-class
- [ ] `regenBounds()` + `spatial.updateQueries()` (not the full update)

**Stroke end**
- [ ] `commitPreviewDab()` → `endDynTopoStroke()` → `endStep()`
- [ ] `multiresWriteback()`, `regenTreeBatch()`, overlay revision bump
- [ ] C++ handles disposed; undo step id recorded for `stepMemSize`/`freeStep`

---

## 10. Symptom → cause

| Symptom | Likely cause |
|---|---|
| Visible seams along spatial-leaf boundaries | filter radius smaller than the live field (`@unbounded`), or a drifting per-dab `viewDir` instead of a pinned one |
| Stale ring left behind when a grab drag reverses | missing monotonic filter-radius high-water latch |
| Snake Hook collapses edges far outside the dab | high-water latch wrongly applied to Snake Hook |
| Mirror side barely remeshes under dyntopo | `dynTopoDue` / `lastDynTopoS` updated per mirror image |
| Symmetry-seam verts pulled only by the last image | `setGrabAccumAdd` not called per image |
| A wandering drag deforms more than a direct one | preview/rollback missing for a non-path stroke method |
| Next stroke's first dab rolls back the wrong thing | `commitPreviewDab()` not called at stroke end |
| Repeated passes stack without converging | `strokeGen == 0`, or `setNonAccum` never called |
| Anchored kelvinlet never builds up along a path | `setAnchoredGrab` left `true` for a path-mode stroke |
| Smooth brush silently no-ops | LiveDisk neighbor mode on a mesh with no live disk links |
| Multi-dab undo restores corrupt geometry | dabs applied outside `beginStep`/`endStep`, or `endDynTopoStroke()` after `endStep()` |
| Paint lands on a different layer than the UI shows | `setCommandAttrLayer` not called (or not on every command / not at the manifest's `attrIdx`); codegen fell back to ensure-by-name |
| A newly added brush behaves as a plain draw | the driver still branches on tool name somewhere instead of the queried policy |
| Inverted smooth blows the surface apart | `relaxesBase` not honored — invert must be suppressed for `@relaxation` kernels |

---

## See also

- [`brush_executor.md`](brush_executor.md) — executor internals
- [`brush.md`](brush.md) / [`brush_dsl.md`](brush_dsl.md) — kernel authoring; the
  `@grabmode` / `@unbounded` / `@incremental` / `@relaxation` annotations and the
  `attr … @use(<category>)` syntax behind §4.0
- [`plans/brushMetadataToTS-2026-07-28.md`](plans/brushMetadataToTS-2026-07-28.md) — how the metadata surface was built
- [`meshlog.md`](meshlog.md) — the undo log
- [`dynamic-topology.md`](dynamic-topology.md) — dyntopo internals
- [`../../documentation/strokeDriverReport.md`](../../documentation/strokeDriverReport.md) — the TS driver's architecture
- [`../../documentation/debugStrokeGuide.md`](../../documentation/debugStrokeGuide.md) — headless stroke tests
