# Custom addon-authored `.sbrush` kernels (compile-time "extra kernel dirs")

Plan for letting a downstream consumer (the Blender addon repo) carry its own
developer-authored `.sbrush` files that compile into `sculptcore_capi.dll`
alongside the built-ins. Phase A is engine work (this repo); Phase B is the
addon repo (`sculptcore-blender-addon`).

## Context

The addon maps Blender brush types to engine kernels by name (`mapping.py` `_MAP` →
reflected `SculptBrushes` enum int → `execBrush`). All kernels live in the engine repo
(`source/brush/kernels/*.sbrush`), compiled at build time by the `sbrushc` host
tool into checked-in C++ headers and hand-wired in three places: `brushes/types.h`
(enum 0..21 + reflection `Binder`), `brushes/all.h` (includes), and
`brush_executor.h` `createCommandImpl` (dispatch switch, whose `default:` calls
`abort()`). There is no way for the addon to add a brush without editing the engine.

**Goal:** the addon repo can carry its own developer-authored `.sbrush` files that get
compiled into `sculptcore_capi.dll` alongside the built-ins. **Strictly compile-time** —
no runtime parsing/interpretation. The engine gains a generic "extra kernel dirs"
build option and stays addon-agnostic.

Verified enablers:
- `queryUniformManifest(int)` dispatches through `createCommand` (brush_executor.h:1241),
  so one dispatch hook also covers uniform introspection → `engine_props.py` UI works
  automatically for extras.
- Python bindings are fully reflection-generated; new enum items registered by the
  `Binder` appear in Python with zero binding work.
- No brush id persists anywhere (meshlog/undo/.blend are id-free; addon resolves by
  name per stroke) → cross-build id drift for extras is safe.
- The addon never uses the GPU stroke path (`GpuBrush_*` unused in stroke.py) →
  CPU-only extras are sufficient; no WGSL/SPIR-V for extras in this wave.

**Key constraint (document prominently):** sbrush `uniform`/`ctx` fields (other than
ctx builtins like `surfaceNo`, `strokeDir`, `mousePos`...) lower to `ctx.brush.X` —
they must be *existing* `Brush` members (brush.h). Extras cannot introduce new
uniforms; violations fail the C++ compile of the generated header (acceptable, and a
build-time error as desired). New tunables need an engine-side member (wave-2 idea:
a name-keyed float store on `Brush`).

## Design summary

- **Registration:** generated "extras" unit only; built-ins keep hand wiring. The
  built-in switch encodes non-derivable per-brush policy (plane-family sharing,
  CsrNbr/LiveDiskNbr selection, enhance pre-pass, grab forcing) — a full refactor is
  all risk, no addon benefit. The extras path is fully generated, so a dropped-in
  `.sbrush` is either registered or a build error.
- **Ids:** built-ins 0..21 fixed; extras get `SculptBrushesBuiltinCount + i`, i over
  stems sorted bytewise (dirs in option order). Generated code references the
  constant, never a literal 22.
- **Generation happens in-build via CMake custom commands** (native targets only;
  `sbrushc` is already a real target there), outputs land in the **build tree**
  (`<build>/sbrush_extra/gen/`) — engine checkout stays clean.
  `SBRUSH_SKIP_NATIVE_CODEGEN` is unaffected (it only gates host-side regen of the
  checked-in built-in headers).
- **Gate:** when the cache var is nonempty, root CMake adds a build-wide
  `SCULPTCORE_EXTRA_BRUSHES=1` compile definition + include dir (build-wide, not
  `__has_include`, to avoid ODR divergence between TUs). Warn+ignore under
  `BUILD_WASM`.
- **Addon brushes live at `<addon repo>/brushes/`** (repo root — build-time source,
  not runtime addon files; `build-blender-dist.mjs` copies `sculptcore_addon/`
  verbatim into installs, so `.sbrush` must not live there).

## Phase A — engine (ordered commits)

