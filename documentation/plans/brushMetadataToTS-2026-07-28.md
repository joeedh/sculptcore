# Brush metadata → TS: attr manifest query, `@use`, and retiring per-brush conditionals

*Plan written 2026-07-28.*

## Context

Every sbrush kernel already carries the metadata the TS bridge needs, but almost none of it
crosses the binding seam. `BrushCommandDef` (`sculptcore/source/brush/brush_command.h:325`) is
populated by codegen with `needsCoPrev` / `accumulable` / `relaxesBase` / `grabModeCapable` /
`unbounded`, plus a full per-kernel attr manifest (`def.attrs`, emitted at
`compiler/emit_cpp.cc:1634-1647`). Only the *uniform* manifest is bound
(`queryUniformManifest` / `queriedUniformEntry`, `brush_executor.h:313-314`).

So the TS side re-derives, by hand, facts the engine already knows:

- `toolAttrCategory()` (`sculptcore_bindings.ts:202`) hand-maps 4 tools → attr categories, and
  `buildBrushProgram` then hardcodes **`attrIdx 0`** (`:238`, `:254`). That is correct only
  because today's four paint kernels declare exactly one attr; `featurealign.sbrush` already
  declares two, so the next multi-attr paint kernel silently paints the wrong handle.
- `isGrabTool()` (`:170`) and `isSmoothTool()` (`:180`) restate `@grabmode` and `@relaxation`.
  `pbvh_base.ts:1222` keeps a **third, divergent copy** that omits KELVINLET.
- `brush.tool === SculptTools.KELVINLET ? radius * unboundedExtent : radius`
  (`sculptcore_ops.ts:650` and `:1153`) restates `@unbounded` — and C++ already has the exact
  helper, `CommandExecutor::filterRadiusFloor` (`brush_executor.h:539`), which is simply not bound.
- `brush.tool === SculptTools.SNAKE` (`sculptcore_ops.ts:619`, `:1137`) restates `@incremental`,
  which **never reaches `BrushCommandDef` at all** — `emit_cpp.cc:1616` folds it into
  `accumulable = false` and drops it.
- `gpu_marshal.cc:25-54` keeps a hand-maintained 16-row table whose every bool
  (`accumulable`, `grabMode`, `writesMask`, `writesColor`, `needsNeighbors`, `faceMode`,
  `readsVclass`) duplicates codegen output.

Outcome: the engine becomes the single source of truth for per-kernel policy. Adding a brush stops
requiring TS edits, the `attrIdx 0` latent bug goes away, and the three copies of "is this a grab
tool" collapse to one metadata read.

**Order of work**: (1) attr manifest query, (2) expose the remaining sbrush properties + minimal
bindings, (3) refactor the TS conditionals behind a shared helper, (4) fold in `gpu_marshal`.

**Execution environment**: a new worktree. Before any build, `source worktree-env.sh` (sets
`SCULPTCORE_EMSDK_DIR` + sccache) — worktrees have no in-tree emsdk, and a stale prebuilt
`build/sculptcore.wasm` will mask a failed build. Verify the artifact mtime/size actually changed.

---

## Phase 1 — Attr manifest query + `@use(...)`

### 1a. DSL: `@use(<category>)` on attr declarations

Grammar becomes:

```
attr <vertex|face|edge|corner> <type> <name> [= "<layerName>"] [@use(<category>)];
```

- **`compiler/parser.cc`** — `parseAttrField` (`:333-364`) has no annotation loop. Copy the one
  from `parseField` (`:303-322`, which already handles the parenthesized `@range(a,b)` form and the
  `errorf(attrTok, "unknown field attribute '%s'")` reject path). Insert it between the handle-name
  `advance()` (`:356`) / optional `= "layer"` clause (`:358-361`) and the
  `expect(TokKind::Semicolon)` at `:362`. Accept a single ident argument mapped to the
  `mesh::_AttrUse` names: `unit`, `color`, `uv`, `polygroup`, `select`, `sculpt_layer`.
