# Sculptcore

A C++20 sculpting / mesh engine that builds natively and to WebAssembly via
Emscripten. CMake + Ninja, driven by a small Node dispatcher (`make.mjs`).

See `documentation/projectIndex.md` for the full source-tree map — prefer
reading it before doing wide exploration.

## Commit logs
When suggesting commit logs, do not include additions to the CSpell 
dictionary.

Keep messages short: a one-line summary of intent, then a few bullets
calling out the notable changes (with brief sub-bullets for non-obvious
details). Don't write per-file paragraphs or restate diffs the reader
can see — mention specific files only when the file *is* the point
(e.g. a new module). Skip routine churn (lockfiles, formatting,
generated files) unless it's load-bearing.

## Build

Use the Node dispatcher rather than invoking cmake/emcmake directly:

```
node make.mjs install-emsdk          # one-time; clones emsdk submodule, installs pinned emsdk + cmake + ninja
node make.mjs configure [wasm|native]  # default wasm
node make.mjs build     [wasm|native]
node make.mjs test      [wasm|native]  # runs ctest in the build dir
node make.mjs clean     [wasm|native]  # ninja clean
```

Notes:
- All commands take an optional `target` positional (`wasm` default, or `native`).
- Build dirs: WASM → `build/`, native → `build/native/`.
- WASM configure runs `emcmake cmake .. -G Ninja -DBUILD_WASM=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`;
  native configure runs plain `cmake ../.. -G Ninja`.
- Every command runs under `node configureEnv.mjs` (with `--emsdk` for WASM) to set up the
  emsdk/PATH environment — don't invoke cmake/ninja/ctest directly.
- `emsdkVersion.txt` pins the Emscripten version. `install-emsdk` also installs pinned
  `cmake-4.2.0-rc3-64bit` and `ninja-git-release-64bit` via emsdk and activates them
  `--permanent`, then appends `cmake` to `emsdk/.gitignore` (upstream omits it).
- The WASM `build` step deletes `build/sculptcore.{js,wasm}` before linking because
  emcc can silently succeed on compile errors otherwise — don't "optimize" that away.
- Native (non-WASM) builds enable `tests/` and the `sculptcore` executable still links,
  but the primary target is WASM.
- `node serv.mjs` serves `index.html` + the WASM module for browser testing.

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
  mesh/             mesh data structures + attributes + C API
  brush/            sculpt brushes and commands
  spatial/          spatial acceleration (BVH-style nodes)
  props/            property / reflection system (runtime-side)
  gpu/              GPU abstraction (frontend + opengl/ backend)
  io/               serialization
  window/           GLFW windowing (native only)
  wasm/jslib.js     Emscripten JS library glue
  app/              application entry (stub)
extern/             vendored: eigen_dist, glew, glfw
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
touch with care and prefer additive changes.

## Tests

Native tests (Google Test) live under `tests/` and run only when
`BUILD_WASM=OFF`. There is also a WASM harness (`wasmTest.mjs`).
`source/litestl/tests/` holds litestl-internal tests.

When adding a test, match the existing style (`test_<thing>.cc`) and
wire it through the appropriate `CMakeLists.txt`.

## Conventions

- Namespaces: `litestl::util`, `litestl::binding`, etc.
- Prefer editing existing files over adding new ones; the module
  granularity is already fairly fine.
- Don't add backward-compat shims when renaming internals — this is a
  single-repo project.
- Keep comments minimal; explain *why* only when non-obvious.
- Path handling: use `litestl::path` utilities rather than ad-hoc string
  manipulation or raw `std::filesystem` in engine code.
