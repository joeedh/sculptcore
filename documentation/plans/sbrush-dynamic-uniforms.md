# Plan: codegen-driven dynamic sbrush uniforms (Route B)

> **Status: shipped (Waves 0–5 + cleanup).** Every scalar `float uniform` is a
> codegen-registered, default/range-carrying, dynamics-capable prop, validated
> per stroke (`CommandExecutor::validateUniformDynamics`). The TS bridge drives
> per-kernel uniforms through the index-keyed `CommandExecutor` manifest surface
> (`queryUniformManifest`/`queriedUniformEntry` + by-index dynamics). Gates:
> `test_brush_props` / `test_brush_dynamics` / `test_brush_uniform_validate`.

Make every scalar `uniform` declared in a `.sbrush` kernel a first-class brush
**property** — registered, default/range-carrying, drivable by device dynamics
(pen pressure/tilt/…), and **validated before the brush is invoked** — without
hand-maintaining three parallel lists. Driven entirely by codegen from the
kernel's `uniform` declarations.

Background / feasibility discussion that produced this: see
[`addingSBrushUniforms.md`](../addingSBrushUniforms.md) (the manual "Route A"
path) and the dynamics primitives in `source/props/prop_dynamics.h`.

## Why this is mostly plumbing

The hard machinery already exists and is **not** touched by this plan:

- **Per-property dynamics** (`props::Dynamics` = stack of `DynamicDevice`
  layers, each a `DeviceType` + baked response `curveTable` + mix mode/factor),
  evaluated by `StructProp::lookupValue<float>(name, default, &deviceInputCtx)`.
- **Per-dab evaluation already runs** — `brush->loadProps()` is called every dab
  in `brush_executor.h:579`, applying dynamics into the cached scalar members.
- **No GPU/backend work.** Dynamics resolve to a plain scalar CPU-side *before*
  `GpuStrokeSession` marshals it into the uniform block. A dynamic uniform is a
  static scalar by marshal time — `emit_wgsl.cc` / `compute_layout.h` /
  `gpu_stroke.cc` are unaffected.
- **A per-brush codegen manifest pattern already exists**: `attr` fields emit
  `BrushAttrManifestEntry` into `BrushCommandDef::attrs`, which the executor
  resolves per dab (`brush_command.h:35,191`). This plan mirrors it with a
  *uniform* manifest — the same shape, the same seam.

What's missing is the wiring that turns a `uniform` declaration into a
registered, dynamic-capable, validated prop. Today that wiring is three
hand-edited lists (`Brush()` ctor `structDef_`, `loadPropsWithDevices`,
`writeProps`) plus a `BrushProp` enum that only covers 5 props and can't name a
custom uniform. There is even constexpr scaffolding in `source/brush/props.h`
(`BaseProps[]`, `validate_prop`, `BrushProps::lookup_prop`) gesturing at exactly
the manifest this plan builds — we are finishing that thought and feeding it
from the compiler.

## End state

```sbrush
@brush("kelvinlet")
brush Kelvinlet {
  uniform float mu = 1.0 @range(1e-6, 100.0);   // default + range; dynamic-capable
  uniform float nu = 0.4 @range(0.0, 0.499);
  uniform float seed = 0.0 @static;             // opt OUT of props/dynamics
  ...
}
```

- `mu`/`nu` are auto-registered props with defaults and ranges, drivable by
  pressure/tilt, validated at stroke start, no hand-edits to `brush.h`.
- A misconfigured dynamic (stray binding from another brush, non-float target,
  unbaked curve, out-of-range default) is caught **before** the first dab and
  surfaced as a structured error, not a silent no-op or a bad stroke.

## Design overview