> Engine is on its default branch: per engine/CLAUDE.md co-commit rules, ask the
> user before committing/pushing the engine default branch + gitlink bump.

**A1. `sbrushc` registry mode** — `source/brush/compiler/sbrushc_main.cc`
(+ small `emit_registry.cc` if cleaner):
`sbrushc --registry --out-dir=<dir> --in=<extra.sbrush>... --builtin=<builtin.sbrush>... --reserved=<names>`
Reuses `lex`/`parse`; derives enum item = upper(@brush name), factory symbol from the
brush class name. Collision checks (exit nonzero, clear message): duplicate stems
among extras or vs built-ins (include ambiguity), duplicate `@brush` names,
case-insensitive enum-name collision vs built-ins/reserved (pass the 22 enum item
names literally via `--reserved=DRAW,...,ENHANCE` maintained beside the CMake wiring,
covering hand-named divergences like `FEATURE_ALIGN` vs `@brush("featurealign")`).
Emits via existing `writeFileIfChanged`:
- `sculptcore_extra_brushes_enum.inc` — bare `e->addItem("NUDGE",
  sculptcore::brush::SculptBrushesBuiltinCount + 0);` lines, included inside
  `Binder<SculptBrushes>::bind()`.
- `sculptcore_extra_brushes.gen.h` — includes each `<stem>.brush.gen.h`;
  `inline constexpr int extraBrushCount`; `inline bool extraBrushUsesForNeighbor(int)`
  (cases only for for_neighbor extras); templated
  `command::createExtraBrush<TYPES, AccMode>(int id, bool csrNeighbors, def&) -> bool`
  switch calling generated factories (for_neighbor extras emit the two-branch
  CsrNbr/LiveDiskNbr form mirroring SMOOTH; others single-instantiation).

**A2. `source/brush/brushes/types.h`** — add
`inline constexpr int SculptBrushesBuiltinCount = 22;` +
`static_assert(int(_SculptBrushes::ENHANCE) == SculptBrushesBuiltinCount - 1, ...)`;
inside `bind()` after the ENHANCE item, `#ifdef SCULPTCORE_EXTRA_BRUSHES
#include "sculptcore_extra_brushes_enum.inc" #endif`.

**A3. New `source/brush/brushes/extra.h`** (checked in, ~25 lines): under
`#ifdef SCULPTCORE_EXTRA_BRUSHES` include the gen header; else inline no-op fallbacks
(`extraBrushCount = 0`, `extraBrushUsesForNeighbor` → false, `createExtraBrush` →
false). `all.h` gains `#include "extra.h"` last.

**A4. `source/brush/brush_executor.h`** —
- `createCommandImpl` `default:` → before the printf/abort:
  `if (command::createExtraBrush<CommandExecutor, AccMode>(int(brushType),
  effectiveNeighborMode() == NeighborMode::Csr, def)) return;`
- `brushNeedsLiveLinks` (line ~1067): append
  `|| (extraBrushUsesForNeighbor(int(brushType)) && neighborMode != NeighborMode::Csr)`.

**A5. CMake** —
- Root `CMakeLists.txt`: `set(SCULPTCORE_EXTRA_KERNEL_DIRS "" CACHE STRING ...)`
  (semicolon-separated absolute dirs); when nonempty (native only):
  `include_directories(${CMAKE_BINARY_DIR}/sbrush_extra/gen)` +
  `add_compile_definitions(SCULPTCORE_EXTRA_BRUSHES=1)`; warn+ignore under BUILD_WASM.
  Retire the dead `SBRUSH_REGEN_ON_BUILD` comment (CMakeLists.txt:182).
- `source/brush/CMakeLists.txt`: gated block (style-matching the existing WGSL
  backend blocks at :66-245): per-dir `file(GLOB ... CONFIGURE_DEPENDS "*.sbrush")` +
  `list(SORT)`; per-kernel `add_custom_command(sbrushc --backend=cpp --in=... --out=
  ${CMAKE_BINARY_DIR}/sbrush_extra/gen/<stem>.brush.gen.h)`; one registry
  `add_custom_command` (extras + built-in kernel list + `--reserved`);
  `add_custom_target(sbrush-extra ALL ...)` + `add_dependencies(brush sbrush-extra)`.