- **`compiler/ir.h`** — add `int use = 0;` to `struct Field` (`:191-213`), next to the other
  per-field metadata at `:207-212`. Attrs reuse `Field`, so no new struct.
- **`compiler/emit_cpp.cc`** — the attr loop's trailing `write(", true});\n")` at `:1646` becomes
  `", true, <use>});\n"`.
- **`source/brush/brush_command.h`** — add `int use = 0;` to `BrushAttrManifestEntry` (`:38-44`),
  **after `bool write`** so the aggregate-init order in the emitter still matches.
- **`source/mesh/attribute_enums.h:72-81`** is the value source (`_AttrUse`); TS `AttrUseFlags`
  (`scripts/lite-mesh/litemesh.ts:92`) already has identical values — no new enum needed on
  either side.

Kernel edits (4 files):

| kernel | line | new decl |
|---|---|---|
| `kernels/color.sbrush` | 8 | `attr vertex float4 color @use(color);` |
| `kernels/colorsmooth.sbrush` | 9 | `attr vertex float4 color @use(color);` |
| `kernels/polygroup.sbrush` | 9 | `attr face int group @use(polygroup);` |
| `kernels/layerdraw.sbrush` | 11 | `attr vertex float3 slayer @use(sculpt_layer);` |

Leave the *fixed-name* engine-internal attrs alone (`bsmooth` `vclass`, `featurealign`
`field`/`vclass`, `enhance` `edisp`) — a non-empty `boundName` already marks them non-retargetable,
and they get `use = 0`.

Then: **rebuild `sbrushc` before regenerating.** `make.mjs:902-926` only builds the compiler when
the binary is *absent*, so an edited compiler is silently ignored:

```
cmake --build build/native --target sbrushc     # or delete build/native/source/brush/compiler/sbrushc[.exe]
node make.mjs codegen
```

Diff `kernels/generated/*.brush.gen.h` and confirm only the intended `def.attrs.append(...)` lines
moved.

Doc: update the annotation table in `sculptcore/documentation/brush_dsl.md:49-61` and the attr
syntax block at `:134-149`.

### 1b. Bind the manifest

- **`BrushAttrManifestEntry::defineBindings()`** — new, in `brush_command.h`. Copy
  `BrushUniformManifestEntry::defineBindings()` (`:66-83`) verbatim in shape; one
  `BIND_STRUCT_MEMBER` per field (`handle`, `boundName`, `type`, `domain`, `write`, `use`).
  Reading `util::string` *out* works; only string *args* are unmarshalable — hence the
  index-query pattern below.
- **`source/brush/bindings.cc:44-50`** — register it alongside
  `manager.add(Bind<BrushUniformManifestEntry>())`.
- **`brush_executor.h`** — add, mirroring `queryUniformManifest`/`queriedUniformEntry`
  (impl `:1308-1336`, cache field `queriedUniforms` at `:274`):

  ```cpp
  Vector<BrushAttrManifestEntry> queriedAttrs;          // near :274
  int queryAttrManifest(int brushType);                 // clears + fills queriedAttrs
  BrushAttrManifestEntry *queriedAttrEntry(int idx);    // nullptr when out of range
  ```

  Bind both in `defineBindings()` (`:276-323`) with `MARGS("brushType")` / `MARGS("idx")`.

  **Critical**: `createCommand` (`:507`) re-runs `createCommandImpl` for the grab/non-accum
  variants and must clear `def.uniforms` first (`:521-528`) or manifests double-append.
  `def.attrs` needs the identical clear — add it in the same block.

### 1c. Consume it in TS

Regenerate bindings: `node make.mjs build wasm` (which copies the wasm into `typescript/build/`
and runs `cd tools && pnpm build` — `make.mjs:1627-1635`). Generated TS under
`sculptcore/typescript/sculptcore/**` **is committed**; `typescript/api/` is hand-written and
preserved.