| Layer | Today | After |
|---|---|---|
| DSL field | `uniform float mu;` (name+type only) | `+ default, range, @static/@dynamic` (IR `Field`) |
| Codegen | nothing per uniform | emit `BrushUniformManifestEntry` + generated `registerUniformProps` / `loadUniformProps` |
| Registration | hand-written `structDef_` ctor list | manifest-driven, per active brush |
| `loadProps` | hand-written member list | generated `loadUniformProps(brush, ctx)` per brush + a small fixed `loadCommonProps` |
| Dynamics target | `propDynamics(int propId)` via `BrushProp` enum (5 entries) | `propDynamics(const char *name)` — any registered float uniform |
| Validation | none (null = silent no-op) | `validateUniformDynamics()` at stroke start |

**Key invariant to respect:** there is **one shared `Brush` struct** holding
every kernel's uniforms as members (`mu` exists even when the SMOOTH brush is
active). So registration, dynamics-targeting, and validation must all be scoped
to the **active brush's manifest** (keyed by `SculptBrushes`), or stale
cross-brush dynamics leak in. The manifest is what provides that scoping.

---

## Wave 0 — uniform manifest plumbing (no behavior change)

Mirror the existing `attr` manifest for uniforms; nothing consumes it yet.

- `brush_command.h`: add `BrushUniformManifestEntry { string name; float def;
  float min; float max; bool hasRange; bool dynamic; bool isFloat; }` and
  `Vector<BrushUniformManifestEntry> uniforms;` on `BrushCommandDef`
  (alongside `attrs`).
- `emit_cpp.cc` (`create<Name>Brush`): emit one
  `def.uniforms.append(BrushUniformManifestEntry{...})` per `FieldKind::Uniform`
  field, right beside the existing `def.attrs.append(...)` loop (lines
  ~1068-1079). Scalar `float` → `isFloat=true`; vec/array uniforms still get an
  entry with `isFloat=false` (registerable as plain props, never dynamic).

**Deliverable / gate:** `node make.mjs codegen` regenerates `*.brush.gen.h`;
diff shows only the new `def.uniforms` lines; build still green; existing
`sbrush-verify` goldens unchanged.

---

## Wave 1 — DSL default + range metadata

Give the manifest real defaults/ranges instead of the magic literals currently
buried in `loadPropsWithDevices`.

- **IR** (`ir.h` `Field`): add `bool hasDefault; double defaultValue; bool
  hasRange; double rangeMin, rangeMax; bool dynamicCapable = true;`.
- **Lexer/parser**: extend `parseField` (`parser.cc:130`) to accept a trailing
  `= <number>` initializer (reuses the `Assign` token already handled for
  `attr` boundNames) and a field-trailing attribute for range / opt-out. Reuse
  the existing `@brush(...)` attribute tokenization (`At` + ident + paren args)
  rather than inventing bracket tokens:
  - `= 1.0` → default
  - `@range(1e-6, 100.0)` → min/max
  - `@static` → `dynamicCapable = false` (and skip prop registration entirely
    for host-set-only values)
  - multi-var decls (`uniform float a, b;`) keep working; a default/range
    attaches to each declared name (decide: shared vs per-name — recommend
    per-name, so `uniform float a = 1, b = 2;` is the only form that sets
    distinct defaults; bare names default to 0).
- Emit `default`/`min`/`max`/`dynamic` into the Wave 0 manifest entry.

**Deliverable / gate:** parser unit tests for default/range/`@static`; a golden
`.gen.h` for one kernel showing the populated manifest; clear parse errors for
malformed ranges.

---

## Wave 2 — manifest-driven registration & loadProps

Delete the per-uniform hand-maintained lists; generate them.

- `emit_cpp.cc`: emit two generated functions per brush into the `.gen.h`:
  - `registerUniformProps(props::StructDef &sd)` — one `sd.Float32(name, name)`
    (or `Bool`/`Int32`) per non-`@static` uniform, with the default seeded.
  - `loadUniformProps(Brush &brush, props::DeviceInputCtx *ctx)` — one
    `brush.<name> = brush.props.lookupValue<float>("<name>", <default>, ctx);`
    per scalar float uniform (this is what applies dynamics). Generated, so it
    stays in lockstep with the kernel; it writes the **typed members** the CPU
    lowering already reads (`ctx.brush.mu`).
  - Wire both onto `BrushCommandDef` (new `std::function` slots
    `registerProps` / `loadUniformProps`, set in `create<Name>Brush`).
