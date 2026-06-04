# Sculptcore — Project Index

A C++20 sculpting/mesh engine that builds natively and to WebAssembly via Emscripten. Cross-compiled with CMake + Ninja; a Node-based dispatcher (`make.mjs`) drives configure/build.

## Top-level layout

| Path | Purpose |
|---|---|
| `CMakeLists.txt` / `CMakePresets.json` | Root CMake config. Builds `sculptcore` executable; links `util`, `mesh`, `platform`. WASM exports are collected via the `WASM_SYMBOLS` global property. |
| `make.mjs` | Node build dispatcher (replaces prior emscripten wrappers). Commands: `configure`, `build`, `test`, `clean`, `install-emsdk`; each build command takes `[wasm\|native]` (default `wasm`). WASM builds in `build/`, native in `build/native/`. |
| `configureEnv.mjs` | Environment bootstrap for emsdk / pinned toolchain; wraps every cmake/ninja/ctest invocation from `make.mjs` (with `--emsdk` for WASM targets). |
| `serv.mjs` | Dev HTTP server for the WASM/browser frontend. |
| `index.html` | Browser entry that loads the WASM module. |
| `emsdk/` | Emscripten SDK (git-cloned + pinned by `make.mjs install-emsdk`, gitignored — not a submodule). Version pinned in `emsdkVersion.txt`. |
| `build_files/` | `macros.cmake`, `WASM.cmake`, `link_wasm.py`, `emsdk_env.py`. |
| `extern/` | Vendored deps: `eigen_dist/`, `glfw/`. Vulkan headers + loader come from the system Vulkan SDK (`find_package(Vulkan)`). |
| `assets/` | Runtime assets. |
| `tests/` | GTest-style unit tests (native builds only). |
| `documentation/` | Project docs (this file). |

## Source tree (`source/`)

### `source/litestl/` — foundational library (self-contained, has own CMake/tests)

| Module | Notes |
|---|---|
| `util/` | Core containers and primitives: `vector`, `map`, `set`, `ordered_set`, `span`, `array`, `boolvector`, `binaryHeap`, `atomicLinkedList`, `function`, `callback_list`, `hash`, `index_range`, `rand`, `string`, `task` (job system), `memory`/`alloc`, `concepts`, `type_tags`, `time`, `wasm.h`. |
| `math/` | `vector`, `matrix`, `quat`, `color`, `geom`, `bspline`, `mix`, `lut`. |
| `platform/` | Platform abstraction: `win32.cc`, `linux.cc`, `common.cc`, `cpu.h`, `time.h`, `export.h`. |
| `path/` | Filesystem path utilities (`path.cc/.h`). |
| `binding/` | Reflection/binding system — `binding_base`, `binding_struct`, `binding_types`, `binding_utils`, `manager`. |
| `binding/generators/` | Codegen; currently a TypeScript generator (`typescript.cc/.h`). |
| `tests/` | litestl-local tests. |

### `source/mesh/` — mesh data structures

Core: `mesh.cc/.h`, `mesh_base.h`, `mesh_types.cc/.h`, `mesh_shapes.cc/.h`, `mesh_proxy.h`, `mesh_iter.h`, `mesh_enums.h`.
Attributes: `attribute.cc/.h`, `attribute_base.h`, `attribute_bool.h`, `attribute_builtin.h`, `attribute_enums.h`, `elem_data.h`.
ID map: `idmap.cc/.h`.
Bindings: `bindings.cc/.h` — module-level `registerBindings(BindingManager&)`.
Utils: `utils/triangulate.h`, `utils/delaunay.h`, `utils/edge_collapse.h`.
GPU bridge: `gpu/mesh_drawbatch.cc/.h` — builds a `sculptcore::gpu::DrawBatch` from a mesh.
C API: `c-api/mesh_c_api.cc/.h` — external surface for WASM/JS.

See `documentation/mesh.md` for a detailed overview.

### `source/meshlog/` — sculpt undo/redo log

`meshlog.h` (umbrella), `meshlog_base.h` (`MeshLog`, `LogEntry`, `LogChunk`, `LogChunkSimple`, `LogChunkTopo`, `LogElem`, `detail::ChunkElemData`, `detail::ChunkElemRow`), `bindings.cc/.h`. Two chunk types: `LogChunkSimple` stores per-spatial-node attribute swaps for plain position sculpting; `LogChunkTopo` stores merged per-element create/change/kill records driven by `mesh::MeshCallbacks` for full topological undo. Integrated with `brush::CommandExecutor` (`meshLog` member).

See `documentation/meshlog.md` for a detailed overview.

### `source/brush/` — sculpt brushes