In `sculptcore_bindings.ts`, replace `toolAttrCategory` + the `attrIdx 0` hardcodes
(`:231-239`, `:250-256`) with a manifest walk:

```ts
const n = wasmExec.queryAttrManifest(mainBrushType)
for (let idx = 0; idx < n; idx++) {
  const e = wasmExec.queriedAttrEntry(idx)
  if (!e || e.boundName !== '' || e.use === 0) continue   // fixed-name => engine-internal
  const layer = mesh.activeAttrLayerIndex(e.use)
  if (layer >= 0) prog.setCommandAttrLayer(cmdIdx, idx, layer)
}
```

Two placement caveats from exploration:

- `queryAttrManifest` must use its **own** cache field, not `queriedUniforms` — a second query
  clobbers the first (`brush_executor.h:1319` clears).
- The obvious home is the `if (freshBrush)` block in `builSculptcoreBrush`
  (`sculptcore_bindings.ts:478-483`), but `freshBrush` is per-*brush-object*, not per-stroke:
  `SculptPaintOp` keeps `this.wasmBrush`/`this.executor` alive across strokes. Cache the resolved
  entries keyed by `brushType` and re-query when the tool changes. `buildBrushProgram` needs a
  `wasmExec` param (it currently takes only `mesh`).

---

## Phase 2 — Expose the remaining sbrush properties

### 2a. `@incremental` reaches the def

- `brush_command.h` — add `bool incremental = false;` next to `unbounded` (`:353`).
- `emit_cpp.cc` — emit it right after the `def.unbounded` block (`:1631-1633`), before the attrs
  loop, from `brush->isIncremental` (`ir.h:283`). Existing `accumulable` emission at `:1616` is
  unchanged.

### 2b. Bind `filterRadiusFloor`

`CommandExecutor::filterRadiusFloor(SculptBrushes)` already exists (`brush_executor.h:539`,
memoized via `floorMemoTool_`/`floorMemoUnbounded_`) and is applied internally at `:1817` / `:1857`.
It just needs a `BIND_STRUCT_METHOD` line in `defineBindings()`. This is strictly better than the
TS expression: TS only needs the floor in order to *add* the drag length on top, which a `max`
can't express.

### 2c. Instance-free flag query (needed by `gpu_marshal`)

`createCommand` / `createCommandImpl` are `CommandExecutor` **members** (`:412`), and
`gpuKernelForTool` (`gpu_marshal.cc:56`) is a free function with no executor. Add a **static**
member:

```cpp
struct BrushDefFlags { bool needsCoPrev, accumulable, relaxesBase, grabModeCapable,
                       unbounded, incremental, writesMask, writesColor, faceMode, readsVclass; };
static BrushDefFlags queryBrushFlags(SculptBrushes brushType);
```

`createCommandImpl`'s body is a pure type-dispatch switch except for `effectiveNeighborMode()` in
the SMOOTH case (`:441`) — the flags are identical for both neighbor policies, so the static
variant can pick `LiveDiskNbr` unconditionally. Implement it by factoring the switch into a
static helper that `createCommandImpl` also calls (avoid duplicating the 20-case switch).

`writesMask` / `writesColor` / `faceMode` / `readsVclass` are derivable from the attr manifest
(`domain == Face` → `faceMode`; `boundName`/`use` identify mask/color/vclass), so derive them in
`queryBrushFlags` from `def.attrs` rather than adding more codegen fields.

Bind a thin instance wrapper for TS (`queryBrushFlags` as a method returning the bound struct, or
individual `int`-returning accessors if a returned struct is awkward across the binding seam —
check how existing struct-returning binds behave before committing).

---

## Phase 3 — Refactor the TS conditionals behind one shared helper

