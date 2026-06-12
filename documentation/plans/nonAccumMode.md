# Non-Accumulating Brush Mode — Implementation Plan

## Context

Today every non-global sculpt brush applies its displacement on top of the
*current* (already-deformed) vertex position, so dragging the brush back and
forth over the same area keeps piling on displacement. We want a
**non-accumulating mode**: within a single stroke, brush deformation is measured
from each vertex's **stroke-start position**, so repeated passes converge instead
of stacking (Blender's "Accumulate off" behavior).

Requirements from the request:
- Applies to **non-global** brushes only (excludes Kelvinlet / Pose, which
  already deform from rest/anchor state).
- Implemented via a **generational beginning-of-stroke position cache** held in a
  vertex attribute.
- Dyntopo's **tangential smooth must move the cached position too**, so remesh
  relaxation isn't undone on the next dab.
- **No duplicated `.sbrush` scripts** — extend the sbrush compiler seam so one
  kernel source serves both modes.

Confirmed product decisions:
- **Per-brush setting** (mirrors the existing `invert` flag), exposed in the
  brush UI.
- **Default = non-accumulate ON** (the flag bit *clear* means non-accumulate;
  setting it re-enables accumulate).
- **Deformation brushes only** for v1. Mask/Color/Polygroup (paint) keep current
  behavior.

## Dependency & sequencing — runs AFTER `sbrush-dynamic-uniforms`

This plan is sequenced to land **on top of**
[`sbrush-dynamic-uniforms.md`](sbrush-dynamic-uniforms.md)
(Route B), which the user is executing first. That plan builds machinery this one
reuses rather than reinvents:

- **DSL `@`-attribute grammar** (its Wave 1: lexer/parser accept `@ident(args)`
  field attributes such as `@range`/`@static`, plus IR `Field` metadata). I reuse
  the same tokenization for a **brush-level** attribute to declare eligibility
  (`@global` / `@paint`) instead of hardcoding a classification table — see
  Design §3.
- **Per-brush codegen manifest** (`BrushUniformManifestEntry` on
  `BrushCommandDef`, emitted in `create<Name>Brush`, consumed per stroke). I
  mirror it: emit a tiny `accumulable` brush-level manifest bit the same way.
- **`loadProps` split + stroke-start validation hook in `execBrush`** (its Waves
  2/4). My stroke-start work (ensure cache attrs, bump generation) co-locates
  with that hook; my per-brush `accumulate` toggle rides the **common-prop /
  flag** path (like `invert`), untouched by the uniform refactor.

**Shared files** (mine layers onto the dynamic-uniforms versions, not a parallel
rewrite): `compiler/emit_cpp.cc`, `compiler/ir.h` + `parser.cc`,
`brush_command.h`, `brush_executor.h`. Rebase onto its Wave-0..4 result before
starting; if `sbrush-dynamic-uniforms` is **not** yet landed, the only fallback
is the hardcoded global/paint table noted in §3 (everything else here is
independent of it).

## Why this design works (key facts from exploration)

- **Per-dab order:** dyntopo runs **before** the brush kernel within one dab
  (`scripts/editors/view3d/tools/sculptcore_ops.ts` `applyDab`:
  `applyDynTopoDab` then `execProgram`).
- **`v.co` usage in every deformation kernel** (verified across all
  `source/brush/kernels/*.sbrush`): `v.co` is read only as a whole vector
  (`strength(v.co)`, subtraction, `length`/`dot`) and written **exactly once**,
  guarded by `if (s==0) continue;`. No `v.co.x/y/z` access (except `graddraw`, a
  demo using `grad()` autodiff). → a read-base/write-live `co` proxy serves both
  modes with **zero `.sbrush` edits**.
- **Policy-template precedent:** `smooth`/`bsmooth` are already templated on a
  `NbrSource` policy selected per-dab in `createCommand()`
  (`source/brush/brush_executor.h`); neighbor reads are emitted as
  `(*ctx.co_prev)[nb]` by `source/brush/compiler/emit_cpp.cc` (~509-546). We
  mirror this with an `AccumMode` policy (honors the "templates over per-iter
  branches" convention).
- **`co_prev`:** `exec()` snapshots live `co` per-dab
  (`brush_executor.h` ~376-397) so `for_neighbor` reads a frozen state.
- **Attr flags:** `AttrFlag::TEMP` skips **both** serialization and
  split/collapse interpolation (`source/mesh/utils/attr_interp.h`,
  `source/mesh/mesh_serialize.cc`). No "interpolate-but-don't-serialize" flag
  exists — and we don't need interpolation (see Design §1).
- **Attr access by name:**
  `m.v.attrs.find_attribute(type, name).get_data<T>()`.

## Design

### 1. Generational cache (two `TEMP` vertex attributes)
- `.brush.orig.co`  : `float3`, `AttrFlag::TEMP` — stroke-start position.
- `.brush.orig.gen` : `int`,    `AttrFlag::TEMP` — generation stamp.
- Monotonic stroke-generation counter `strokeGen` (starts at 1).
  `base(v) = (gen[v]==strokeGen) ? orig_co[v] : co[v]`.
- TEMP is correct: a vertex created mid-stroke reads as **unstamped**
  (`gen!=strokeGen`), so `base` falls back to its creation position — exactly its
  effective stroke-start. So we need **no** split/collapse interpolation of the
  cache; we only need new/recycled slots to read as unstamped (§4).
- Owned by the brush layer via a spatial-style `BrushStrokeAttrs::setup(m)` in
  `source/brush/`, so `source/mesh` stays decoupled. `ensure()`d once at executor
  construction (per stroke), before the first dab's dyntopo.

### 2. Stamp pre-pass (race-free)
In `exec()`, when non-accumulate is active, run an O(region) pre-pass over the
dab's node verts **before** the parallel kernel loop:
`for v in region: if gen[v]!=strokeGen { orig_co[v]=co[v]; gen[v]=strokeGen; }`.
The parallel loop then only **reads** orig_co/gen (frozen) → no data race.

### 3. `AccumMode` policy + read-base/write-live proxy (the DSL win)
New `source/brush/accum_mode.h` with two policies (default `AccumLive`):
- **AccumLive** (accumulate): proxy `co` is the live `float3&` (today's
  behavior); `neighborCo(ctx,nb) = (*co_prev)[nb]`.
- **AccumOrig** (non-accumulate): proxy `co` reads return `base`
  (`written ? live : base`) and writes go to live (`live = base ± d;
  written=true`). So `strength(v.co)` and all displacement math evaluate at the
  stroke-start position; the single guarded write yields `live = base + disp`;
  an `s==0 continue` leaves live untouched (keeps prior dabs).
  `neighborCo(ctx,nb) = (gen[nb]==strokeGen) ? orig_co[nb] : (*co_prev)[nb]`
  (both frozen → race-free).

Proxy must support implicit `→ float3`, `operator+=`, `operator-=`, `operator=`
(verified sufficient for draw / inflate / pinch / plane / sharp / smooth /
bsmooth / texdraw / wingscrape).

**Displacement envelope (the moving-stroke fix).** A bare overwrite-from-base
snaps the trailing edge back: as the brush moves on, later dabs still cover a
vert weakly and rewrite `live = base + small_disp`, discarding the full push it
got when the brush was centered on it. `AccumOrig` writes are therefore a
**max-magnitude envelope**: a write lands only if its displacement from base
exceeds the one already applied. No accumulator attribute is needed — since
dyntopo coherence (§5) moves `orig_co` in lockstep with `co`, the applied
displacement is always recoverable as `live − base`:
`if |want − base|² > |live − base|² then live = want` (`CoProxy::commit`).
Repeated dabs over one spot still converge (the envelope saturates at the
single-dab maximum); the trailing edge of a moving stroke holds its peak. The
WGSL write-back mirrors this with the same compare against `co_buf − orig_co`
under `brush_u.nonaccum` — and that also fixes a latent GPU-only hazard where a
kernel taking its no-write path (e.g. plane's gated side) would have written the
re-seeded orig back over prior dabs' displacement.

**Codegen** (`emit_cpp.cc`): emit `AccumMode AccMode` on every kernel +
create-fn signature; change the vertex loop to
`ctx.template vertexIter<AccMode>(ctx.node)`; change the neighbor-co emission to
`AccMode::neighborCo(ctx, __nb_v)`. Regenerate `kernels/generated/*.brush.gen.h`
via `node make.mjs codegen`. **`.sbrush` sources are untouched.** AccumMode is
a C++-codegen concept, absent from `.sbrush`; the GPU side gets the same
semantics through the WGSL emitter instead (§3b below).

### 3b. GPU support (WGSL emitter; covers Vulkan-SPIR-V via naga + WebGPU)

The CPU machinery collapses on the GPU because a stroke's topology is static
there and `beginStroke` uploads co exactly once: **every vertex's stroke-start
position is that initial upload**, so the generational `.brush.orig.*`
snapshot reduces to one read-only buffer captured at `beginStroke`. No gen
stamps are needed. Neighbor parity holds too: CPU `AccumOrig::neighborCo`
reads `orig_co` if stamped else `co_prev`, and an unstamped vertex has never
moved, so `co_prev == initial co == orig_co` — "always read orig_co under
non-accumulate" is bit-identical.

- **Layout** (`brush/compute_layout.h`): `nonaccum: u32` fills the former
  `_pad0[2]` hole at offset 24 of `ComputeBrushUniforms` (sizes/offsets
  unchanged); `kOrigCoBinding = 22` sits just past the attr-slot superset
  (vk `kAttrBase=14 + kMaxAttrBindings=8`, static_assert'd).
- **Emitter** (`compiler/emit_wgsl.cc`): for accumulable kernels (neither
  `@global` nor `@paint`, non-face) emit the binding-22 `orig_co` decl, reseed
  the local `<p>_co` from `orig_co[sb_vidx]` when `brush_u.nonaccum != 0u`
  (the WGSL twin of `CoProxy<AccumOrig>` — the local is read-base until the
  single end-of-kernel write-back), and read neighbor co via
  `select(co_prev[idx], orig_co[idx], brush_u.nonaccum != 0u)`. SPIR-V is
  produced from the WGSL by naga, so one emitter covers both GPU backends.
- **Storage-buffer budget**: orig_co made neighbor kernels (smooth) bind 11
  storage buffers — one over Dawn's per-stage limit of 10 (the pipeline fails
  and the dispatch silently no-ops). Fixed by moving the falloff LUT
  (binding 7, fixed 256 f32) to a **uniform** buffer (`array<vec4<f32>, 64>`,
  scalar-indexed via `sb_lut`); identical bytes, frees one storage slot for
  every kernel, smooth lands exactly at 10.
- **Dispatchers**: `vulkan/vk_compute.*` adds the binding-22 layout slot +
  `origCo_` buffer (memcpy'd from the beginStroke co upload);
  `webgpu/wgpu_compute.*` mirrors it (its bind groups are parse-driven, only
  the buffer + `buildBindGroup` case are new). `debug/gpu_stroke.cc` computes
  `accumulable_` per kernel and sets `bu.nonaccum = scene.nonAccum &&
  accumulable_`.
- **Verification**: `tests/scripts/brush_backends/draw_nonaccum_ab.txt` under
  `sbrush-verify` (cpp↔wgsl + golden) and `webgpu-verify` (capture fixtures
  carry raw brushU bytes, so `nonaccum` replays automatically;
  `tests/webgpu/replay.mjs` binds the fixture's initial co as binding 22).
- **Orphan clay kernel fixed in passing**: `gpu_stroke.cc` still mapped CLAY to
  the deleted `clay` kernel (stale `sbrush_out` artifacts kept it loadable until
  the falloff-uniform layout change broke them). CLAY/SCRAPE/FILL now run the
  `plane` kernel like the CPU path; its `planeoff`/`planeSide` DSL uniforms get
  named union slots at offsets 72/76 in `ComputeBrushUniforms` (shared with
  kelvinlet's `mu`/`nu` — mutually exclusive per dispatch).

**Dispatch** in `createCommand()`: select `AccumOrig` when
`executor.nonAccum && def.accumulable`, else `AccumLive` (crossed with the
existing `NbrSource` choice for neighbor brushes). `def.accumulable` is a
manifest bit on `BrushCommandDef` (mirroring the dynamic-uniforms
`def.uniforms` pattern), emitted by `create<Name>Brush` from a **brush-level DSL
attribute**: global brushes declare `@global` and paint brushes `@paint` in their
`.sbrush` (reusing the dynamic-uniforms `@`-attribute grammar), so eligibility is
declared at the kernel, not hardcoded. `accumulable = !global && !paint`.
*Fallback if dynamic-uniforms hasn't landed:* a one-line `isNonGlobalDeform(
SculptBrushes)` set in C++ instead of the manifest bit.

### 4. New-vertex stamp reset
During a non-accumulate stroke, `MeshCallbacks.onVertCreate` sets `gen[v]=0`
(invalid) so fresh **and recycled** vertex slots read as unstamped. Wired into
the cb assembled in `applyDynTopoDab` only when non-accumulate is active.

### 5. Dyntopo coherence (cache tracks remesh motion)
For verts already stamped this stroke (`gen[v]==nonAccumGen`), apply the same
positional delta to `orig_co` wherever dyntopo moves a vertex:
- **Tangential smooth** Jacobi write (`source/dyntopo/dyntopo.h` ~785-813):
  `if (gen[v]==nonAccumGen) orig_co[v] += (np - co[v]);` before `co[v]=np`.
- **Collapse** survivor reposition to the midpoint: same delta for the kept vert
  if stamped (without it, the next dab snaps the survivor back). (Split inserts a
  new vert → handled by §4; flip moves nothing.)
Dyntopo looks the two attrs up by name (cached per call) and reads the active
generation from a new `DynTopoParams` field; all behind null checks so it is
inert when the cache is absent or non-accumulate is off.

### 6. Generation counter + plumbing
- Counter must be **monotonic across strokes** and shared by the executor (proxy
  + pre-pass) and dyntopo. Own it as a **TS-side static** in the sculpt op,
  incremented in `undoPre` (the once-per-stroke hook), and push the value to
  both the executor (`setStrokeGen` + `nonAccum`) and
  `DynTopoParams.nonAccumGen` (0 = off). Monotonicity guarantees stamps from
  prior strokes (and any restored by undo) never collide.
- Counter stored on the executor and exposed to kernels via ctx fields in
  `source/brush/brush_command.h`: raw `float3* origCo; int* origGen; uint32_t
  strokeGen;` set in `exec()` before the loop (alongside `co_prev`).

### 7. TS brush setting
- Add `BrushFlags.ACCUMULATE` bit in `scripts/brush/brush_base.ts` (set =
  accumulate; default-clear = non-accumulate). Surface it in
  `scripts/brush/brush.ts` `defineAPI` (the `bst.flags('flag', …)` already
  exposes flag bits; add label/icon). Run `pnpm gen:paths` if a new prop path
  appears.
- In `scripts/editors/view3d/tools/sculptcore_bindings.ts` /
  `sculptcore_ops.ts`: compute `accumulate = !!(brush.flag &
  BrushFlags.ACCUMULATE)`; push `!accumulate` to the executor; set
  `params.nonAccumGen = !accumulate ? strokeGen : 0` in
  `configureDynTopoParams`.

## Scope
- **In:** non-global deformation brushes — draw, inflate, pinch, plane
  (clay/scrape/fill), sharp, smooth, bsmooth, texdraw, wingscrape.
- **Excluded:** kelvinlet, pose (global); mask, color, polygroup (paint);
  graddraw (demo `grad()` over `v.co`). These always take the `AccumLive` path.

## Implementation order (waves)
*(Prereq: rebase onto `sbrush-dynamic-uniforms` Waves 0–4.)*
0. **Eligibility attribute** — add brush-level `@global`/`@paint` parsing
   (extends the dynamic-uniforms `@`-attribute work) → IR → `def.accumulable`
   manifest bit in `create<Name>Brush`; tag kelvinlet/pose `@global` and
   mask/color/polygroup `@paint`. *(Or the C++ fallback table.)*
1. **Cache infra** — attrs + `BrushStrokeAttrs::setup`, ctx fields, executor
   `strokeGen`/`nonAccum` members + setters. No behavior change yet.
2. **AccumMode + proxy + codegen** — `accum_mode.h`, templated proxy/iterator,
   `emit_cpp.cc` changes, regenerate headers, stamp pre-pass, dispatch off
   `def.accumulable`.
3. **Dyntopo coherence** — `DynTopoParams.nonAccumGen`, smooth + collapse delta,
   `onVertCreate` reset.
4. **TS UI + wiring** — `BrushFlags.ACCUMULATE`, defineAPI, bindings/ops
   threading; strip `CLAUDENOTE:` scaffolding.
5. **Tests** — see below.

## Critical files
- `sculptcore/source/brush/accum_mode.h` (new) — policies.
- `sculptcore/source/brush/brush_iterators.h` — AccumMode-templated proxy.
- `sculptcore/source/brush/brush_command.h` — ctx cache pointers + strokeGen;
  `accumulable` bit on `BrushCommandDef` (beside dynamic-uniforms `uniforms`).
- `sculptcore/source/brush/brush_executor.h` — ensure attrs, stamp pre-pass,
  dispatch, `setStrokeGen`/`nonAccum`, `onVertCreate` reset wiring (stroke-start
  work co-located with the dynamic-uniforms validation hook).
- `sculptcore/source/brush/compiler/{ir.h,parser.cc,emit_cpp.cc}` — brush-level
  `@global`/`@paint` attribute → `def.accumulable`; AccumMode template param +
  policy neighbor read; then regenerate `kernels/generated/*.brush.gen.h`.
- `sculptcore/source/brush/kernels/*.sbrush` — `@global` on kelvinlet/pose,
  `@paint` on mask/color/polygroup (no other `.sbrush` edits).
- `sculptcore/source/dyntopo/dyntopo.h` — cache coherence (smooth + collapse),
  `DynTopoParams.nonAccumGen`.
- `scripts/brush/brush_base.ts`, `scripts/brush/brush.ts` — TS flag + API.
- `scripts/editors/view3d/tools/sculptcore_bindings.ts`,
  `scripts/editors/view3d/tools/sculptcore_ops.ts` — counter + threading.

## Verification
- **ctest (new, native):** `tests/test_brush_nonaccum.cc` —
  (a) a small mesh, repeated identical dabs over the same area: assert max
  displacement **converges/saturates** in non-accum vs **grows** in accumulate;
  (b) base-fallback: an untouched neighbor contributes its live position;
  (c) cross-stroke: bump the generation, confirm old stamps are ignored;
  (e) envelope retention: a moving `stroke_path` keeps mid-path verts at their
  peak push (≥ 70% of the stroke-end push) instead of snapping back.
- **ctest (dyntopo):** extend `test_spatial_dyntopo`/`_smooth` — with dyntopo +
  non-accum, a stamped vert moved by tangential smooth does **not** snap back on
  the next dab (orig_co tracked the delta); same for a collapse survivor.
- **Parity:** with the flag set (accumulate), behavior is bit-identical to today
  (the `AccumLive` path is the unchanged code) — assert against a pre-change
  golden if available.
- **Backends:** `node make.mjs codegen` regenerates kernels; `sbrush-validate`
  per backend still compiles (`.sbrush` unchanged).
- **Interactive:** `source/debug` `debug_app` stroke script (set up + a
  back-and-forth stroke) to eyeball convergence; build the native addon
  (`node sculptcore/make.mjs node`) and drive a real stroke via the Electron
  harness to confirm the TS toggle flips behavior.
