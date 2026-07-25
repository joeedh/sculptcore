# Adding sbrush uniforms / extending the brush ctx

How to add a new `uniform` or `ctx` field to a brush kernel and wire it
through to every backend. This is the *task* guide; the language surface is in
[`brush_dsl.md`](brush_dsl.md) (§Fields) and the compiler/backends in
[`brush_compute.md`](brush_compute.md). Read those first if the terms below are
unfamiliar.

## The one thing to internalize

A `uniform`/`ctx` declaration is just a way to **opt a name into identifier
resolution** — the host value must already exist. Where it resolves depends on
the backend, and the two halves have very different ergonomics:

- **CPU (the `emit_cpp` reference) is automatic.** Declare the field, add the
  matching member on the host, and the emitter lowers the name to a host member
  read. Nothing else to touch.
- **GPU backends are half-automatic.** The WGSL/CUDA/OpenCL emitters
  *automatically* spill the field into a fixed-schema uniform block, but the
  **host-side mirror struct and the marshal that fills it are hand-written**.
  Skip either and the GPU kernel silently reads stale/garbage bytes.

So: a CPU-only brush needs ~2 edits; a brush that also dispatches on GPU needs
~4, two of which are easy to forget. The checklist at the bottom is the
backstop.

> **Extra (out-of-repo) kernels** cannot make the host-side edit at all: their
> `uniform`/`ctx` names must resolve to *existing* `Brush` members, or the
> generated header fails to compile. See "Extra kernel dirs" in
> [`brush_compute.md`](brush_compute.md).

## `uniform` vs `ctx` — which to pick

| | `uniform` | `ctx` |
|---|---|---|
| Meaning | a brush *property* (authored, conceptually constant across the dab) | per-stroke-dot *state* (recomputed each dab) |
| Host source | a `Brush` member | a `Brush` member, **or** a `CommandCtxBase` builtin |
| GPU block | `BrushUniforms` (`@binding(5)`) | `CtxUniforms` (`@binding(6)`) |
| Examples | `strength`, `radius`, `mu`, `nu`, `activeGroup` | `surfaceNo`, `grabFrom`/`grabTo`, `poseCage*` |

On the **CPU path the two are nearly identical** — both a `uniform X` and a
non-builtin `ctx X` lower to `ctx.brush.X`. The distinction matters for (a)
which GPU uniform block the field lands in, (b) the semantic cadence you intend,
and (c) whether the name is one of the hardcoded `CommandCtxBase` builtins.

## Where host values live

- **`uniform` → a `Brush` member** (`source/brush/brush.h`). If it's an
  authorable brush property (exposed/animatable), also register it as a prop
  (see below). If the host just sets it per stroke, a plain member is enough
  (e.g. `activeGroup`, `strokeDir`).
- **`ctx` builtin → already on `CommandCtxBase`** (`source/brush/brush_command.h`):
  `mousePos`, `surfacePos`, `surfaceNo`, `mouseDir`, `renderMatrix`,
  `isFirstOfStep`, `meshLog`. Declaring `ctx float3 surfaceNo;` reuses these
  with no host change.
- **new `ctx` (non-builtin) → also a `Brush` member**, set host-side per dab by
  the executor. On the CPU path it resolves to `ctx.brush.X` exactly like a
  uniform; the `ctx` keyword is what places it in `CtxUniforms` on GPU. You only
  extend `CommandCtxBase` itself (and the `isCtxBase` list in `emit_cpp.cc`) for
  genuine engine-level per-dab state that wants the bare `ctx.X` spelling —
  rare.

### How the C++ emitter resolves a field name

In `source/brush/compiler/emit_cpp.cc` (the `ExprKind::Ident` case), a declared
field lowers to:

| Field | vertex / reduce stage | host stage |
|---|---|---|
| `uniform X` | `ctx.brush.X` | `brush.X` |
| `ctx X` (non-builtin) | `ctx.brush.X` | `brush.X` |
| `ctx X` (a `CommandCtxBase` builtin) | `ctx.X` | `ctx.X` |

(Host stages take `(CommandCtxBase &ctx, Brush &brush)` — there's no `ctx.brush`
there, hence the bare `brush.X`.)