The two exec paths — `SculptPaintOp.applyDabOne` and `runSculptcoreStroke`, both in
`scripts/editors/view3d/tools/sculptcore_ops.ts` — duplicate the grab/filter-radius block verbatim
(`:611-662` vs `:1125-1161`), differing only in state ownership (`this.prevDabLocal[mirrorIdx]` /
`this.maxFilterRadius` / `ps.anchorVec` vs `prevByImage[]` / `anchorByImage[]` / a local). The only
extracted piece today is `applyGrabDabState` (`:900-914`), covering just the snakehook branch.

Add to `sculptcore_bindings.ts` (next to `applyGrabDabState`'s spirit):

```ts
export function resolveDabPolicy(args: {
  wasmBrush, wasmExec, brushType, radius,
  p: number[], dragVec: number[], imageIdx: number,
  prevDab?: number[], maxFilterRadius: number,
}): {dabCenter: number[], filterRadius: number, maxFilterRadius: number, prevDab?: number[]}
```

Internally driven by metadata, not tool identity:

| old test | replacement |
|---|---|
| `isGrabTool(tool)` | `flags.grabModeCapable` |
| `tool === SNAKE` | `flags.incremental` |
| `tool === KELVINLET ? radius * unboundedExtent : radius` | `radius + wasmExec.filterRadiusFloor(brushType)` |
| `isSmoothTool(tool)` (invert suppression, `:401`) | `flags.relaxesBase` |
| `toolAttrCategory(tool)` + `attrIdx 0` | attr-manifest walk (Phase 1c) |
| `tool === POLYGROUP` (group-id assignment, `:553` / `:1102`) | attr manifest has a `Face`-domain entry with `use == POLYGROUP` |

Call it identically from both paths. Delete `isGrabTool`, `isSmoothTool`, `toolAttrCategory`, and
**fix the divergent third copy** at `pbvh_base.ts:1222` (currently omits KELVINLET, so kelvinlet
strokes wrongly get ctrl-invert and stroke interpolation) to read `flags.grabModeCapable`.
`litemesh_fuzztest_support.ts:162` (test-only) follows the same replacement.

Keep the tool→kernel map `TOOL_TO_SCULPTBRUSH` (`:146-166`) and `configureToolUniforms`
(`:278-313`) — those are genuinely app-side policy (clay/scrape/fill plane offsets, brush color),
not kernel metadata.

---

## Phase 4 — Fold in `gpu_marshal`

`kGpuKernels[]` (`gpu_marshal.cc:25-54`) keeps its 16 rows for the **irreducible** part only: the
tool → WGSL kernel-name string, which is not derivable (the plane family collapses
CLAY/SCRAPE/FILL onto one kernel). Every bool in `GpuKernelInfo` (`gpu_marshal.h:34`) instead comes
from `CommandExecutor::queryBrushFlags(tool)` at `gpuKernelForTool` time.

`gpu_marshal.cc` including `brush_executor.h` is fine (include guards; the dependency is one-way at
the .cc level) — but verify `brush_executor.h` doesn't pull in `gpu_marshal.cc`-only symbols. If it
does, hoist `queryBrushFlags` into a small `brush/brush_flags.h` that both include.

The only behavioral consumer is `packBrushUniforms` (`gpu_marshal.cc:99`,
`out.nonaccum = (nonaccum && info && info->accumulable) ? 1u : 0u`), so a mismatch shows up as a
non-accumulate divergence — exactly what `sbrush-verify` gates.

---

## Verification

Build (worktree: `source worktree-env.sh` first):

```
node make.mjs build native
node make.mjs build wasm        # also regenerates typescript/ bindings
npx tsgo --noEmit               # NOT tsc
```

**Native (fastest signal):**

- `node make.mjs test test_brush_uniform_validate` — the existing by-index manifest gate
  (`tests/test_brush_uniform_validate.cc:145-177`). **Add an attr-manifest section beside it**:
  `queryAttrManifest(COLOR) == 1`, `queriedAttrEntry(0)->handle == "color"`, `boundName == ""`,
  `use == (int)AttrUse::COLOR`, `domain == Vertex`; `POLYGROUP` → `domain == Face`,
  `use == POLYGROUP`; `BSMOOTH` → its `vclass` entry has non-empty `boundName` and `use == 0`;
  `queriedAttrEntry(99) == nullptr`. Also assert `queryAttrManifest` twice in a row returns the
  same count (the `def.attrs` double-append regression).
- `node make.mjs test test_brush_attr` — attr binding still paints (color + polygroup).
- `node make.mjs test test_unbounded_seam` — the only `filterRadiusFloor` regression gate.
- `node make.mjs test test_snakehook`, `test_brush_mirror_grab`, `test_brush_nonaccum`,
  `test_layer_stroke_undo`, `test_paint_undo`, `test_brush_props`.
- Full sweep: `node make.mjs test` (ctest). Note 3 native ctests fail pre-existing — compare
  against a baseline run on the branch point, don't assume new breakage.

**Cross-backend (required by Phase 4):**

- `node make.mjs sbrush-verify` — 22 A/B scripts incl. `color_ab.txt`, `polygroup_ab.txt`,
  `grab_ab.txt`, `kelvinlet_ab.txt`, `draw_nonaccum_ab.txt`, `mask_ab.txt`. Must show **zero**
  golden diffs; any diff means the gpu_marshal fold changed GPU behavior.
- `node make.mjs sbrush-validate wgsl` (and `spirv`) — the DSL grammar change must still emit
  valid GPU source for every kernel.

**TS integration** (`pnpm test`, NW.js, two backend passes):

- `sculptcore_brushes.test.ts` — kelvinlet/grab/snakehook/color/autosmooth/symmetry battery.
- `sculptcore_layers.test.ts` — `__layerToolTest` is the one test that drives the real
  `toolAttrCategory` → `activeAttrLayerIndex` path with no override seam; it must still pass after
  that function is deleted.
- `sculptcore_anchored_dragdot.test.ts`, `sculptcore_snakehook_op.test.ts`,
  `sculptcore_stroke_tester.test.ts`, `sculptcore_colormix.test.ts`, `sculptcore_gpu_brush.test.ts`.

**New test closing a real gap** (exploration finding): *no* existing test would catch a silent
revert to codegen ensure-by-name binding — every color/polygroup test ensures exactly one layer
exists, so index 0 and the by-name fallback are indistinguishable. Add to
`scripts/lite-mesh/litemesh_brushtest_support.ts`: create **two** color layers, activate the second,
stroke, assert layer 0 is untouched and layer 1 changed. This is the direct regression gate for
Phase 1c.

**Manual spot-check** (`debug_app`, optional):

```
make_cube subdivs=12 size=0.5
build_spatial leaf_limit=256 depth_limit=8
set_brush_tool tool=layerdraw
set_brush radius=0.25 strength=1.0
layer_add name=A
layer_add name=B
stroke origin=0,0,0.25 normal=0,0,1 layer=B
dump_state out=layerB.json
```

Note `dump_state` has no manifest section — for eyeballing new metadata, assert in the native test
rather than adding a verb.

## Risks

- **Stale `sbrushc`** silently regenerating with the old compiler (§1a) — the single most likely
  way this goes wrong. Force the rebuild.
- **`def.attrs` double-append** in `createCommand`'s second `createCommandImpl` pass — mirrors a
  bug already fixed for `def.uniforms` (`brush_executor.h:521-528`).
- **`freshBrush` is not per-stroke** — a manifest cached there goes stale on tool change (§1c).
- **`queryBrushFlags` static-ness** vs `effectiveNeighborMode()` (§2c) — verify the factored switch
  compiles for every `SculptBrushes` case before wiring `gpu_marshal`.
- **`write` is hardcoded `true`** for every attr (`emit_cpp.cc:1646`); there is no write analysis.
  Out of scope here — don't let the new `use` field imply otherwise.