- `brush.h`:
  - Split `loadProps()` into `loadCommonProps()` (the always-present base set —
    strength/radius/spacing/planeoff/autosmooth/invert) + a call to the active
    brush's `def.loadUniformProps`. **Remove `mu`/`nu` from the ctor
    `structDef_` list, from `loadPropsWithDevices`, and from `writeProps`** —
    they become kelvinlet's generated wiring.
  - At stroke begin (or first `createCommand`), call `def.registerProps` against
    `brush.props.struct_def` (idempotent: register if missing). This is where a
    custom uniform becomes a real prop.
- `brush_executor.h`: per-dab path calls `loadCommonProps()` then
  `def.loadUniformProps(*brush, &brush->deviceInputCtx)` instead of the
  monolithic `loadProps()` (the float-override snapshot loop at lines 566-576
  stays, but see Wave 3 for name-keying it).

**Risk:** the single-shared-`Brush` subtlety. `registerUniformProps` for the
active brush must not clobber another brush's same-named prop — names are
globally unique across kernels in practice (`mu` only on kelvinlet), but add a
codegen check / runtime assert that two manifests never declare the same name
with different type/default.

**Deliverable / gate:** `sbrush-verify` goldens **unchanged** (this wave is a
refactor — same values, sourced from the manifest instead of hand lists).
`test_brush_props` (new) asserts kelvinlet `mu`/`nu` register + load identically
to the old hardcoded path.

---

## Wave 3 — name-keyed dynamics

Let any registered float uniform receive a dynamics stack.

