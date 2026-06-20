# Sculptcore

A C++20 digital-sculpting / mesh engine that builds **natively** and to
**WebAssembly** (via Emscripten). It provides the geometry core for a host
3D content-creation app — dynamic remeshing under the brush, sculpt brushes,
feature-preserving boundary constraints, an undo/redo log, a spatial
acceleration tree, and a backend-agnostic renderer.

Sculptcore is **not** an application on its own; it is embedded in a host DCC
app through one of two interchangeable runtimes:

- **WASM** — the browser build; a TypeScript runtime over Emscripten linear memory.
- **Native N-API** — the Electron build; a C++ reflection runtime that
  dereferences real pointers.

Both sit behind a single `IWasmInterface`, so the host code is backend-agnostic.

## Highlights

- **Dynamic topology** — geometry under a sculpt dab is subdivided where edges
  exceed a target length and collapsed where they fall below it, tracking the
  brush radius, with attributes interpolated onto new geometry. The CPU core
  meets a 5-million-triangle / ≥25 fps target with no GPU offload required.
- **Compiled brushes** — brushes are authored once in the small `sbrush` DSL and
  compiled by `sbrushc` to a reference C++ executor plus WGSL, SPIR-V, CUDA, HIP,
  and OpenCL. Cross-backend correctness is gated by A/B verification.
- **Feature-preserving boundaries** — seam / sharp edge constraints that survive
  remeshing.
- **Sculpt undo/redo log** — sparse per-element attribute swaps plus a full
  topological create/change/kill log, robust to mid-stroke retopology.
- **Spatial tree** — a BVH-style tree serving both spatial queries / brush
  iteration (leaves) and aggregated GPU draw buffers (GPU nodes), kept current
  incrementally during a stroke.
- **Reflection / binding system** (`litestl::binding`) that drives the WASM
  Embind surface, the generated TypeScript bindings, and the native N-API runtime.

## Building

Use the Node dispatcher (`make.mjs`) rather than invoking cmake/emcmake directly.
clang is the required toolchain everywhere.

```sh
node make.mjs install-emsdk                 # one-time: git-clones + pins emsdk, cmake, ninja
node make.mjs configure [wasm|native|node]  # no arg: configure all three (emsdk = wasm alias)
node make.mjs build     [wasm|native|node]  # default: wasm; `build node [--smoke]` = Node/NW.js N-API addon
node make.mjs test      [testName]          # no arg: ctest (build/native); arg: run one test binary
node make.mjs clean     [wasm|native]
```

Build directories:

| Target | Directory | Notes |
| --- | --- | --- |
| WASM | `build/` | `-DBUILD_WASM=ON`, linked with `-sMODULARIZE=1 --bind` (Embind) |
| Native | `build/native/` | clang toolchain; enables `tests/` + the `sculptcore` executable |
| Node addon | `build/native-node/` | `sculptcore_node.node`, built for the Electron ABI via cmake-js |

Notes:

- `configure`, `build`, and `clean` take an optional `wasm`/`native` positional
  (default `wasm`). `test` takes an optional test *name* and always uses
  `build/native`: no name runs `ctest`; a name runs that single test binary.
- Every command runs under `node configureEnv.mjs` to set up the emsdk / pinned
  toolchain — don't invoke cmake/ninja/ctest directly.
- `emsdk` is **not** a submodule; `install-emsdk` git-clones it at the pinned
  `EMSDK_COMMIT` and installs pinned cmake + ninja (all gitignored). The
  Emscripten version is pinned in `emsdkVersion.txt`.
- `node serv.mjs` serves `index.html` + the WASM module for browser testing.

### Native dependencies (OpenBLAS + SuiteSparse/CHOLMOD)

