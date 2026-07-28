# Brush compute stack — `sbrush`

Sculpt brushes are authored once in a small DSL (`sbrush`) and compiled to
every backend sculptcore targets: the native/WASM C++ executor (the
reference), WebGPU (WGSL), Vulkan (SPIR-V), CUDA, HIP, and OpenCL. This
document covers the *compute stack* — the compiler, the build wiring, and the
verification harness. The language surface is documented separately in
[`brush_dsl.md`](brush_dsl.md); the original design rationale (and the parts
still deferred) lives in [`brush_compute_dsl.md`](brush_compute_dsl.md).

The high-level brush *runtime* (how a stroke reaches a kernel) is in
[`brush.md`](brush.md).

## Where the source lives

```
source/brush/
  kernels/                 # .sbrush source — one file per brush
    draw.sbrush  clay.sbrush  smooth.sbrush  inflate.sbrush  pinch.sbrush
    sharp.sbrush mask.sbrush  kelvinlet.sbrush pose.sbrush  texdraw.sbrush
    graddraw.sbrush          # autodiff demo (validate-only, not a tool)
    generated/             # checked-in C++ output (<stem>.brush.gen.h)
    ir/                    # intrinsics.{h,cc} — the backend-agnostic op table
  compiler/                # the sbrushc host tool
    lexer.{h,cc}  parser.{h,cc}  ir.{h,cc}
    emit_cpp.cc  emit_wgsl.cc  emit_cuda.cc  emit_opencl.cc
    sbrushc_main.cc  CMakeLists.txt
```

`sbrushc` is a host-only tool — `compiler/CMakeLists.txt` `return()`s early
under `BUILD_WASM`, so it is built only in native configurations. It links
just `util` + `math`.

## Pipeline

`sbrushc` is a straight-line `lex → parse → emit` driver
(`sbrushc_main.cc`). There is no separate semantic-analysis or
optimization pass — the parser produces a typed tree and each emitter walks
it directly.

```
.sbrush ──lex()──▶ tokens ──parse()──▶ Brush (typed AST) ──emit{Cpp,Wgsl,Cuda}──▶ backend source
```

CLI (`sbrushc_main.cc`):

```
sbrushc --backend=<cpp|wgsl|spirv|cuda|hip|opencl> --in=<file.sbrush> --out=<file>
        [--dry-run]       # print to stdout, don't write
        [--dump-tokens]   # print the token stream and exit
        [--eol=<auto|lf|crlf>]  # line endings for written files (default auto)
```

`--backend=spirv` emits WGSL — SPIR-V is produced downstream by `tint`
(see *Backends*). The writer skips a file whose contents are byte-identical
to what's already on disk, so re-running codegen doesn't churn CMake
timestamps (`writeFileIfChanged`).

Output is written with the line endings git would check the file out with:
`--eol=auto` reads `core.eol` / `core.autocrlf` (the same rule as
`tools/eol.mjs`, shared by `genTS.ts` and `make.mjs`), so the checked-in
`*.brush.gen.h` don't show up as autocrlf-only diffs — and, because the
byte-compare above happens *after* the conversion, a freshly checked-out tree
isn't rewritten wholesale. `--eol=lf|crlf` forces one explicitly.

### Lexer (`lexer.{h,cc}`)

Hand-written scanner producing a `Vector<Token>`. `TokKind` covers
punctuation, the operator set (`+ - * /`, comparisons, `&& || !`, compound
assigns `+= -= *= /=`), literals, and the keyword set: `brush`, `uniform`,
`ctx`, `vertex`, `reduce`, `host`, `in`/`out`/`inout`, `if`/`else`,
`return`/`continue`, `true`/`false`, `for`, `for_neighbor`, `struct`,
`texture`.

### Parser (`parser.{h,cc}`)

Recursive-descent; builds the `Brush` tree (`parseBrush → parseStage →
parseStmt → parseExpr`). It tracks `currentBrush` so struct-typed names
resolve. No types are inferred beyond what the grammar fixes — `Expr.type`
is mostly `Unknown` and emitters rely on the declared types and the
intrinsic table.

### IR (`ir.{h,cc}`)

The "IR" is the parsed tree itself — plain C++ structs using
`litestl::util::Vector`, no STL, no SSA. Key definitions (`ir.h`):