Core: `brush.cc/.h` (Brush state + props), `brush_command.cc/.h` (`CommandCtxBase`, `CommandCtx<TYPES>`, falloff), `brush_executor.cc/.h` (`CommandExecutor`: builds + dispatches commands, owns `MeshLog` pointer), `brush_iterators.h` (`BasicVertexIter`, `PtrHelper`), `brush_concepts.h` (C++20 concepts pinning the command ABI).
Brushes: `brushes/types.h` (`SculptBrushes` enum), `brushes/all.h`, `brushes/draw.h`.
Bindings: `bindings.cc/.h`.
Misc: `props.h` (brush-property templates), `exec.h` / `test.h` (reserved).

See `documentation/brush.md` for a detailed overview.

### `source/spatial/` — spatial acceleration

`spatial.cc/.h`, `spatial_base.h`, `node.cc/.h`, `spatial_attrs.h`, `spatial_enums.h`, `spatial_gpu.cc`. Bindings: `bindings.cc/.h`. C API: `c-api/spatial_c_api.cc` (build/free `SpatialTree`, `getSpatialShaders`). GPU shaders: `shaders/`. Incremental dyntopo currency (M7.6) lives here: `add_face_at` (O(1) anchor placement), `applyDeferredRebalance`, `applyDeferredMerge`/`merge_node`, `free_node`.

### `source/dyntopo/` — dynamic-topology remesh

`dyntopo.h` (header-only). `applyBrushDab(...)` runs the per-dab remesh as independent-set rounds of split/collapse/flip/smooth with a graded target and a per-dab split budget. Spatial/brush/meshlog-free — the caller threads `MeshCallbacks`. Design + plan: `documentation/dynamic-topology.md`, `documentation/plans/dyntopo-m7-cascade.md`.

### `source/props/` — property/reflection system

`props.cc/.h`, `prop_base.h`, `prop_struct.cc/.h`, `prop_curve.h`, `curve_cache.cc`, `prop_coerce.h`, `prop_dynamics.h`, `prop_enums.h`, `prop_types.h`, `prop_inherit.cc`. Bindings: `bindings.cc/.h`.

### `source/gpu/` — GPU abstraction

Frontend: `batch.h`, `command.h`, `pipeline.h`, `shader.cc/.h`, `texture.h`, `vbo.cc/.h`, `types.h`, `standard_attrs.h`, `manager.cc/.h`, `uniform_link.cc/.h`. Bindings: `bindings.h`.
Native backend: `source/vulkan/` — `vk_context`, `vk_backend`, `vk_overlay`, `vk_screenshot`. WGSL shaders under `source/spatial/shaders/*.wgsl` are compiled to SPIR-V at build time via `naga` (`tools/wgsl-to-spirv.mjs`).

See `documentation/rendering.md` for the batch/command object model and the uniform-block link pass.

### `source/core/` — aggregate bindings

`bindings.cc` — `extern "C" initBindings()` invokes every module's `registerBindings()` and registers the primitive `util::Vector<T>` instantiations exposed to JS. The single WASM-exported entry point for reflection setup.

### `source/io/` — serialization

Currently empty (placeholder).

### `source/window/` — windowing

`window.cc/.h` (GLFW-based, native only).

### `source/wasm/` — WASM glue

`jslib.js` — Emscripten JS library layer. `wasmManager.cc/.h` — C++-side WASM manager singleton (`sculptcore::wasm::manager`).

### `source/app/` — application entry

`app.cc`, `app.h` (currently stub), `CMakeLists.txt`.

## Tests (`tests/`)

GTest-style C++ tests: `test_binding`, `test_brush`, `test_delaunay`, `test_edge_collapse`, `test_mesh`, `test_props`. Helpers: `mesh_dump.h`, `test_util.h`. TS-side: `wasmTest.ts` (browser harness), `testViewer3D/`. litestl-internal tests live under `source/litestl/tests/`.

## Build targets

- Native: standard CMake + Ninja. Tests enabled when `BUILD_WASM` is OFF.
- WASM: `BUILD_WASM=ON`; exported symbols aggregated via CMake `WASM_SYMBOLS` global property; linked with `-sMODULARIZE=1 --bind`.

## Entry points

- Browser: `index.html` → loads `build/sculptcore.js` (Emscripten modularized).
- Native executable: `sculptcore` target (root `CMakeLists.txt`).
- Dev loop (WASM): `node make.mjs configure && node make.mjs build`, then `node serv.mjs`.
- Native loop: `node make.mjs configure native && node make.mjs build native && node make.mjs test native`.
- One-time setup: `node make.mjs install-emsdk` (git-clones `emsdk` at its pinned commit, installs pinned emsdk + cmake + ninja).
