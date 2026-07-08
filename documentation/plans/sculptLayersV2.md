# Sculpt layers V2 — the active layer is live geometry

Successor to the V5 sculpt-layer wiring
([`displacementAndSubSurf.md`](displacementAndSubSurf.md) workstream V;
design background in [`../sculpt-layers-design.md`](../sculpt-layers-design.md)).
V5 shipped the stack (per-vert FLOAT3 delta columns + the
`Mesh::sculptLayers` settings sidecar, the `displace::` compositor, undoable
weight/enable/frozen ops, a panel) but only ONE brush can write a layer: the
`LAYERDRAW` kernel, the single kernel authored to write deltas through
`LayerEditScope`. Every other kernel — smooth, grab, clay, plus autosmooth
and dyntopo's own vertex motion — writes `v.co` and knows nothing about
layers.

## The idea

V2 makes the **active layer implicit**: while a layer is the edit target,
its delta is not stored at all — it is *defined* as

```
d_active(v) ≡ co(v) − rest(v),   rest(v) = base(v) + Σ_{i≠active} wᵢ·enabledᵢ·dᵢ(v)
```

and only **folded** into its column at the moments something needs the
number: a weight/enable/frozen edit, switching the active layer, save, or a
multires writeback. Sculpting itself is untouched geometry editing — every
brush, autosmooth, dyntopo reposition, and GPU stroke edits the active layer
*by construction*, because editing `co` **is** editing the active layer.

This is the natural completion of a choice the F1 compositor already made
(`compositor.h`): the base is ALREADY implicit (`base = co − Σ wᵢdᵢ`), `co`
is authoritative, and meshlog restores co + layer columns atomically. V2
extends the same reasoning one layer up the stack.

Why not per-stroke "record mode" (end-of-step diffs from the meshlog element
store)? It was the runner-up: it needs a capture pass over three record
classes (elem rows, topo Existed records, created verts), an extra undo
chunk per stroke, and it sits *on top of* multires writeback fighting for
the same co-deviation. The implicit model needs none of that — no
stroke-time work, and undo is free (the existing co undo IS the layer undo,
since the delta is derived).

## Decisions locked

- **Delta space stays object-space absolute** (what V5 ships: the compositor
  applies `co += Δw·d` directly). Tangent-space layers (detail that re-poses
  with the surface) are NOT in V2; if ever wanted, the fold point is the
  single place to add them (project through the F3 frames on fold,
  reconstruct through them on composite — the grids/VDM `frameᵀ`/`frame`
  seam). Do not half-adopt: mixed-space stacks are a semantics trap.
- **Active-edit pins weight to 1.** Folding at weight `w` divides by `w`
  (explodes at 0, amplifies noise near it). The ZBrush record-mode rule:
  making a layer the edit target sets its weight to 1 (through the existing
  undoable weight op, so the pin itself is undoable); the weight slider is
  read-only while the layer is the edit target. A frozen layer cannot be the
  edit target (existing semantics).
- **Fold-on-demand with a staleness marker.** `Mesh` gains
  `activeEditLayer` (settings index, −1 = none — moves engine-side from the
  app) and a `layerFoldDirty` flag set by… nothing: it is simply *assumed
  stale* whenever `activeEditLayer >= 0` (tracking individual co writes
  would touch every writer — the exact thing this design avoids). Fold is
  idempotent and cheap (one region/whole-mesh pass), so consumers fold
  unconditionally: the settings mutators, layer switch, `serialize()`, and
  multires `writeback`.
- **Fold math**: `d_active(v) = co(v) − base(v) − Σ_{i≠active} wᵢ·enabledᵢ·dᵢ(v)`
  where `base` is reconstructed the same way the compositor already does.
  Equivalently and cheaper: `d_active += co − co_expected` is wrong (no
  co_expected exists); the direct form above is one resolveStack walk per
  vert. Whole-mesh fold is O(V·layers) — the same cost class as the
  existing weight mutators.
- **Undo model**: strokes need nothing (co undo suffices; the derived delta
  is consistent at every cursor). The FOLD itself mutates the layer column
  without changing co — folds happen inside ops that already own undo
  (weight/enable/switch ops extend their snapshots to cover the folded
  column; the V5 blob/inverse patterns apply). Save-time folds must not
  dirty undo state: fold, serialize, then *unfold is unnecessary* — a fold
  is semantically a no-op (co unchanged, stack evaluation unchanged), so a
  post-save mesh is bit-identical in every observable except the column,
  and the column's new content is the CORRECT current delta. Folds are
  therefore safe to run at any time, which is what makes the staleness
  model workable.
- **`LAYERDRAW` is retired from the UI, kept in the engine** for one
  release as a test fixture (the sbrush attr-redirection path it exercises
  is still used by color/paint). The Layer Draw entry disappears from the
  brush picker; the panel's "edit target" toggle replaces it.
- **Multires: a sculpt layer on a level mesh is a grids-store CHANNEL, not
  a vertex column.** `GridsStore` is already multi-channel
  (`addChannel(name, floatsPerElem)`; "disp" is channel 0). Per layer, one
  FLOAT3 channel; the level compositor sums
  `disp_total = ch0 + Σ wᵢ·enabledᵢ·chᵢ` at materialization/chain-eval;
  `writeback` gains a **target channel** (the active layer's, else 0).
  This resolves the writeback-vs-layers fight over co-deviation by
  construction, inherits X5 eviction and store serialization for free, and
  keeps grids channels in the store's native frame-relative space (the
  channel composition happens in frame space BEFORE the `base + frame·disp`
  reconstruction, so object-space plain-mesh layers and frame-space level
  channels never mix — they are different storage for the same UI concept).
