# Sculptcore

A C++20 sculpting / mesh engine that builds natively and to WebAssembly via
Emscripten. CMake + Ninja, driven by a small Node dispatcher (`make.mjs`).

See `documentation/projectIndex.md` for the full source-tree map — prefer
reading it before doing wide exploration.

## Commit logs
* When creating commit logs, do not include additions to the CSpell dictionary.
* Keep messages short: a one-line summary of intent, then a few bullets
  calling out the notable changes (with brief sub-bullets for non-obvious
  details). 
* Don't write per-file paragraphs or restate diffs the reader
  can see — mention specific files only when the file *is* the point
  (e.g. a new module). 
* Skip routine churn (lockfiles, formatting, generated files) unless it's load-bearing.  
* Skip the generated typescript files in typescript/ (note that typescript/api does not 
  contain generated files, everything else in typescript/ is generated).

## Build

Use the Node dispatcher rather than invoking cmake/emcmake directly:

```
node make.mjs install-emsdk               # one-time; git-clones emsdk (pinned commit), installs pinned emsdk + cmake + ninja
node make.mjs configure [wasm|native|node]  # no arg: configure all three; `wasm` ⟵ `emsdk` alias
node make.mjs build     [wasm|native|node]  # default wasm; `build node [--smoke]` builds the NW.js/Node N-API addon
node make.mjs test      [testName]          # no arg: ctest in build/native; arg: run that one test binary
node make.mjs clean     [wasm|native]       # ninja clean
```

Notes:
- `configure` and `build` take an optional `target` positional, one of `wasm`,
  `native`, or `node` (`emsdk` is accepted as an alias for `wasm`). **No-arg
  `configure` configures all three targets** (wasm, native, node); `build`
  defaults to `wasm`. `clean` takes `wasm`/`native`. `test` instead takes an
  optional test *name* and always uses the native build dir (`build/native`):
  with no name it runs `ctest`; with a name it runs that single
  `<name>.cc_out[.exe]` binary under `build/native/tests` or
  `build/native/source/litestl/tests`.
- Build dirs: WASM → `build/`, native → `build/native/`, Node addon → `build/native-node/`.
- A global `-j` / `--jobs <n>` flag caps `cmake --build` parallelism (passed as
  `--parallel <n>`); omit it to use all cores. Lower it (e.g. `-j 2`) when clang
  OOMs on the heavy template translation units.
- A global `--release` flag forces `CMAKE_BUILD_TYPE=Release` (optimized, no
  debug info) over the `RelWithDebInfo` default; an explicit
  `SCULPTCORE_CMAKE_BUILD_TYPE` env var still wins. This is what the parent
  repo's Pages CI builds the shipped WASM with (emcc `-O3 -DNDEBUG`, no DWARF /
  `.wasm.map`). The wasm tree is shared between configs, so `build wasm`
  re-runs `configure` when the build dir's cached type doesn't match.