| Type | Role |
|---|---|
| `Brush` | root: `attrName` (`@brush("draw")`), `cppName`, `fields`, `structs`, `textures`, `stages` |
| `Field` (`FieldKind` Uniform/Ctx) | a `uniform`/`ctx` declaration; floats also carry `hasDefault`/`defaultValue`, `hasRange`/`rangeMin`/`rangeMax`, and `dynamicCapable` (from `= n` / `@range` / `@static`) |
| `Stage` (`StageKind` Vertex/Reduce/Host) | a stage body + params |
| `Param` (`ParamDir` In/Out/InOut) | stage/texture parameter |
| `StructDef`, `TextureDef` | user `struct` / inline `texture` block |
| `Stmt` (`StmtKind`) | Block, DeclLocal, Assign, If, For, Continue, Return, ExprStmt, NeighborLoop |
| `Expr` (`ExprKind`) | LitFloat/Int/Bool, Ident, Member, Index, Binary, Unary, Call, Paren |
| `TypeKind` | Void, Bool, Int, Float, Float2..4, Vertex, Struct, Array, Unknown |
| `BackendKind` | **Cpp, Wgsl, Spirv, Cuda, Hip, Opencl** (count drives the intrinsic table) |

`grad(expr, var)` needs no grammar support — it parses as an ordinary
`Call` and is intercepted by name in the emitters (see *Autodiff* below).

## Backends and emitters

Each emitter is a self-contained `struct Emit` with the same shape:
`emitExpr`, `emitStmt`, `emitPrelude`/`run`, a per-`TypeKind` type mapper,
and `locals`/`indent` bookkeeping. **C++ is the reference**; the others must
match it bit-for-bit modulo floating point.

| Backend | Emitter | Output | Validator | Notes |
|---|---|---|---|---|
| C++ | `emit_cpp.cc` | `<stem>.brush.gen.h` (templated functor) | C++ compiler | The only backend whose output links into `libbrush`. Checked into `kernels/generated/`. |
| WGSL | `emit_wgsl.cc` | `<stem>.wgsl` | `tint` | One `@compute @workgroup_size(64)` kernel, one workgroup per spatial node. |
| SPIR-V | `emit_wgsl.cc` | `<stem>.spv` | `tint --format=spirv` → `spirv-val` | Reuses the WGSL emit; a direct `emit_spirv.cc` could replace the tint step later without build changes. |
| CUDA | `emit_cuda.cc` | `<stem>.cu` | `clang -x cuda --cuda-device-only -S` | |
| HIP | `emit_cuda.cc` | `<stem>.hip` | `clang -x hip --cuda-device-only -S` | Shares the CUDA emitter; a `target` field (`BackendKind::Cuda`/`Hip`) selects only the thread-index prelude (nvvm sreg vs. amdgcn). |
| OpenCL | `emit_opencl.cc` | `<stem>.cl` → `<stem>.spv` | `clspv` → `spirv-val` | Buffers/uniforms become kernel args (OpenCL 1.2 has no program-scope global pointers); `brush_*` helpers are macro-bound to those args. |

GPU device source is self-contained: each emitter writes a prelude
(`float2/3/4` algebra, the `sc_*` math helpers, and the
`brush_falloff`/`brush_strength`/`brush_sample_tex` helpers) so the kernel
needs no headers and `clang --cuda-device-only -nogpuinc -nogpulib` can
syntax-check it without a GPU or toolkit install.

### Intrinsics (`kernels/ir/intrinsics.{h,cc}`)

Builtins (`length`, `dot`, `mix`, `sin`, `strength`, `sampleBrushTex`, …)
are table-driven. Each `IntrinsicDef` declares a name, return type, arity,
arg types, and one emit *pattern* per backend, indexed by `BackendKind`:

```cpp
INTR_CWGO("length", TypeKind::Float, 1, ARG1(TypeKind::Float3),
        "($0).length()", "length($0)", "sc_length($0)", "length($0)")
//        ^cpp           ^wgsl         ^cuda/hip        ^opencl
```

`$0,$1,…` are the already-lowered argument expressions. A `nullptr` slot
means "not lowered on this backend" and the emitter errors if the intrinsic
is used there. SPIR-V's slot stays `nullptr` because it reaches the GPU via
WGSL. `findIntrinsic(name)` is the lookup; emitters call it from their
`Call` case and fall through to user textures / dotted calls otherwise.

### How the C++ output links in

The cpp emitter writes a template per stage into
`kernels/generated/<stem>.brush.gen.h` (e.g. `draw`, `drawPre`).
`brushes/all.h` includes every `.gen.h`; `brushes/types.h` holds the
`SculptBrushes` enum; `CommandExecutor::createCommand()` dispatches the enum
to the matching template. So a new brush is wired by codegen + an enum entry
+ a dispatch case (see [`brush.md`](brush.md)). The `.gen.h` files are
committed because the WASM build consumes them directly — it never runs
`sbrushc`.

From the float-uniform metadata the cpp emitter also fills, per brush, a
uniform **manifest** and the `registerProps` / `loadUniformProps` closures on
`BrushCommandDef`. These drive automatic prop registration, name-keyed device
dynamics, and the pre-invocation `validateUniformDynamics` pass — so a new
authorable uniform needs no hand-written registration. See
[`addingSBrushUniforms.md`](addingSBrushUniforms.md) (Part C) and
[`plans/sbrush-dynamic-uniforms.md`](plans/sbrush-dynamic-uniforms.md).