**A6. `make.mjs`** — `--kernels-extra <dir>` (repeatable /
`path.delimiter`-separated) on `configure`, `build`, `bundle`; resolves to absolute
forward-slash paths → `-DSCULPTCORE_EXTRA_KERNEL_DIRS="a;b"` in `configureTarget`
(~L1364); `bundle`/`buildPythonCapi` does a cheap cache refresh when the cached value
differs so flag changes don't need manual reconfigure. Optional `EXTRA_KERNEL_DIRS`
key in `local-build-options.mjs.example`. Fix the stale "regenerates kernels
in-build" comment at make.mjs:525-526.

**A7. Docs** — "Extra kernel dirs" section in `documentation/brush_compute.md`
(option names, build-tree outputs, cpp/CPU-only, SKIP_NATIVE_CODEGEN non-interaction,
the Brush-member uniform constraint, host-stage OK / executor-pre-pass kernels not
available to extras); pointer from `addingSBrushUniforms.md`.

## Phase B — addon repo (separate commits, gitlink bump per co-commit rules)

**B1. `brushes/nudge.sbrush` + `brushes/README.md`** — proof-of-concept: Blender's
`'NUDGE'` type is currently unmapped, and the kernel needs no new Brush members
(`strokeDir` is maintained by `updateStrokeFrame()` on the execBrush path;
`wingscrape.sbrush` proves `ctx float3 strokeDir;`). Reuses existing member
`projection` as its tunable to exercise the engine_props chain. Sketch (finalize
syntax against `brush_dsl.md`; SPDX header if the DSL accepts comments, else `//`):

```
@brush("nudge")
brush Nudge {
  ctx float3 surfaceNo;
  ctx float3 strokeDir;
  uniform float radius;
  uniform float projection @static = 1.0 @range(0.0, 1.0);
  vertex void apply(inout Vertex v) {
    float s = strength(v.co) * (1.0 - v.mask);
    s *= sampleBrushTex(v.co, surfaceNo);
    if (s == 0.0) { continue; }
    float3 t = strokeDir - surfaceNo * (dot(strokeDir, surfaceNo) * projection);
    float l = length(t);
    if (l < 1e-6) { continue; }
    v.co += (t / l) * (s * radius * 0.5);
  }
}
```

README documents: the Brush-member uniform constraint, naming/collision rules,
per-build ids, cpp/CPU-only.