---

## Part A — CPU-only brush (the common case)

Most kernels only ever run the C++ executor. Two edits:

### A1. Add a uniform

1. **Declare it in the kernel** (`source/brush/kernels/<name>.sbrush`):

   ```sbrush
   uniform float pinchFactor;
   ```

2. **Add the host member** to `struct Brush` (`source/brush/brush.h`):

   ```cpp
   float pinchFactor = 0.5f;
   ```

   The CPU emitter always reads the member directly (`brush.pinchFactor`), so
   it is mandatory regardless of the kind below.

   - **Authorable `float` uniform — registration is now automatic.** Give the
     declaration a default (and optionally `@range`): `uniform float
     pinchFactor = 0.5 @range(0, 1);`. Codegen reads the kernel's uniform
     manifest and emits, per kernel, the prop **registration**
     (`sd.Float32("pinchFactor").Default(0.5)`), the per-dab **load**
     (`lookupValue<float>` — which applies any bound device dynamic), and the
     **manifest entry** consumed by validation. You do **not** hand-write a
     `BrushProp` enum entry, `brushPropName()` case, `BIND_STRUCT_MEMBER`, or a
     `loadProps`/`writeProps` round-trip for it. The uniform is automatically
     drivable by pen pressure/tilt/etc., keyed by its name (see Part C); add
     `@static` to the declaration to opt the uniform out of dynamics.
   - **Common cross-kernel props** (`strength`, `radius`, `spacing`,
     `planeoff`, `autosmooth`) are the exception: they are shared by every
     kernel and addressed by an **int** `BrushProp` id (the TS bridge can't
     marshal a JS string → `util::string`), so they keep their manual
     enum + `brushPropName()` + `loadCommonProps` wiring. Don't add new
     per-kernel uniforms to that path — declare them in the kernel instead.
   - If the host just *sets it per stroke* (like `activeGroup`, `strokeDir`,
     `grabFrom`), or it isn't a `float`, a plain member with no prop
     registration is fine — codegen only auto-registers `float` uniforms.

3. **Regenerate** the checked-in C++ output:

   ```
   node make.mjs codegen
   ```

4. Use it in the kernel — `v.co += dir * pinchFactor;` now reads
   `ctx.brush.pinchFactor`.

### A2. Add or reuse a ctx field

- **Reuse a builtin** — just declare it, no host edit:

  ```sbrush
  ctx float3 surfaceNo;     // -> ctx.surfaceNo (CommandCtxBase)
  ```

- **New per-dab state** — add a `Brush` member, set it host-side each dab (the
  executor / a `host` stage), and declare it `ctx`:

  ```sbrush
  ctx float3 grabTo;        // -> ctx.brush.grabTo
  ```

  A `host` stage is the right place for per-dab derivation that must stay
  CPU-only (clamps, frame construction); see `kelvinlet.sbrush`'s
  `clampParams` and `wingscrape.sbrush`'s wing-normal setup.

That's it for CPU. `node make.mjs codegen` then build.

---

## Part B — also dispatching on GPU

If the brush has a `runBrushStrokeGPU` case (participates in `sbrush-verify` /
real WebGPU), the field has to cross the host→GPU uniform seam. The emitter side
is free; the host side is not.

### B1. (automatic) The emitter spills the field into the uniform block

`source/brush/compiler/emit_wgsl.cc` emits a **fixed superset schema** so every
kernel shares one bind-group layout:

- `struct BrushUniforms` — the hardcoded built-ins first
  (`strength`, `radius`, `spacing`, `invert`, the `falloff_*` selectors,
  `coord_space`, `tex_repeat`, `stroke_path_count`), then **every DSL `uniform`
  field appended** (names already in the built-in set re-bind to the existing
  slot instead of being re-emitted).
- `struct CtxUniforms` — `surfacePos`, `surfaceNo`, `render_matrix`, then **every
  DSL `ctx` field appended**.

References lower to `brush_u.X` / `ctx_u.X`. `Array<T,N>` becomes
`array<T,N>` (note: `array<vec3<f32>,N>` has **stride 16** in uniform address
space). You don't edit this file — `codegen` regenerates it.

### B2. (manual) Mirror the field in the host struct