## Build-system integration

Each backend is gated by a CMake option (`source/brush/CMakeLists.txt`); the
native and WASM trees share one `kernels/` set and one `sbrushc`.

```
SBRUSH_BACKEND_CPP     ON   # reference; the only linkable output
SBRUSH_BACKEND_WGSL    OFF
SBRUSH_BACKEND_SPIRV   OFF
SBRUSH_BACKEND_CUDA    OFF
SBRUSH_BACKEND_HIP     OFF
SBRUSH_BACKEND_OPENCL  OFF
SBRUSH_VALIDATE_ALL    OFF  # CI mode: run each backend's external validator
```

For each enabled non-cpp backend, CMake globs `kernels/*.sbrush` and adds one
custom command per file: run `sbrushc --backend=<name>`, then (when
`SBRUSH_VALIDATE_ALL=ON`) pipe the output through that backend's validator
and fail the build on non-zero exit. Outputs land in
`build/<dir>/sbrush_out/<backend>/`. Only the cpp output is a
`target_sources` link input; everything else is a build-only artifact. The
CUDA/HIP/OpenCL rules share an `sb_gpu_backend` macro.

## Extra kernel dirs (out-of-repo kernels)

A downstream consumer (e.g. the Blender addon repo) can carry its own
`.sbrush` files and have them compiled into the engine alongside the
built-ins — strictly at build time, no runtime parsing:

```
cmake  -DSCULPTCORE_EXTRA_KERNEL_DIRS="C:/path/a;C:/path/b"   # cache var, absolute dirs
node make.mjs configure|build|bundle --kernels-extra <dir>    # repeatable; the driver flag
```

- **Wiring** (`source/brush/CMakeLists.txt`): per `.sbrush` a custom command
  runs `sbrushc --backend=cpp` into `<build>/sbrush_extra/gen/` (the source
  tree stays clean), plus one `sbrushc --registry` invocation that emits
  `sculptcore_extra_brushes_enum.inc` (enum items, included by
  `brushes/types.h` inside the reflection `Binder`) and
  `sculptcore_extra_brushes.gen.h` (`extraBrushCount`,
  `extraBrushUsesForNeighbor(int)`, and the `createExtraBrush` factory
  dispatch, consumed via the checked-in `brushes/extra.h` shim). The root
  gate adds a build-wide `SCULPTCORE_EXTRA_BRUSHES=1` define + include dir;
  without extras `extra.h` compiles inline no-op fallbacks.
- **Ids are per-build**: extras get `SculptBrushesBuiltinCount + i`, `i` over
  dirs in option order, stems sorted bytewise per dir. Nothing persists these
  ids (undo/.blend/meshlog are id-free; the addon resolves kernels by name),
  so drift across builds is safe.
- **Collisions fail the build**: duplicate file stems (extras vs extras or vs
  built-ins — the generated include would be ambiguous), duplicate `@brush`
  names, and case-insensitive enum-name collisions against the built-in enum
  items (passed to `--registry` via `--reserved`, maintained beside the CMake
  wiring — they are not derivable from `@brush` names, e.g. `FEATURE_ALIGN`
  vs `@brush("featurealign")`).
- **cpp/CPU only**: extras compile through the reference C++ backend only; no
  WGSL/SPIR-V/CUDA outputs, so no GPU stroke dispatch for extras.
- **Uniforms are member-backed or store-backed**: names listed in
  `Brush::builtinPropNames` (brush.h — the hand-maintained member registry,
  consulted by `sbrushc` at generation time) lower to `ctx.brush.<name>` as
  members; any *other scalar float* uniform in an extra kernel gets a dense
  per-build slot in the `Brush.namedFloats` store (`kExtraSlot_<name>`,
  assigned by the registry, DSL defaults seeded at command creation via the
  generated `ensureExtraUniformDefaults`; the manifest reports the slot in
  `storeSlot`, and callers write it with `Brush::setNamedFloat`). Non-float
  uniforms still need an engine-side member. Built-in kernels never use the
  store — for them an unlisted name is a codegen error, which keeps
  `builtinPropNames` honest. Kernels sharing a store-uniform name share the
  slot, so their DSL defaults must agree (registry error otherwise).
- **No executor pre-pass coupling**: kernels that rely on hand-written
  executor pre-passes (enhance / featurealign style) can't be authored as
  extras; the vertex/face stages plus `for_neighbor` (which auto-selects
  CsrNbr/LiveDiskNbr like SMOOTH, including the live-links stroke rule) are
  the supported surface.
- **`SBRUSH_SKIP_NATIVE_CODEGEN` doesn't interact**: it only gates host-side
  regeneration of the *checked-in built-in* headers (`make.mjs codegen`);
  extra kernels always regenerate in-build via their custom commands.