**B2. `tools/build-blender-dist.mjs`** — step 4: if `<repo>/brushes/` contains
`.sbrush` files, append `'--kernels-extra', <repo>/brushes` to the bundle args; log a
hint when extras exist but `--skip-engine` is set (DLL won't be rebuilt).

**B3. `sculptcore_addon/mapping.py`** —
- `_MAP['NUDGE'] = ("NUDGE", {"projection": 1.0})` (explicit reset — `projection` is a
  shared Brush member also written by the plane family; same pattern as SHARP's
  `pinch` reset at mapping.py:38).
- Harden `kernel_enum` (mapping.py:280): `items[...]` → `.get()`; return None when the
  vendored DLL lacks the name → existing clean `'CANCELLED'` + warning path
  (stroke.py:414-416).

**B4. `sculptcore_addon/engine_props.py`** — harden `_walk_manifests` (line 83):
`items.get(kernel_name)`, `continue` when missing — today one missing name would
raise KeyError and (via `register()`'s broad except) disable ALL generated props.

## Verification

1. **Regression, no extras:** `node make.mjs build native` + `node make.mjs test` —
   fallback `extra.h` path compiles; behavior unchanged. Also `build wasm`
   (all.h now includes extra.h).
2. **With extras:** `node make.mjs configure python --kernels-extra=<addon>/brushes &&
   node make.mjs build python` — confirm the three generated files under
   `build/python/sbrush_extra/gen/` and the DLL links.
3. **Negative tests:** copy `draw.sbrush` into the extras dir → registry step fails
   with duplicate-name error. Kernel using a nonexistent uniform → legible C++ error
   naming the missing Brush member.
4. **Reflection check, no Blender:** point `SCULPTCORE_CAPI_PATH`/`PATH` at
   `build/python`, `python/` on `sys.path`: assert
   `"NUDGE" in mgr.get("sculptcore::brush::SculptBrushes").items` (value 22);
   throwaway executor (recipe = `engine_props._walk_manifests`) →
   `queryUniformManifest(22)` includes `radius` and `projection` (range 0..1).
5. **Incremental:** touch `nudge.sbrush` → only extra commands rerun; delete it →
   `CONFIGURE_DEPENDS` re-glob drops the enum item.
6. **Full chain:** `node tools/build-blender-dist.mjs --skip-blender` → headless
   Blender script asserts NUDGE in the reflected enum and the generated `projection`
   prop on `Brush.sculptcore`.
7. **Stale-DLL tolerance:** with an old DLL restaged, addon loads; NUDGE stroke
   reports "brush type has no kernel" and cancels; other generated props register.
8. **Manual in Blender:** Nudge stroke drags geometry tangentially along the stroke
   direction; `projection` slider in the engine props UI changes behavior; undo
   restores (meshlog capture is kernel-generated — nothing to wire).

## Open risks

- **Brush-member uniform constraint** is the real ceiling for addon kernels; wave-2:
  name-keyed float store on `Brush` targeted by codegen for non-member uniforms.
  Design sketch (agreed with user):
  - `Brush` gains only `util::Vector<float> namedFloats` + inline slot accessors;
    the registry generator dedupes store-uniform names across extras and assigns
    dense slot indices (`inline constexpr int`), so the vertex stage reads
    `ctx.brush.namedFloats[kSlot_X]` — array index, no per-vertex hashing.
  - **No per-uniform tag** (user rejected `@store`): member-vs-store is inferred
    from a hand-maintained static method beside the struct,
    `Brush::builtinPropNames(Vector<string>&)`; `sbrushc` (same tree) includes
    brush.h and calls it at generation time. Listed name → `ctx.brush.X` member
    lowering as today; unlisted → store slot.
  - Honesty tripwire: built-in kernels never use the store, so built-in codegen
    errors on any uniform not resolving to the member path (catches a member
    added to `Brush` but not listed). Stale listed names are caught by the C++
    compile of generated headers that use them.
  - Defaults: generated `ensureExtraUniformDefaults(brush)` called from
    `createExtraBrush` (per command creation) resizes + fills from a generated
    table, so the hot-path read stays branch-free.
  - Introspection: `BrushUniformManifestEntry` gains `storeSlot` (−1 =
    member-backed) + the DSL default; `engine_props.py` writes store uniforms via
    `setNamedFloat(slot, v)` instead of `setattr` (its `hasattr(brush, name)`
    filter would otherwise drop them), and the int-keyed slot sidesteps the
    string-marshalling limit noted on `BrushFloatOverride`.
  - Deferred (user): a possible future explicit tag would be `@builtin` (assert
    member-backed) rather than `@store` — tightens the inference, decide later.
  - Scope: floats only, CPU only (GPU marshalling via `compute_layout.h` would
    need a float-array extension); slots are per-build like extra brush ids —
    safe, nothing persists them.
- `default:` `abort()` still crashes on a truly unknown int id — the addon gates by
  name; optional engine hardening later (warn + no-op command).
- Executor-side pre-pass coupling (enhance/featurealign-style) not available to
  extras — documented, not solved.
- `file(GLOB CONFIGURE_DEPENDS)` assumes Ninja (project standard).
- Blender NUDGE parity/strength tuning is a follow-up, not acceptance criteria.