- **Plain-mesh layers and level channels do not convert in V2.** Enabling
  multires on a mesh with sculpt layers folds & bakes them (flatten), with
  a confirm; deleting a stack does the reverse only for channel 0 (layers
  authored on levels are lost with the stack — documented, gated by a
  confirm). Cross-representation migration is a V3/X-track item.

## Module map

- `source/displace/compositor.{h,cc}` — `foldActiveLayer(m, region?)`,
  `setActiveEditLayer(m, idx)` (pin weight, fold the outgoing layer),
  rest-evaluation helper shared with the existing mutators.
- `source/displace/c-api/` — `Mesh_layerFold`, `Mesh_setActiveEditLayer`
  (napi + 4-place TS threading per the house rule).
- `source/mesh/mesh.h` — `activeEditLayer` on the sidecar.
- `source/subdiv/grids.{h,cc}` — nothing new (channels exist); level
  channel registry keyed by layer settings row.
- `source/subdiv/multires.{h,cc}` — channel-aware `ensureChain`/
  `materialize` composition; `writeback(level, targetChannel)`;
  `storeDispFromPositions` channel parameter; per-level channel add/remove.
- `scripts/lite-mesh/litemesh_ops.ts` / `litemesh.ts` — edit-target toggle
  op + panel wiring, weight pin, LAYERDRAW removal from the picker.
- Tests: `tests/test_compositor.cc` (or extend the existing layer gate),
  `test_multires.cc` channel gates, `sculptcore_layers` integration
  extensions.

## Milestones

### M1 — engine fold machinery (plain meshes)

`foldActiveLayer` + `setActiveEditLayer` + the mutators folding first.
The existing `LayerEditScope` stays (color-brush-style writers and tests).

Gate (ctest): sculpt-sim writes co directly (no kernel involved) with an
active edit layer → fold → weight 0 returns the surface bit-exactly to
rest; weight round-trip 1→0→1 is bit-stable; fold idempotence (double fold
== single fold); frozen/disabled interplay; fold-under-region vs whole-mesh
equivalence.

### M2 — app wiring

Edit-target toggle on the panel (per-layer radio; toggling runs an undoable
op that pins weight 1 + folds the outgoing target), weight slider read-only
while targeted, Layer Draw removed from the picker (flag-gated engine
kernel retained), `serialize()` folds first. All existing `sculptcore_layers`
gates stay green; new integration gates: a SMOOTH stroke and a GRAB stroke
into the targeted layer (real op path, both backends), weight-0 restores
the pre-stroke surface to fp residual, stroke undo returns both co and the
(derived) delta consistently, dyntopo-under-edit interpolates the stack
consistently (existing linearity argument), GPU-stroke (kelvinlet) recorded
correctly (co syncs before endStep, so the fold sees final positions).

### M3 — multires layers as grids channels

Channel registry (settings row ↔ channel index per stack), channel-aware
chain/materialize composition, `writeback` target channel, level-mesh panel
parity (add/remove/weight on level meshes route to channels). Persistence
rides `Multires_serializeStore` unchanged (channels are already in the
container) — extend the store version note if the channel registry needs a
header field.

Gate (ctest + integration): stroke on a level with a targeted layer →
writeback lands in the layer channel, channel-0 disp untouched; weight 0 at
materialization removes exactly the stroke; level switch round-trips
bit-stable with multi-channel composition; `.wproj` round-trip preserves
channels + weights; eviction/rehydration over multi-channel levels
(gateStoreEviction extension); wasm↔native checksums.

### M4 — docs + close-out

`documentation/sculptLayers.md` rewrite around the edit-target model,
plan status ledger, memory/projectIndex refresh, CLAUDENOTE sweep.

## Testing strategy

Same doctrine as displacementAndSubSurf: engine invariants in ctest,
behavior through the real op path in the NW-headless integration suites,
both backends every stage, checksums where the math is exact (folds are
adds/subtracts — bit-stable round-trips are assertable), residuals only
where fp re-encoding is inherent (frame-space level channels: `frameᵀ` then
`frame`, the known two-rounding truth — never checksum-gate those).

## Risks / open questions

1. **Stale-delta leaks**: any NEW consumer that reads a layer column
   directly must fold first. Mitigation: route all engine reads through
   `resolveStack` + make `resolveLayer/resolveStack` assert (debug) when
   `activeEditLayer` is set and the caller didn't fold.
2. **Fold cost on giant meshes**: whole-mesh O(V·layers) at every weight
   tweak. Acceptable now (the V5 mutators already pay similar); if it
   bites, region-scoped folds using the stroke's touched set are the
   escape hatch (the meshlog elem store knows the region).
3. **Layer count × multires memory**: each channel is a full per-level
   lattice. X5 eviction covers residency, but the store blob grows
   linearly per layer — surface the count in the panel; no hard cap in V2.
4. **VDM interplay**: a VDM store on a mesh with a targeted layer — the
   splatter doesn't move co, so folds are unaffected; `vdm_apply` moves co
   and IS a layer edit while a target is set (document: apply-with-target
   records into the layer, which is actually a feature — "apply VDM into a
   layer").
5. **Ordering with existing flags**: `sculptcore.sculpt_layers` gates all
   of it; V2 replaces the V5 UI in place rather than adding a second flag.