- `node make.mjs build node` builds `sculptcore_node.node` for the **NW.js** ABI
  (default `--runtime nw`; `--runtime electron` kept as a fallback). The
  configure step (`make.mjs configure node`, also run on demand by `build node`
  if the build dir isn't configured) uses cmake-js to download the runtime
  headers + import lib (`-r nw`) and inject `CMAKE_JS_*`; then the addon target
  (root `CMakeLists.txt`, gated on `DEFINED CMAKE_JS_VERSION`) is built with the
  clang toolchain. The entry is
  `source/napi/napi_entry.cc` (raw C N-API). `--smoke` (a `build node` flag)
  loads the result in a
  hidden NW.js window (via the shared `source/napi/napi_smoke.cjs` body) and
  checks `version()`/`bindingCount()` + a sculpt stroke. The runtime version is
  read from `../nwjs/package.json` (override with `--runtime-version`). This is
  the native-addon path from `documentation/plans/native-electron.md`; the
  clang↔runtime link was de-risked in `spike/napi/` (`RESULTS.md`).
- WASM configure runs `emcmake cmake .. -G Ninja -DBUILD_WASM=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`;
  native configure runs `cmake ../.. -G Ninja --toolchain ../../build_files/native-clang.cmake`
  (clang is the required toolchain everywhere). The generator (`-G`) and build
  type (`-DCMAKE_BUILD_TYPE`, default `RelWithDebInfo`) come from the local
  build options below, as do the `-DWITH_ASAN` / `-DWITH_MESHLOG_ABSEIL_HASHMAP`
  flags both targets pass through.
- Every command runs under `node configureEnv.mjs` (with `--emsdk` for WASM) to set up the
  emsdk/PATH environment — don't invoke cmake/ninja/ctest directly.
- `emsdk` is **not a submodule** — `install-emsdk` `git clone`s it and checks out the
  pinned `EMSDK_COMMIT` (hardcoded in `make.mjs`); the resulting `emsdk/` directory is
  gitignored. `emsdkVersion.txt` pins the Emscripten version. `install-emsdk` also
  installs pinned `cmake-4.2.0-rc3-64bit` and `ninja-git-release-64bit` via emsdk and
  activates them `--permanent`, then appends `cmake` to `emsdk/.gitignore` (upstream
  omits it).
- The WASM `build` step deletes `build/sculptcore.{js,wasm}` before linking because
  emcc can silently succeed on compile errors otherwise — don't "optimize" that away.
- Native (non-WASM) builds enable `tests/` and the `sculptcore` executable still links,
  but the primary target is WASM.
- `node serv.mjs` serves `index.html` + the WASM module for browser testing.

### Local build options

The build knobs are no longer hardcoded: `make.mjs` reads them from an optional,
**gitignored** `local-build-options.mjs` (default-export an object) — copy the
tracked `local-build-options.mjs.example` to start. Unknown keys are reported and
in-use keys are echoed at startup. Recognized keys (with defaults):

- `CMAKE_BUILD_TYPE` (`'RelWithDebInfo'`) — feeds `-DCMAKE_BUILD_TYPE` and also
  selects the native-deps combo (`configName` in `tools/deps.mjs`).
- `CMAKE_GENERATOR` (`'Ninja'`) — the `-G` generator.
- `WITH_ASAN` (`false`) → `-DWITH_ASAN=ON`, threaded into the wasm, native, and
  node-addon (`--CDWITH_ASAN`) configures.
- `WITH_NATIVE_MSVC` (`false`) → build the **native** + **node-addon** targets
  with MSVC (`cl.exe`, `build_files/native-msvc.cmake`) instead of clang. Each
  toolchain gets its own build dir (`build/native-msvc`, `build/native-node-msvc`)
  so the two trees never clash. WASM is unaffected (still emcc/clang). Compatible
  with `WITH_ASAN` (uses `/fsanitize=address` + the MSVC `clang_rt.asan_dynamic`
  runtime). The prebuilt OpenBLAS/CHOLMOD deps are still clang-built and shared;
  under MSVC their LLVM-OpenMP `__kmpc_*` refs are satisfied by MSVC's bundled
  `libomp.lib` (+ staged `libomp140.x86_64.dll`), since `/openmp` (vcomp) lacks
  them. No sccache (MSVC caching needs `/Z7`, which `/Zi` defeats).
- `WITH_MESHLOG_ABSEIL_HASHMAP` (`false`) → `-DWITH_MESHLOG_ABSEIL_HASHMAP=ON`
  (use `absl::flat_hash_map` in meshlog; run `extern/fetch_abseil.sh` to clone
  abseil into `extern/` first).
- `BUILD_JOBS` (`0` = all cores) — max parallel compile jobs for every build
  mode (wasm/native/node, debug/sbrush targets, local deps builds); feeds
  `cmake --build --parallel`. The `-j`/`--jobs` CLI flag overrides it per
  invocation.

## Native deps (OpenBLAS + SuiteSparse/CHOLMOD)

Native builds use prebuilt OpenBLAS (LAPACK on) and SuiteSparse/CHOLMOD,
**owned by sculptcore** (not litestl). They are cached per
`{platform}/{toolchain-key}/{config}` in the separate repo
`https://github.com/joeedh/sculptcore-deps.git`, checked out as a **gitignored
sparse clone** at `extern/sculptcore-deps`.

- `node make.mjs deps [config]` fetch-or-builds the combo for `config`
  (`release|relwithdebinfo|debug|asan`, default `RelWithDebInfo`). `configure
  native` runs it automatically and passes the combo dir to cmake as
  `-DSCULPTCORE_DEPS_DIR`.
- Pins live in `openblasVersion.txt` / `suitesparseVersion.txt` (mirroring
  `emsdkVersion.txt`). Driver: `tools/deps.mjs`. CHOLMOD is consumed via the
  `cholmod` INTERFACE target in `extern/CMakeLists.txt` (linked by `source/remesh`).
- **Cache miss → build locally, push manually.** A missing combo is built from
  the pinned tags into `extern/sculptcore-deps/<combo>/{openblas,suitesparse}`
  with a `manifest.json`; the script then **prints** the `git add/commit/push`
  for sculptcore-deps (it does not push). To add a combo by hand the flow is the
  same the script uses: `git -C extern/sculptcore-deps sparse-checkout add
  <platform>/<toolchain>/<config>` (cone mode lets you add new combos to a sparse
  clone), drop the install trees in, commit, push.
- WASM is unaffected: no CHOLMOD there (`cholmod` target only exists native), the
  Eigen `SimplicialLDLT` solver works without it.

## Language / standard

- C++20, extensions `.cc` / `.cpp` (headers `.h`).
- `-Wno-invalid-offsetof` is set under WASM; don't work around offsetof
  warnings, they're expected.
- Avoid STL containers in hot paths — use `litestl::util` equivalents
  (`Vector`, `Map`, `Set`, `Span`, `Array`, `BoolVector`, ...).

## Layout (short)

```
source/
  litestl/          self-contained foundational lib (util, math, platform, path, binding)
  mesh/             mesh data structures + attributes + utils + C API
  meshlog/          sculpt undo/redo log (per-node attribute swaps + topology log)
  brush/            sculpt brushes (sbrush DSL + sbrushc compiler), command executor
  spatial/          spatial acceleration (BVH-style nodes) + C API + shaders
  dyntopo/          dynamic-topology remesh under a sculpt dab (CPU core; M1-M7 done)
  subdiv/           Catmull-Clark refiner + multires grids/store (see below)
  displace/         sculpt-layer compositor + the F3 tangent-frame provider
  vdm/              vector-displacement store, splat, bake, promote
  remesh/           quad remeshing (cross field -> param -> extraction)
  props/            property / reflection system (runtime-side)
  gpu/              GPU abstraction (frontend; backends are native-only)
  vulkan/           native Vulkan backend (vk_context/backend/overlay/screenshot)
  webgpu/           wgpu-native backend + GPU stencil SpMV
  core/             aggregate binding registration (initBindings)
  window/           GLFW windowing (native only; Vulkan-friendly, no GL context)
  wasm/             Emscripten glue: jslib.js, wasmManager
  napi/             NW.js/Node native-addon entry
  debug/            scripted native harness (debug_app)
  app/              application entry (stub)
extern/             vendored: glfw (Vulkan via system SDK)
build_files/        macros.cmake, WASM.cmake, link_wasm.py
tests/              GTest-style; native only (BUILD_WASM=OFF)
```

`source/litestl/` is intended to be reusable and has its own CMakeLists /
tests / docs — treat it as a sub-library, not free-form project code.

## WASM symbol export

The root `CMakeLists.txt` collects a CMake global property `WASM_SYMBOLS`,
comma-joins it, and passes it as `-sEXPORTED_FUNCTIONS=...` to the linker.
Code that needs to be callable from JS must register its symbol via the
`macros.cmake` helpers rather than hand-editing link flags. Linking also
uses `-sMODULARIZE=1 --bind` (Embind).

## litestl::binding

`source/litestl/binding/` is a C++ reflection / type-description system.
It currently uses runtime allocation (`new types::Number<...>`,
`util::Vector<StructMember>`, `util::Map` in `BindingManager`) — this is
**intentional scaffolding**. The design target is a fully `constexpr` /
`consteval` system with compile-time-sized storage.

When touching it:
- Concepts (`ClassBindingReq`, `std::same_as<T>` overloads) already do
  compile-time dispatch — keep that.
- Prefer changes that move toward the constexpr end-state (value
  semantics, `std::array` sized by template param, `consteval`
  `defineBindings()`).
- Don't refactor the whole system to constexpr unless asked, and don't
  flag the current runtime `new`/`Vector`/`Map` usage as a bug — it's
  deliberate.
- The TypeScript generator (`binding/generators/typescript.cc`) consumes
  the descriptors to emit TS bindings for the WASM surface.

## Mesh C API

`source/mesh/c-api/` is the external surface used by WASM/JS callers and by the
Blender addon's ctypes bridge. Changes here ripple to both the Embind bindings
and any generated TS — touch with care and prefer additive changes.
`source/spatial/c-api/` follows the same convention for spatial-tree
construction.

**Adding a function means adding its name to the module's
`wasm_add_symbols` list** (`source/mesh/CMakeLists.txt`), which feeds both the
WASM `-sEXPORTED_FUNCTIONS` and the native shared library's export list. Left
out, a new `extern "C"` function compiles and links cleanly and is simply
invisible at runtime — which looks like a load or ABI failure, not a missing
export. The function inventory is in
[`documentation/mesh.md`](documentation/mesh.md) § *C API*.

## Mesh validate / repair

`Mesh::validateAndRepair()` (`source/mesh/mesh.cc`) checks the topology
(edge-vert refs, face corner loops, disk + radial cycles) and repairs what it
can: it kills unrepairable faces/edges, then rebuilds every disk cycle from the
authoritative edge endpoints and every radial cycle from the face corners. It is
**cheap on a healthy mesh** — the checks are read-only and it early-returns
`0` (no rebuild) when nothing is wrong — so it is safe to call eagerly.
`Mesh::repairMesh()` is the bound, no-arg entry (reflected for JS); the LiteMesh
calls it on load (`litemesh.ts` `loadSTRUCT`) to fix structural corruption baked
into a saved file before the spatial tree is built. Per-error detail goes to
stderr + the `repairLog` vector; the return value is the problem count.

## Binding registration

Module-level reflection registration lives in each module's
`bindings.{h,cc}` (see `mesh/`, `brush/`, `spatial/`, `props/`, `gpu/`,
`meshlog/`). `source/core/bindings.cc` provides the single
`extern "C" initBindings()` entry point that the WASM loader calls; it
invokes every module's `registerBindings(manager)` and registers the
primitive `Vector<T>` instantiations exposed to JS.

## Tests

Native tests (Google Test) live under `tests/` and run only when
`BUILD_WASM=OFF`. There is also a WASM harness (`wasmTest.mjs`).
`source/litestl/tests/` holds litestl-internal tests.

When adding a test, match the existing style (`test_<thing>.cc`) and
wire it through the appropriate `CMakeLists.txt`.

## Debug app

`source/debug/` builds a native scripted harness (`debug_app`) that
drives mesh + spatial + brush + GL end-to-end from a plain-text script,
headlessly by default. It's the right tool for reproducing engine bugs,
running regression scripts, and A/B-testing brush backends — prefer it
over ad-hoc `main()` test programs. Full CLI + verb reference and the
"adding a verb" recipe live in
[`documentation/debugApp.md`](documentation/debugApp.md);
[`documentation/debugging.md`](documentation/debugging.md) covers the
Claude-driven workflow that uses it.

- **Undo-fidelity checks**: the `save_pos` / `assert_pos` verbs snapshot
  every live vertex position under a name and later assert all verts
  returned to it (within `eps`). Bracket a stroke with `save_pos` … stroke
  … `undo` … `assert_pos` to catch undo position corruption (the dyntopo-
  undo regression workflow). See the example in `debugApp.md`.

## Spatial

`source/spatial/` is a BVH-style tree layered over a `mesh::Mesh`
that serves two largely independent layers off the same node set:
*leaves* are the spatial-query / brush-iteration unit (tunable via
`leaf_limit`), and a *subset of nodes* (the "GPU nodes") owns
aggregated VBOs covering every triangle of its subtree
(tunable via `gpu_tri_target`, default 2048). Face/vert ownership is
recorded on the mesh through the `.spatial.{v,f}.node` builtin
attributes, which guarantees each face is rendered exactly once
even when a GPU node aggregates several leaves. The
`SpatialTree::update()` pipeline is split: a per-dab
`updateQueries()` queries half (split/merge → tris → bounds →
normals) plus a per-frame `update(gpu)` that adds the GPU half
(partition → propagate-dirty → buffer regen/slice update → draw
batch); both are in [`documentation/spatial.md`](documentation/spatial.md),
which also covers `castRay`, the GPU partition invariants, the
ownership-attribute pitfall when reusing a mesh across multiple
trees in tests, and the **material draw-shader / requested-attribute**
interface (`setRequestedAttrs` / `setDrawShader`, per-attribute vertex
buffers built by `fill_leaf_attr`, default-filled and never throwing).

## Rendering

`source/gpu/` is a backend-agnostic frame description (shaders,
batches, commands, uniform blocks); `source/vulkan/` is the native
backend that walks it. Uniform blocks have three cadences encoded by
which layer owns the instance: `DrawPipeline::blocks` → `set=0` (per
pass), `DrawBatch::blocks` → `set=1` (per batch),
`DrawCommand::blocks` → `set=2` (per draw). A link pass
(`source/gpu/uniform_link.h`) resolves layer instances to shader
blocks by name, stamps `(set, binding)`, computes std140 offsets, and
pre-fills field defaults — see
[`documentation/rendering.md`](documentation/rendering.md) for the
full object model, link semantics, and the recipe for adding a new
uniform block.

## Brush

`source/brush/` applies sculpt operations to mesh geometry. Brushes are
**not hand-written** — each is authored once in the small `sbrush` DSL
(`kernels/<name>.sbrush`) and compiled by the `sbrushc` host tool
(`compiler/`) to every backend: the reference C++ executor plus WGSL,
SPIR-V, CUDA, HIP, and OpenCL. The C++ output is checked into
`kernels/generated/<name>.brush.gen.h` and consumed directly by the WASM
build. **The enum→factory dispatch is generated as well**: each kernel's
`@tool NAME[, …]` claims `SculptBrushes` items, `brushes/tools.txt` fixes the
ids (append-only — they are persisted in brush assets and the addon bridge), and
codegen emits `brushes/generated/` with `createBuiltinBrush(id, …)` plus the
`Binder` item list. `CommandExecutor::createCommand()` calls it and falls
through to the extras registry, so adding a brush edits no host conditional.
`tests/test_brush_registry.cc` grades the generated roster against an
independent transcription; `tests/test_brush.cc` pins the `{name → id}` table.
At runtime
`CommandExecutor::execBrush` walks `SpatialNode`s and runs the compiled
kernel through a vertex-iterator factory.

**There is a second executor.** `brush/grid_executor.h`'s `GridBrushExecutor`
runs the *same* generated kernels against a multires `GridLevelDomain` instead
of a materialized `mesh::Mesh`: the spatial unit is a `GridTree` leaf,
positions/normals/mask are the domain's dense buffers, neighbours come from its
lattice CSR, and undo capture is `GridStrokeLog` block snapshots rather than the
meshlog. It is deliberately *not* a generalization of `CommandExecutor` — no
dyntopo, no meshlog, no attr overrides, no preview machinery — and it holds no
tool roster: `supportsBrush(brushType, attrs)` is derived from each kernel's own
def, declining only for a capability the domain lacks (an attr layer with no
grid storage, see § *Attributes on the grid domain*). Stroke shape is
`beginStep()` → `applyDab()*` → `endStep()`, where `endStep` folds into the store
through `Multires::gridsWriteback` restricted to the touched verts' occurrence
grids — O(region), not O(level). Gate: `test_grid_stroke`. Codegen is
`node make.mjs codegen`; cross-backend correctness is gated by
`sbrush-validate` (per-backend compile) and `sbrush-verify` (C++ vs GPU
A/B, bit-for-bit modulo fp). Five docs cover it:
[`documentation/brush.md`](documentation/brush.md) (runtime),
[`documentation/brush_dsl.md`](documentation/brush_dsl.md) (the language),
[`documentation/brush_compute.md`](documentation/brush_compute.md)
(compiler, build wiring, verification),
[`documentation/strokeDriverGuide.md`](documentation/strokeDriverGuide.md) (the
host-side contract: what a driver must do around `applyDab` for correct undo,
symmetry, grab/anchored, dyntopo and preview), and
[`documentation/textureScripts.md`](documentation/textureScripts.md)
(standalone `.stex` texture scripts — the one runtime-compiled exception to
"brushes stay compile-time": tcc CPU JIT + stroke-begin WGSL splice,
host/builtin samplers, param slab).

Per-kernel **policy is engine-owned, not host-owned**: the sbrush annotations
(`@grabmode`, `@unbounded`, `@incremental`, `@relaxation`, `@gpu`, `attr … @use(...)`)
are reflected out through the stateless `BrushMetadata` binding
(`queryBrushFlags` / `queryAttrManifest` / `queriedAttrEntry`, plus
`CommandExecutor::filterRadiusFloor`). Hosts query it instead of branching on a
tool name — adding a brush must not require editing a host conditional.

## Debugging

in source/litestl/platform/platform.h:
* litestl::platform::getStackTrace(): returns stack trace as a std::string
* litestl::platform::debugBreak(): break in debugger (calls __debugbreak on windows,
  derefs a nullptr on other platforms/wasm).

## Debugging with source-line prints

When a test or scenario crashes deep inside a header (heap corruption,
double-free, segfault in a destructor, etc.) and a debugger is awkward
to attach — common on the WASM side, but also useful for tracking down
which template instantiation actually runs — sprinkle `__FILE__` /
`__LINE__` prints through the suspect code path:

```cpp
printf("X inside %s\n", __FILE__);
printf("X at line %d\n", __LINE__);
```

Pick a one-letter tag per file (`P`, `B`, `T`, ...) so the interleaved
output is easy to read. In the test's `main()`, call
`setvbuf(stdout, nullptr, _IONBF, 0)` so output isn't lost when the
process aborts. Read the trace to find the last line that printed
before the crash, then narrow from there.

When you're done, **remove every print you added** — these are
debugging scaffolding, not diagnostics that should live in the tree.
Also: `litestl::alloc` is a leak-tracking allocator; if a test crashes
inside `free`/`release` it usually means a real bug, not allocator
noise — don't paper over it by defining `NO_DEBUG_ALLOC`.

## Profiling a periodic hitch (temporary scaffolding)

Run-of-the-mill perf regressions — especially ones that only show up in
the *interactive* live path (e.g. a ~1s stutter while sculpting) — are
chased the same way: add throwaway, `--profile`-gated instrumentation,
narrow the cost to a phase, fix it, then **rip the scaffolding back out**.
The `StrokeProfiler` (`source/debug/profile.h`) phase counters
(cpu/gpu/read, begin/end) are the permanent part and stay; the temporary
part is whatever you bolt on to localize a spike:

- A per-event **SPIKE log** in the hot path (print index +
  time-since-stroke-start + phase split when a dab exceeds a threshold)
  turns a "lags every second" report into evenly-spaced, phase-labeled
  lines — that's how the WGSL read-phase regression was pinned to
  uncached host-visible readback.
- A **FRAME-SPIKE log** wrapping the interactive frame loop catches
  stalls that land in render/present rather than a brush dab (so they
  never reach `addDab`).
- Gate it all behind `scene.profiler.enabled` (`--profile`) so it's
  inert in normal runs, and remember scripted/batch runs won't reproduce
  a live-path-only hitch — hand the user a setup-only script + `--interactive`.

Like the source-line prints above, this instrumentation is **not** meant
to live in the tree: once the fix is confirmed, delete every SPIKE /
FRAME-SPIKE counter and printf you added and leave only `StrokeProfiler`.

## Dynamic topology

Geometry under a sculpt dab is subdivided where edges exceed a target length and
collapsed where they fall below it, tracking the brush radius — triangles only,
attributes interpolated onto new geometry. **Built and shipped through M1–M7;
the 5 M-tri / ≥25 fps target is met on the CPU with no GPU offload.** Design +
post-M7 re-evaluation: [`documentation/dynamic-topology.md`](documentation/dynamic-topology.md);
the perf/cascade work: [`documentation/plans/dyntopo-m7-cascade.md`](documentation/plans/dyntopo-m7-cascade.md).

- **Core** is `source/dyntopo/dyntopo.h` — `runDyntopoRemesh(mesh, center, radius,
  params, seed, cb?, seedVerts?)`. It is spatial/brush/meshlog-free (mutates only
  the `mesh::Mesh`); the caller threads `MeshCallbacks` to keep the spatial tree
  and meshlog current, and passes `seedVerts` (the in-region leaves' verts) so a
  dab is O(brush region), not O(mesh).
- **The round loop** is the Botsch-Kobbelt quartet over maximal independent sets:
  split / collapse / **flip** / **smooth**, with a **graded** target. The
  performance levers are all here and all CPU — chiefly the **length-criterion
  flip sweep** (`do_flips`, M7.2), which breaks the split-spoke cascade (it is
  *not* optional). `DynTopoParams` also has `grade` (sizing field), `do_smooth`
  (tangential, default off), and `max_splits` (per-dab safety valve).
- **Spatial currency** is incremental (`source/spatial/`, M7.6): `add_face_at`
  O(1) anchor placement, deferred batched leaf rebalance, and cadenced merge of
  under-full leaves — `tree->update()` is 2–11 ms at 5 M vs a ~68 s full rebuild.
- **The GPU offload is now optional**, not required — see the design doc's
  post-M7 banner before touching it.
- Regression gates (ctest): `test_dyntopo_cascade` / `_budget` / `_smooth`,
  `test_spatial_dyntopo` / `_merge`. The `bench_dyntopo` debug-app verb is the
  A/B measurement tool (`flip=`, `grade=`, `smooth=`, `max_splits=`, `rebuild=`).

## Subdivision and multires

`source/subdiv/` is the Catmull-Clark refiner (`subdiv.cc`), the multires data
carrier (`grids.cc` — per-cage-corner Ptex grids of frame-relative displacement
plus custom float channels, lz4 chunking, level eviction) and `Multires`
(`multires.cc`), which materializes a level's `mesh::Mesh` + `SpatialTree` from
the stencil chain and the stored displacement. The composition rule per level is
`base = stencil(pos of level below)`, `pos = base + frame·d`; `writeback()`
re-expresses edits as store deltas, skipping bit-identical verts so an edit-free
level switch is lossless. Module map:
[`documentation/projectIndex.md`](documentation/projectIndex.md) §`source/subdiv/`;
plan: [`documentation/plans/displacementAndSubSurf.md`](documentation/plans/displacementAndSubSurf.md)
(workstreams S/X). Gates: `test_multires`, `test_multires_stroke`.

**The frame is the sharp edge here.** `pos = base + frame·d` makes the frame a
lever arm: a perturbation of it is amplified by `|d|`. The multires frame space
is `Multires::parametricFrames()` — normal and tangent derived from the grid's
own `(u,v)` lattice by finite differences on the smoothed base. Every encode
(`storeDispFromPositions`) and decode (`applyDisp`) goes through it, so stored
`d` never depends on a cross-field representative.

It replaced the F3 provider (`source/displace/frames.cc` — smoothed normal plus
a 4-RoSy cross-field tangent) on this path for **correctness first**: a cross
field must *choose* a representative from a 4-fold-symmetric tensor, with
nothing pinning that choice across rematerializations, so a rebuild could decode
unchanged stored `d` rotated by a multiple of 90° — detail flipping in one step.
`test_multires`'s `gateFrameStability` measures both: nudging one cage vertex of
a subdivided cube *reverses* a provider tangent (dot −0.999962) where the
lattice frame holds at 0.999970. The lattice frame is also far cheaper — it
needs no `mesh::Mesh`, uses only `+ - * / sqrt` (so backends agree bitwise), and
is embarrassingly parallel, where the provider's Gauss-Seidel smoothing is
deliberately serial. That is most of what made mode-enter fast (2433 ms → 26 ms
for the base+frames phase at 1 M verts / level 4).

`captureDetailToVdm` is where the two frame spaces now coexist, and it is
**open**: it writes texels in the lattice frame while the VDM consumers
(`vdm_bake`, `vdm_promote`, `vdm_splat`) still decode against the provider's
`FRAME_*_ATTR`. Capture must convert — or the VDM path must adopt the lattice
frame — before it is wired to a host. Nothing outside the c-api reaches it today.

### Attributes on the grid domain

`subdiv/grid_attrs.{h,cc}` (`MultiresAttrs`, reached as `Multires::gridAttrs()`)
owns everything on the grid domain that is *not* displacement: per-grid-sample
layers for UV, colour and face sets, ptex-bilinear for point attributes and
Catmull-Clark face-varying for UV maps, keyed on the host's `uv_smooth`.

**Where a write may land is a storage class, never a tool list.**
`storageFor(name, type, flags)` answers with one of three:

- `Temp` — `AttrFlag::TEMP` scratch the host never stores, so a kernel may
  always author it per grid element.
- `Host` — the host declared it through `declareHostAttr` (c-api
  `Multires_declareHostGridAttr`), so it persists per grid element and a kernel
  may author it. Blender declares exactly one: the scalar `mask`.
- `Derived` — everything else. An engine-owned *cache*, re-subdivided from the
  cage attribute, and a brush may not author a cache.

`brush/grid_attr_bind.h::gridAttrPlan` is the single enforcement point: a
writable `Derived` attribute is `Unbindable`, so that kernel takes the mesh path
and each dab writes the **cage** (`Multires::scatterVertFloat4ToCage` /
`scatterFaceIntToCage`), which re-derives the grids it touched from what the
cage now holds. On a Blender host that is colour and face sets. Capability is
the host's to declare, not the user's to toggle — a session switch that could
override the storage class existed briefly and was deleted once the class was
made to decide first.

Two staleness stamps keep the derived layers honest. `generation()` covers any
derived rebuild; `cageGeneration()` is bumped by `noteCageEdit()` when a cage
write-back has reconciled *its own* level and left every other level's copy
behind. `Multires::materialize` compares it against the slot's `derivedGen` and
re-derives a resident slot on the way in — lazily on purpose: one whole-level
re-derive per level switch, versus one per dab for levels nobody is looking at.

**Down-propagation is a different operator from the level transition.**
`GridsStore::restrictChannelDown` is 9-point full weighting (the transpose of
the prolongation), spent one step at a time by `Multires::propagateAttrsDown`
against per-(channel, level) debt (`LevelData::downPending`, set by
`noteAttrEdit`, settled in `setActiveLevel` alongside the positional
`propagateDown`). `restrictLevelToBelow`'s injection is the lossless left
inverse used when a level is *dropped*, and the two must not be confused: full
weighting re-run on a level that owes nothing would smooth the user's own coarse
edits away, which is why the debt flag gates it. One documented deviation: an
on-seam *tap* is counted once per incident grid, so on a two-grid seam the
centre weight comes out 1/3 rather than 1/4. Normalization absorbs it
(constants stay fixed points), but the filter is mildly seam-biased and that is
a choice, not an accident.

**The mask splits into a seed and an edit, and they must not be confused.**
`Multires_writeDomainMask` (c-api) is the whole-domain *seed* — it overwrites
every sample of a level and owes nothing to any other level, so it runs
propagation-free. `Multires_editDomainMask` is the *edit* flavor: it writes the
touched samples at the edited level, prolongates the **delta** up through the
finer levels (`GridsStore::prolongateChannelEditUp`, 4-tap and bounded to the
touched box; a finer level nobody has authored is seeded whole instead), and
leaves the downward direction to the ordinary restriction debt. Consumers pull:
`Multires::maskGeneration()` bumps on `noteMaskChange()` and readers compare
stamps — the old push protocol (`GridStroke_syncMask`) is gone.

**Sculpt layers get a fourth binding answer: `LayerScratch`.** A kernel that
writes a `SCULPT_LAYER` handle binds per-dab scratch instead of the channel
itself — and only when the host has armed a live edit target
(`MultiresAttrs::hasLayerEditTarget`, i.e. `Multires::writebackChannel() > 0`);
with no target the plan declines and the brush takes the mesh path. The
executor folds the scratch into positions after `execPost` (`co += w·delta`),
and stroke end writes the accumulated residual back into the target layer's
grid channel. This is what LAYERDRAW rides.

**Kernels that need mesh 1-rings take the cage-smooth route.** The grid
lattice CSR carries no cage-vertex neighbourhood, so COLORSMOOTH runs
`CageSmoothSession` (`brush/cage_smooth.h`): a tree-less `CommandExecutor`
pass over one synthetic `SpatialNode` holding the dab's grids' owning cage
verts, with falloff from a limit-position snapshot; a per-dab epilogue
re-derives the incident grids from the edited cage.

The net roster is pinned by `gateGridsRoster` (`tests/test_grid_stroke.cc`):
session-free, 20 of the 23 built-ins run grids-native
(`SculptBrushesBuiltinCount - 3`), the decliners being FEATURE_ALIGN (cross-
field attr layer), ENHANCE (per-vert displacement layer) and LAYERDRAW — and
arming a sculpt-layer edit target flips LAYERDRAW to supported, leaving
exactly 2.

Gates: `test_multires_attrs` (including `gateResidentSlotFreshness` and the
down-propagation gate) and `test_grid_stroke`. The embedding addon's headless
gates — `verify_multires_color`, `verify_multires_face_sets`,
`verify_grid_channels`, `verify_multires_uv_parity` — cover the same rules from
the host side.

## Quad remeshing

See [`documentation/quad-remeshing.md`](documentation/quad-remeshing.md) for the
design, pipeline, and status of the quad-remesh module (`source/remesh/`).

## Submodules

- Keep submodules checked out at the HEADs of their current branches, pulling and
  merging as needed — **except** `extern/imgui`, which stays pinned at its recorded
  commit (third-party, version-locked). (`emsdk` is no longer a submodule — it is
  git-cloned and pinned by `make.mjs install-emsdk` and is gitignored.)
- The default branch must always link submodules at their default-branch commits
  (never pin the default branch's gitlinks to a submodule feature branch).
- **Commit this repo and its submodules together** whenever their branch names
  match, or both are on their default branches: make the submodule commit, then
  bump the gitlink, as one logical change. The pinned exception (`extern/imgui`)
  is excluded — bump it deliberately, never as part of a co-commit. The same rule
  applies one level up: when sculptcore's branch matches
  its parent superproject's, they are committed together too.
- **Parent on a branch, submodule on its default branch:** do not silently commit
  or advance the submodule's shared default branch. Ask the user whether they want
  to commit and/or push the submodule's default branch (and bump the gitlink)
  before doing so.
- **Worktree teardown:** before removing a worktree, every submodule sitting on its
  default branch — except the pinned `extern/imgui` — must be committed and pushed,
  so no work is lost when the checkout goes away.

## Conventions

- Namespaces: `litestl::util`, `litestl::binding`, etc.
- Prefer editing existing files over adding new ones; the module
  granularity is already fairly fine.
- Don't add backward-compat shims when renaming internals — this is a
  single-repo project.
- Keep comments minimal; explain *why* only when non-obvious.
- Path handling: use `litestl::path` utilities rather than ad-hoc string
  manipulation or raw `std::filesystem` in engine code.
- **Pressure-test every plan after formulation.** Launch multiple adversarial
  agents with fresh contexts (one lens each — per-workstream buildability,
  semantics/correctness, cross-cutting seams and gates) whose brief is to
  *kill* the plan against the actual code, then fold every surviving finding
  back into the plan before any phase starts. A citation audit is not a
  substitute: it verifies references, not buildability.

## Code Comments

The repo-wide rules in the root `CLAUDE.md` "Code Comments" section apply here
(doc vs non-doc distinction, the 3-line non-doc limit, `CLAUDENOTE:` for temp
scaffolding). The C++-specific additions:

- **Non-doc comments must use C++ `//` line comments**, never C-style `/* … */`.
- **Approved long comments are the one exception**: a non-doc comment that
  exceeds 3 lines must first be approved by the user, then recorded in
  `approvedLongComments.md` (sculptcore root) as
  `{path}:{function}:{one-line summary}` (`path` relative to the sculptcore
  root). An approved long comment uses a C-style `/* … */` block **without** a
  leading `*` on its continuation lines. Entries in `approvedLongComments.md` are
  exempt from the length limit and the per-file budget — don't flag or shorten
  them in a later audit.
- **Doc comments must use the `/** … */` form** — the block comment that
  documents the signature directly below it (file header, function, method,
  struct/class). Use `/** … */` rather than a `///` run or a plain `//` block,
  so doc comments are visually distinct from non-doc `//` comments. They are not
  subject to the 3-line length limit, but stay concise.

## Prose

These rules govern every piece of prose in the repository. They apply to code comments, to
this file, and to everything under `documentation/`.

- **Write plain declarative prose — no epigrams.** State the constraint or decision
  directly: "An empty answer is deliberate and is passed to the model as-is", not "Empty is an
  answer — silence, said out loud." If a sentence needs a second read to parse, rewrite it.
  Specific patterns to catch:
  - **Inverted syntax and personification** — the sentence performs rather than informs.
  - **Metaphorical equations** — "The leak scan is the refusal", "what ships is identity",
    "the project as commands". The connector word varies — do not get hung up on "is"
    versus "as". Say what happens instead: "Refuses if the leak scan finds a known name
    still in the body."
  - **Fragment openers that defer the subject — never use this pattern.** Naming a placeholder
    and then withholding the real content behind a colon or a dash is always wrong: "The
    redactor to scan a report with: the one that wrote it, else one built from the project as it
    stands." Lead with a complete sentence and name each case as you reach it. A doc comment is
    not an exception, and deleting the label is not the fix, because the apposition left behind
    is still headless. Supply a predicate instead. Write "Draws the links beneath the node
    frames in screen space." rather than "The link underlay: a screen-space canvas beneath the
    node frames." or the bare "Screen-space canvas beneath the node frames."
  - **Double negatives** — "the palette cannot be relied on not to". State the positive claim.
  - **Pronouns and ellipses that point outside the sentence** — "the second case", "asking
    twice is how…" — each sentence should carry its own referents.
  - **"Clause A, else B" constructions** — "Resolve a push's destination: the named window
    when it still exists, else the focused window falling back to the most recently focused
    one." Spell out the cases as ordinary sentences instead: "Pushes to the named window if it
    still exists. Otherwise pushes to the focused window, or the most recently focused window
    if none is focused."
  - **Adverbs hung off the end of a noun phrase** — "the next pointerdown anywhere", "the
    handler above". The adverb postmodifies the noun, but the reader cannot tell on first pass
    whether it attaches to the noun or to the clause's verb, and an event or API name coined
    from a verb ("pointerdown") re-parses as a clause when an adverb follows it. Attach the
    qualification to a verb, or state it as its own fact: "the listener is on `window`".
  - **Non-assertive words under a definite** — "any", "anywhere", "ever" range over
    alternatives, so they fight a definite description that names exactly one thing. "A press
    anywhere dismisses it" reads fine; "the next pointerdown anywhere" does not.
  - **Rhetorical emphasis** — bold and italics inside a sentence mark the clause the author
    found most interesting, not the one the reader needs first. Put the load-bearing claim in
    the first sentence and drop the markup. A bolded lead-in that labels a Markdown bullet is
    structure rather than emphasis, and is fine.
  - **A head noun that is not what the thing is** — a module of commands documented as "The
    prompt an asset is generated from, as commands" asserts that the module is a prompt, then
    retracts it through a preposition. Lead with the head noun that names the thing —
    "Commands for the prompt an asset is generated from" — and demote the rest to a
    complement. A trailing ", as X" or ", in the form of X" is the same metaphorical equation
    above smuggled in through an adjunct.
- **Reserve backticks for code symbols.** Backticks belong on identifiers, types, commands,
  and file globs the reader will type. A file path cited mid-sentence as a reference —
  documentation/NodeEditor.md §3 — takes none, because marking it up gives it the same weight
  as the identifiers around it and dilutes them. Markdown link text is the one exception and
  keeps its backticks, where the marking separates a path from the prose around it rather than
  competing with nearby identifiers.
- **Bracket a subordinate alternative rather than fencing it with commas.** Parentheses mark the
  material as skippable, so the reader gets a complete sentence either way; paired commas leave
  it unclear whether the second comma closes an interpolation or opens a new clause. Write
  "Dropping onto itself (or onto a neighbor it would split against) is not a rip". Drop any comma
  that would follow the closing bracket — it separates the subject from its verb.