Add the field to the matching mirror in `source/brush/compute_layout.h`,
**with explicit std140 padding that matches the WGSL layout**:

- `uniform` → `struct ComputeBrushUniforms` (binding 5)
- `ctx` → `struct ComputeCtxUniforms` (binding 6)

These structs are the single source of truth shared by both GPU backends
(`vk_compute`, `wgpu_compute`), so the offsets must match the shader exactly,
independent of the C++ ABI.

**std140 cheat-sheet** (the rules you'll actually hit):

| WGSL type | align | size | host mirror |
|---|---|---|---|
| `f32` / `i32` / `u32` | 4 | 4 | `float` / `int32_t` / `uint32_t` |
| `vec2<f32>` | 8 | 8 | `float[2]` |
| `vec3<f32>` | **16** | 12 | `float[3]` — **pad to 16** before the next field |
| `vec4<f32>` | 16 | 16 | `float[4]` |
| `mat4x4<f32>` | 16 | 64 | `float[16]` (column-major — transpose on fill) |
| `array<vec3<f32>,N>` | 16 | 16·N | `float[N][4]` (xyz + 1 pad each) |

The rule of thumb: **keep field order identical to the WGSL struct and insert
`uint32_t _padN` so every `vec3`/`vec4`/`mat`/array starts on a 16-byte
boundary.** Enums widen from the C++ `u8`/`int` to `u32` on the GPU. The
existing `_pad0/_pad1` members and offset comments in `compute_layout.h` are the
worked reference.

### B3. (manual) Fill the field in the marshal

The per-dab marshal that populates these structs lives in `GpuStrokeSession`
(`source/debug/gpu_stroke.cc`) — the `bu.* = scene.brush.*` / `cu.* = …` block.
The vk/wgpu dispatchers only `memcpy` the already-filled structs, so this is the
**one** place to add the copy:

```cpp
bu.pinchFactor = scene.brush.pinchFactor;        // BrushUniforms
// or, for ctx state:
cu.global.myBrush.foo = scene.brush.foo;          // CtxUniforms tail
```

Per-kernel `ctx` tails that don't fit the common 96-byte base are overlaid in a
`union` in `ComputeCtxUniforms` (kelvinlet's grab vectors vs. pose's cage) —
only one kernel is live per dispatch. If your new ctx state is brush-specific,
add a `struct` arm to that union rather than growing the base block.

> **Cautionary tale — std140 is unforgiving.** `polygroup`'s `activeGroup`
> aliases kelvinlet's `mu` slot (offset 72, the first post-fixed field) via a
> bit-reinterpret, pinned by a `static_assert` in `gpu_stroke.cc`. It works only
> because the two brushes are mutually exclusive. Don't imitate this for a new
> field — give it its own named, padded slot.

### B4. Regenerate, build, verify

```
node make.mjs codegen
node make.mjs sbrush-verify        # cpp-vs-wgsl A/B + golden
```

A mismatch here (the diff is tolerant: `ATOL=1e-5`, `RTOL=1e-4`) almost always
means a B2/B3 padding or fill bug — the CPU path is the reference, so when cpp
disagrees with wgsl, suspect the host mirror first.

---

## Part C — device dynamics & pre-invocation validation

Every non-`@static` `float` uniform is, for free, a **dynamic-capable** brush
property: a device dynamic (pen pressure, tilt, speed, …) can be bound to it by
**name** and the resolved per-dab value follows a baked response curve. Nothing
kernel-side is needed — it falls out of the codegen registration in A1.

- **Bind a dynamic** with `Brush::addPropDynamicByName(name, deviceType, mix,
  factor)` / `setPropDynamicSampleByName(name, deviceType, i, n, value)` /
  `clearPropDynamicsByName(name)` (`source/brush/brush.h`). The common props
  also have int-keyed wrappers; per-kernel uniforms use the by-name surface.
- **Resolution** happens inside the generated `loadUniformProps`
  (`lookupValue<float>` applies the bound dynamic via the device-input ctx).
  The C++ golden path (`execBrush`) reads the member directly, so dynamics are
  exercised through the composite `execProgram` path and the
  `test_brush_dynamics` gate.
- **Validation runs once at stroke start** (`isFirstOfStep`), before any mesh
  or topology state is touched: `CommandExecutor::validateUniformDynamics`
  scopes to the *active* brush's manifest and rejects the shared-`Brush`-struct
  traps and malformed metadata. A failed validation aborts the stroke — the
  mesh is never mutated — and the messages print to stderr;
  `lastUniformValidationOk()` exposes the verdict. The failure modes:

  | Rejected | Cause |
  |---|---|
  | *stray dynamic* | a dynamic bound to a name that isn't a uniform of the active brush (another kernel's uniform on the shared struct) |
  | *`@static` target* | a dynamic on a uniform declared `@static` (or a non-float) |
  | *unbaked curve* | a device response curve with exactly 1 entry (need 0 = identity, or ≥2 to interpolate) |
  | *out-of-range default* | the manifest default escapes its `@range` |
  | *invalid `@range`* | `rangeMin > rangeMax` or a NaN bound |

  Gate: `tests/test_brush_uniform_validate.cc` covers each mode plus the happy
  path and an integration check that a failed validation leaves every vertex
  unchanged.

