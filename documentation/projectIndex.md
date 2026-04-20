# Sculptcore — Project Index

A C++20 sculpting/mesh engine that builds natively and to WebAssembly via Emscripten. Cross-compiled with CMake + Ninja; a Node-based dispatcher (`make.mjs`) drives configure/build.

## Top-level layout

| Path | Purpose |
|---|---|
| `CMakeLists.txt` / `CMakePresets.json` | Root CMake config. Builds `sculptcore` executable; links `util`, `mesh`, `platform`. WASM exports are collected via the `WASM_SYMBOLS` global property. |
| `make.mjs` | Node build dispatcher (replaces prior emscripten wrappers). Commands: `configure`, `build`, `clean`. |
| `configureEnv.mjs` | Environment bootstrap for emsdk. |
| `serv.mjs` | Dev HTTP server for the WASM/browser frontend. |
| `index.html` | Browser entry that loads the WASM module. |
| `emsdk/` | Emscripten SDK (git submodule). Version pinned in `emsdkVersion.txt`. |
| `build_files/` | `macros.cmake`, `WASM.cmake`, `link_wasm.py`, `emsdk_env.py`. |
| `extern/` | Vendored deps: `eigen_dist/`, `glew/`, `glfw/`. |
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
C API: `c-api/mesh_c_api.cc/.h` — external surface for WASM/JS.

### `source/brush/` — sculpt brushes

`brush.cc/.h`, `brush_iter.h`, `command.cc/.h`, `exec.h`, `props.h`, `test.h`.

### `source/spatial/` — spatial acceleration

`spatial.cc/.h`, `node.cc/.h`, `spatial_attrs.h`, `spatial_enums.h`, `spatial_gpu.cc`.

### `source/props/` — property/reflection system

`props.cc/.h`, `prop_struct.cc/.h`, `prop_curve.h`, `curve_cache.cc`, `prop_coerce.h`, `prop_dynamics.h`, `prop_enums.h`, `prop_types.h`, `prop_inherit.cc`.

### `source/gpu/` — GPU abstraction

Frontend: `batch.h`, `command.h`, `pipeline.h`, `shader.cc/.h`, `texture.h`, `vbo.cc/.h`, `types.h`, `standard_attrs.h`, `opengl.h`.
Backend: `opengl/` — `shader.h`, `texture.h`, `pipeline.h`, `vbo.h`, `command.h`.

### `source/io/` — serialization

`serial.cc/.h`.

### `source/window/` — windowing

`window.cc/.h` (GLFW-based).

### `source/wasm/` — WASM glue

`jslib.js` — Emscripten JS library layer.

### `source/app/` — application entry

`app.cc`, `app.h` (currently stub), `CMakeLists.txt`.

## Tests (`tests/`)

`test_binding`, `test_boolvector`, `test_brush`, `test_function`, `test_map`, `test_math_vec`, `test_matrix`, `test_mesh`, `test_props`, `test_set`, `test_shared_ptr`, `test_task`, `test_vector`. WASM harness in `wasmTest.mjs`.

## Build targets

- Native: standard CMake + Ninja. Tests enabled when `BUILD_WASM` is OFF.
- WASM: `BUILD_WASM=ON`; exported symbols aggregated via CMake `WASM_SYMBOLS` global property; linked with `-sMODULARIZE=1 --bind`.

## Entry points

- Browser: `index.html` → loads `build/sculptcore.js` (Emscripten modularized).
- Native executable: `sculptcore` target (root `CMakeLists.txt`).
- Dev loop: `node make.mjs configure && node make.mjs build`, then `node serv.mjs`.