- `brush.h`: add name-keyed siblings to the `propId`-keyed API —
  `propDynamics(const char *name)` (the underlying `struct_def->lookup(name)` is
  already name-keyed; only the `brushPropName(propId)` indirection is in the
  way), and `addPropDynamicByName` / `clearPropDynamicsByName` /
  `setPropDynamicSampleByName`. Keep the `BrushProp` enum + int-keyed methods as
  thin wrappers (back-comp for the TS bridge's existing common-prop calls).
- `brush_executor.h`: name-key the per-command float-override loop (lines
  566-576) the same way so overrides can target custom uniforms too.
- Bind the new methods in `Brush::defineBindings()` for the bridge.

**Deliverable / gate:** `test_brush_dynamics` — configure a pressure dynamic on
kelvinlet `mu`, push device samples across a synthetic stroke, assert the
per-dab `brush.mu` follows the baked curve (and that an identical run with no
device configured is bit-identical to the static path).

---

## Wave 4 — validation prior to invocation

The headline requirement. A `validateUniformDynamics()` pass run once at stroke
start (in `execBrush`, before the dab loop / `createCommand`), scoped to the
**active brush's manifest**:

- every configured dynamic on `brush.props` names a uniform present in the
  active manifest (rejects stray cross-brush bindings — the shared-struct trap);
- the target prop is a registered `Float32` (i.e. `dynamicCapable`);
- each device layer's `curveTable` is either empty (identity) or ≥2 entries
  (baked) — never a 1-entry partial bake;
- the resolved default/value sits within the declared `@range` (clamp **or**
  reject — recommend clamp + warn for value, reject for an inverted/NaN range);
- `DeviceType`s are in range.

Returns a structured `UniformValidationResult { bool ok; Vector<string>
messages; }`. On failure: skip the brush command and surface the messages (debug
log + a result the bridge can read). Validation is **per stroke**, not per dab,
since the binding is configured once per stroke.

**Deliverable / gate:** `test_brush_uniform_validate` covering each failure mode
(stray dynamic, non-float target, unbaked 1-entry curve, out-of-range default,
inverted range) + the happy path. A failing validation must not mutate the mesh.

---

## Wave 5 — bridge / TS surface (cross-layer; may split out)

Expose the manifest so the TS brush UI can present custom uniforms as bindable
channels (today the bridge only knows the 5 `BrushProp` commons).

- Add `Brush::uniformManifest(SculptBrushes)` (or a free query) returning the
  active brush's manifest entries (name, default, range, dynamic) and bind it.
- Bridge: enumerate the manifest to build dynamics-binding UI; route its
  configure calls through the Wave 3 name-keyed API.
- Update `TODO.md` if this introduces a new cross-layer consumer.

This wave is the only one that reaches outside sculptcore; the engine-only
feature (Waves 0-4) is shippable and testable without it (drive dynamics from a
debug-app verb / test).

---

## Cleanup wave

- Strip any `CLAUDENOTE:` scaffolding; demote keep-worthy notes to ≤3-line
  permanent comments.
- Docs: extend [`addingSBrushUniforms.md`](../addingSBrushUniforms.md) (the
  manual lists it describes are now codegen-driven — rewrite Part A/B around the
  manifest), add a `@range`/`@static`/default section to
  [`brush_dsl.md`](../brush_dsl.md) (§Fields), and note the manifest in
  [`brush_compute.md`](../brush_compute.md) (§"How the C++ output links in").
- Wire the new `test_brush_*` binaries into ctest.

---

## File touch-list (quick reference)

| File | Waves |
|---|---|
| `source/brush/compiler/ir.h` (`Field`) | 1 |
| `source/brush/compiler/lexer.{h,cc}` (default/`@` tokens) | 1 |
| `source/brush/compiler/parser.cc` (`parseField`) | 1 |
| `source/brush/compiler/emit_cpp.cc` (`create<Name>Brush`, new gen fns) | 0,2 |
| `source/brush/brush_command.h` (`BrushUniformManifestEntry`, `BrushCommandDef`) | 0,2 |
| `source/brush/brush.h` (split loadProps, name-keyed dynamics, register) | 2,3 |
| `source/brush/brush_executor.h` (load split, override name-key, validate hook) | 2,3,4 |
| `source/brush/props.h` (retire/realize the constexpr scaffolding) | 2 |
| `source/brush/kernels/*.sbrush` (add defaults/ranges where wanted) | 1+ |
| `tests/test_brush_*` | 2,3,4 |
| `source/brush/bindings.cc` + TS bridge | 5 |

## Effort & sequencing

- **Waves 0-1**: ~1 day (manifest + grammar; small, well-bounded).
- **Wave 2**: ~1-1.5 days (the refactor of the three lists; main risk is the
  shared-struct registration — guard it). Gated by *unchanged* goldens.
- **Waves 3-4**: ~1-1.5 days (name-keying is mechanical; validation is a single
  scoped function with focused tests).
- **Wave 5**: separate, bridge-dependent.

Total engine-only (0-4): **~3-4 focused days**, no architectural blockers, no
GPU work. Each wave is independently gated by existing or new tests; Wave 2's
"goldens unchanged" is the safety net that the refactor preserves behavior
before any new capability is added on top.

## Out of scope (deliberately)

- GPU-side dynamics evaluation — unnecessary; dynamics resolve to a scalar
  before marshalling.
- Auto-generating the GPU host mirror (`compute_layout.h`) / marshal from the
  manifest — a worthwhile *separate* follow-up (would also subsume the manual
  steps in `addingSBrushUniforms.md` Part B), but not required for dynamic
  uniforms.
- Non-scalar dynamics (vec3 driven by a device) — float scalars only, matching
  `Float32Prop::dynamics`.
- Reverse-mode autodiff or any DSL semantic change beyond field metadata.