- Native targets only — under `BUILD_WASM` the cache var warns and is
  ignored (the WASM build has no `sbrushc`).

## `make.mjs` commands

Drive everything through the Node dispatcher, never raw cmake:

| Command | What it does |
|---|---|
| `node make.mjs codegen` (alias `sbrush-build`) | Run `sbrushc --backend=cpp` over every kernel into `kernels/generated/`. Runs automatically before configure/build. |
| `node make.mjs configure native --backends=cpp,wgsl,spirv` | Translate `--backends=` to `-DSBRUSH_BACKEND_<X>=ON`. |
| `node make.mjs sbrush-validate <backend>` | Emit + run the external validator for one backend over all kernels. |
| `node make.mjs sbrush-verify [--regen]` | The cpp-vs-wgsl A/B + golden harness (below). |
| `node make.mjs webgpu-verify` | Replay the same A/B scripts through real Dawn/WebGPU and diff the GPU readback against the native reference. |

## Verification

Three layers, weakest to strongest:

1. **Syntactic** — every backend's emitter output passes its validator
   (`tint`, `spirv-val`, `clang --cuda-device-only`, `clspv`). This is the
   per-backend gate, run by `sbrush-validate` and CI.
2. **Cross-backend + regression** (`sbrush-verify`) — for each
   `tests/scripts/brush_backends/<brush>_ab.txt`, the native `debug_app`
   runs one deterministic dab under `set_backend backend=cpp`, `undo`s, and
   reruns it under `backend=wgsl`. The harness then asserts the two
   `dump_state` JSONs agree *and* the cpp dump matches
   `tests/golden/<brush>.json`. The diff is tolerant numeric
   (`ATOL=1e-5`, `RTOL=1e-4`), keyed on an order-independent coordinate
   fingerprint (`co_sum`, `co_sqsum`); the spatial `flag` bitmask is ignored
   (it tracks update history, not output). `--regen` rewrites goldens from
   the cpp dump.
3. **Real GPU** (`webgpu-verify`) — `debug_app --gpu-capture` records the
   exact per-binding bytes, then `tests/webgpu/replay.mjs` replays through
   Dawn (software Vulkan, headless) and compares the readback bit-exact.

### Runtime GPU dispatch

`runBrushStrokeGPU` (`source/debug/script.cc`) is the native path that loads
a kernel's `.spv`, marshals the mesh + uniforms, dispatches one ≤64-vertex
workgroup per node-chunk per dab, and reads `co` back. Its `switch` on
`SculptBrushes` is the authoritative brush→kernel map (DRAW→`draw`,
SMOOTH→`smooth` with neighbors, MASK→`mask`, KELVINLET→`kelvinlet`, …). A
brush only participates in the GPU A/B if it has both a `SculptBrushes` entry
and a case here; `graddraw` has neither, so it is validate-only.

## Autodiff (`grad`)

`grad(expr, var)` is forward-mode autodiff implemented entirely in the
emitters — there is no IR-level transform. When the emitter hits a `grad`
call it sets `gradVar` to the rendered `var` and rewrites the first argument
through `emitDual`, then reads `.d`:

- scalar → `sbdual { v, d }` (d = ∂/∂var, a float3)
- float3 → `sbdual3 { v, dx, dy, dz }` (3-column Jacobian)
- `var` seeds the identity Jacobian (`sb_seed3`); every other term is a
  zero-derivative constant (`sb_c`/`sb_c3`); intrinsics map to `sbd_*`
  chain-rule helpers.

A dual prelude (the structs + `sbd_*`) is emitted only when a brush uses
`grad` (`brushUsesGrad()`). cpp/cuda use overloaded `sbdual` operators; wgsl
and opencl have no operator overloading, so binary ops emit as
`sbd_add/sub/mul/div`. The rewrite is the same shape on all six backends, so
the gradient is bit-identical modulo fp. `graddraw.sbrush` is the demo; the
slice history is in [`plans/sbrush_autodiff.md`](plans/sbrush_autodiff.md).
Reverse-mode remains deferred.

## Adding a backend

1. Add the `BackendKind` enum member (`ir.h`) — its position indexes the
   intrinsic pattern table, so fill the new slot in `intrinsics.cc` (or leave
   `nullptr` where it routes through an existing backend, as SPIR-V does
   through WGSL).
2. Write `emit_<backend>.cc` (mirror an existing emitter's `struct Emit`),
   wire it into `sbrushc_main.cc`'s backend switch and `compiler/CMakeLists.txt`.
3. Add the `SBRUSH_BACKEND_<X>` option + custom-command block in
   `source/brush/CMakeLists.txt`, the validator probe, and a `make.mjs`
   `sbrush-validate` arm.
4. If the backend can dispatch at runtime, extend `runBrushStrokeGPU`.