Native builds use prebuilt OpenBLAS (LAPACK on) and SuiteSparse/CHOLMOD, cached
per `{platform}/{toolchain}/{config}` in a separate
[`sculptcore-deps`](https://github.com/joeedh/sculptcore-deps.git) repo, checked
out as a gitignored sparse clone at `extern/sculptcore-deps`. `node make.mjs deps
[config]` fetches-or-builds the combo (`configure native` runs it automatically).
WASM is unaffected — there is no CHOLMOD there, and the Eigen `SimplicialLDLT`
solver is used instead.

## Project layout

```text
source/
  litestl/    self-contained foundational lib (util, math, platform, path, binding)
  mesh/       mesh data structures + attributes + utils + C API
  meshlog/    sculpt undo/redo log (per-node attribute swaps + topology log)
  brush/      sculpt brushes (sbrush DSL + sbrushc compiler), command executor
  spatial/    spatial acceleration (BVH-style nodes) + C API + shaders
  dyntopo/    dynamic-topology remesh under a sculpt dab (CPU core)
  remesh/     quad remeshing
  props/      property / reflection system (runtime-side)
  gpu/        GPU abstraction frontend (backend-agnostic frame description)
  vulkan/     native Vulkan backend (vk_context/backend/overlay/screenshot)
  napi/       native N-API runtime + addon entry (raw C N-API)
  core/       aggregate binding registration (initBindings)
  wasm/       Emscripten glue (jslib.js, wasmManager)
  window/     GLFW windowing (native only)
  io/         serialization (placeholder)
  app/        application entry (stub)
extern/       vendored deps (glfw; Vulkan via system SDK)
build_files/  macros.cmake, WASM.cmake, link_wasm.py
tests/        GTest-style native tests (BUILD_WASM=OFF)
typescript/   generated TS bindings (typescript/api is hand-written, not generated)
```

`source/litestl/` is a reusable sub-library with its own CMake / tests / docs —
treat it as a sub-library, not free-form project code.

See [`documentation/projectIndex.md`](documentation/projectIndex.md) for the full
source-tree map; prefer reading it before any wide exploration.

## Testing & debugging

- **Native tests** (Google Test) live under `tests/` and run only when
  `BUILD_WASM=OFF`; `source/litestl/tests/` holds litestl-internal tests. Run
  with `node make.mjs test`.
- **Debug app** — `source/debug/` builds a native scripted harness (`debug_app`)
  that drives mesh + spatial + brush + GL end-to-end from a plain-text script,
  headlessly by default. It's the right tool for reproducing engine bugs and
  A/B-testing brush backends. See
  [`documentation/debugApp.md`](documentation/debugApp.md) and
  [`documentation/debugging.md`](documentation/debugging.md).

## Documentation

| Topic | Doc |
| --- | --- |
| Source-tree map | [`documentation/projectIndex.md`](documentation/projectIndex.md) |
| Mesh data structures | [`documentation/mesh.md`](documentation/mesh.md) |
| Spatial tree | [`documentation/spatial.md`](documentation/spatial.md) |
| Sculpt undo/redo log | [`documentation/meshlog.md`](documentation/meshlog.md) |
| Brushes (runtime) | [`documentation/brush.md`](documentation/brush.md) |
| Brush DSL | [`documentation/brush_dsl.md`](documentation/brush_dsl.md) |
| Brush compiler / verification | [`documentation/brush_compute.md`](documentation/brush_compute.md) |
| Dynamic topology | [`documentation/dynamic-topology.md`](documentation/dynamic-topology.md) |
| Boundary constraints | [`documentation/boundary.md`](documentation/boundary.md) |
| Quad remeshing | [`documentation/quad-remeshing.md`](documentation/quad-remeshing.md) |
| Rendering / GPU object model | [`documentation/rendering.md`](documentation/rendering.md) |
| Debug app + workflow | [`documentation/debugApp.md`](documentation/debugApp.md), [`documentation/debugging.md`](documentation/debugging.md) |

## License

MIT. Copyright (c) 2022 Joseph Eagar. See [`LICENSE.txt`](LICENSE.txt).