### Driving uniforms from the TS bridge

The by-name API above can't be called from JS directly — the binding runtime
can't marshal a JS string into a `util::string` *method arg*. So `CommandExecutor`
also exposes the manifest **index-keyed**, generated from the same data, for the
TS brush UI (`scripts/editors/view3d/tools/sculptcore_bindings.ts`):

- `queryUniformManifest(brushType) -> count` caches the active kernel's manifest
  (and registers its props); `queriedUniformEntry(idx)` returns each
  `BrushUniformManifestEntry` **by pointer** (name/default/range/`dynamic` are
  then readable — string *members* of a bound struct marshal fine, unlike args).
- `clearUniformDynamics(idx)` / `addUniformDynamic(idx, device, mix, factor)` /
  `setUniformDynamicSample(idx, device, i, n, value)` resolve the index to the
  uniform name and delegate to the by-name API.

The bridge enumerates the manifest, skips the int-keyed common props
(`strength`/`radius`/…), and drives each remaining dynamic-capable uniform from
its matching `BrushDynamics` channel. Out-of-range indices are silent no-ops.

## Worked example: a `float pinchFactor` uniform on a GPU brush

1. `kernels/pinch.sbrush`: `uniform float pinchFactor;`
2. `brush.h`: `float pinchFactor = 0.5f;` (+ prop registration if authorable).
3. `compute_layout.h`: append `float pinchFactor;` to `ComputeBrushUniforms`,
   adjust padding so the struct stays 16-byte-rounded (and update the size
   comment).
4. `gpu_stroke.cc`: `bu.pinchFactor = scene.brush.pinchFactor;`
5. `node make.mjs codegen && node make.mjs sbrush-verify`.

CPU-only? Stop after step 2 + `codegen`.

## Checklist

- [ ] Field declared in `.sbrush` (`uniform`/`ctx`, correct type)
- [ ] Host member on `Brush` (`brush.h`) — or a `CommandCtxBase` builtin reused
- [ ] (authorable `float` uniform) give it a default `= n` (and `@range`/`@static`) — registration, load, manifest, and dynamics are then codegen'd; no manual `BrushProp`/binding/`loadProps` edits
- [ ] (per-dab ctx) set host-side each dab (executor or a `host` stage)
- [ ] `node make.mjs codegen`
- [ ] **GPU only:** mirror field in `compute_layout.h` with std140 padding
- [ ] **GPU only:** fill it in `GpuStrokeSession` marshal (`gpu_stroke.cc`)
- [ ] **GPU only:** `node make.mjs sbrush-verify` green

## See also

- [`brush_dsl.md`](brush_dsl.md) — the DSL language reference (§Fields).
- [`brush_compute.md`](brush_compute.md) — compiler pipeline, backends/emitters,
  verification harness.
- [`brush.md`](brush.md) — the brush runtime (how a stroke reaches a kernel).
- `source/brush/compiler/emit_cpp.cc` / `emit_wgsl.cc` — the actual lowerings.
- `source/brush/compute_layout.h` — host mirrors of the GPU uniform blocks.
