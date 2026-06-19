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
node make.mjs install-emsdk          # one-time; git-clones emsdk (pinned commit), installs pinned emsdk + cmake + ninja
node make.mjs configure [wasm|native]  # default wasm
node make.mjs build     [wasm|native]
node make.mjs test      [testName]     # no arg: ctest in build/native; arg: run that one test binary
node make.mjs clean     [wasm|native]  # ninja clean
node make.mjs node      [--smoke]      # build the NW.js/Node N-API addon (.node)
```

Notes:
- `configure`, `build`, and `clean` take an optional `target` positional (`wasm`
  default, or `native`). `test` instead takes an optional test *name* and always
  uses the native build dir (`build/native`): with no name it runs `ctest`; with a
  name it runs that single `<name>.cc_out[.exe]` binary under `build/native/tests`
  or `build/native/source/litestl/tests`.
- Build dirs: WASM → `build/`, native → `build/native/`, Node addon → `build/native-node/`.
- A global `-j` / `--jobs <n>` flag caps `cmake --build` parallelism (passed as
  `--parallel <n>`); omit it to use all cores. Lower it (e.g. `-j 2`) when clang
  OOMs on the heavy template translation units.
- `node make.mjs node` builds `sculptcore_node.node` for the **NW.js** ABI
  (default `--runtime nw`; `--runtime electron` kept as a fallback): cmake-js
  downloads the runtime headers + import lib (`-r nw`) and injects `CMAKE_JS_*`
  during configure, then the addon target (root `CMakeLists.txt`, gated on
  `DEFINED CMAKE_JS_VERSION`) is built with the clang toolchain. The entry is
  `source/napi/napi_entry.cc` (raw C N-API). `--smoke` loads the result in a
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
  props/            property / reflection system (runtime-side)
  gpu/              GPU abstraction (frontend; backends are native-only)
  vulkan/           native Vulkan backend (vk_context/backend/overlay/screenshot)
  core/             aggregate binding registration (initBindings)
  io/               serialization (placeholder)
  window/           GLFW windowing (native only; Vulkan-friendly, no GL context)
  wasm/             Emscripten glue: jslib.js, wasmManager
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

`source/mesh/c-api/` is the external surface used by WASM/JS callers.
Changes here ripple to both the Embind bindings and any generated TS —
touch with care and prefer additive changes. `source/spatial/c-api/`
follows the same convention for spatial-tree construction.

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
even when a GPU node aggregates several leaves. The per-frame
`SpatialTree::update()` pipeline (bounds → tris → normals →
partition → propagate-dirty → buffer regen/slice update → draw
batch) is in [`documentation/spatial.md`](documentation/spatial.md),
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
build; `brushes/all.h` aggregates them and `CommandExecutor::createCommand()`
dispatches the `SculptBrushes` enum to the matching factory. At runtime
`CommandExecutor::execBrush` walks `SpatialNode`s and runs the compiled
kernel through a vertex-iterator factory. Codegen is
`node make.mjs codegen`; cross-backend correctness is gated by
`sbrush-validate` (per-backend compile) and `sbrush-verify` (C++ vs GPU
A/B, bit-for-bit modulo fp). Three docs cover it:
[`documentation/brush.md`](documentation/brush.md) (runtime),
[`documentation/brush_dsl.md`](documentation/brush_dsl.md) (the language),
and [`documentation/brush_compute.md`](documentation/brush_compute.md)
(compiler, build wiring, verification).

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
