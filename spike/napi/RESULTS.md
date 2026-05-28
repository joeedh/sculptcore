# Spike A.5 results — clang ↔ Electron N-API

Milestone 1 of [documentation/plans/native-electron.md](../../../documentation/plans/native-electron.md).
**Question:** does the repo's Windows *clang* toolchain produce a `.node` that
links against Electron's MSVC-built `node.lib` and loads/runs in the Electron
process? **Answer: yes** — with one toolchain flag fix and one library caveat.

## How it was built

```
node sculptcore/configureEnv.mjs "cd sculptcore/spike/napi && \
  node node_modules/cmake-js/bin/cmake-js rebuild -O build -G Ninja \
  --CDCMAKE_TOOLCHAIN_FILE=<repo>/sculptcore/build_files/native-clang.cmake \
  -r electron -v 41.1.1 -a x64"
```

- `configureEnv.mjs` sets up the MSVC (vcvars64) env; clang on Windows targets
  the `x86_64-pc-windows-msvc` ABI, so it consumes MSVC headers/libs and is
  ABI-compatible with Electron's MSVC-built `node.lib`.
- Toolchain: `build_files/native-clang.cmake` (clang/clang++), Ninja generator.
- cmake-js auto-downloaded the Electron 41.1.1 headers + `node.lib` to
  `~/.cmake-js/electron-x64/v41.1.1/`.
- Compiler used (reported by the addon at runtime): **clang 20.1.8** (VS-bundled
  LLVM), not MSVC `cl`.

## Findings

### 1. The link needs the delay-load flag in clang syntax (FIXED)

cmake-js sets `CMAKE_SHARED_LINKER_FLAGS=/DELAYLOAD:NODE.EXE` — `link.exe`
syntax. Our linker *driver* is `clang++` (gcc-style), which reads a bare
`/DELAYLOAD:NODE.EXE` as an input filename → `no such file or directory`. Fix in
`CMakeLists.txt`: strip cmake-js's form and re-add via CMake's `LINKER:`
passthrough (`-Xlinker /DELAYLOAD:node.exe`) + `delayimp.lib`. **This is the
reusable takeaway for Workstream A's real CMake target.**

### 2. The N-API **C ABI is correct**; node-addon-api's C++ `CallbackInfo` is not (under this clang)

Runtime results loading the clang-built `.node` in Electron:

| call | result | verdict |
|---|---|---|
| `hello()` → string | `"hello from native clang N-API addon"` | ✅ |
| `compiler()` | `"clang 20.1.8"` | ✅ proves clang built it |
| `add(2,40)` via node-addon-api | `a=2, b=40, sum=42, status=ok, isNumber=true`, **but `info.Length()` = `6e-310` garbage** | ⚠️ |
| `rawAdd(2,40)` via raw `napi_*` | `rawArgc=2, rawSum=42` | ✅ |

So `napi_get_cb_info` / `napi_get_value_double` / `napi_create_*` all work; only
node-addon-api's inline `Napi::CallbackInfo::Length()` returns garbage. It
reproduces identically at `-O3` and `-O0`, so it is **not** an optimization
miscompile — it's a genuine node-addon-api-header ↔ clang-on-Windows
incompatibility.

## Recommendation for Workstream A/B

- **Build the reflection runtime on the raw C N-API (`node_api.h`), not
  node-addon-api's C++ convenience wrappers.** This is already the style the
  plan describes (`napi_create_external_arraybuffer`, `napi_create_string_utf8`,
  `napi_get_value_*`, method thunks). Dropping `node-addon-api` removes a
  dependency *and* the `CallbackInfo` risk; keep only `cmake-js` for the build.
- Carry the `/DELAYLOAD` clang-syntax fix into the real `sculptcore_node` CMake
  target.
- Toolchain/version pinning that worked: clang 20.1.8, cmake 4.2.3, ninja
  1.12.1, Electron 41.1.1, N-API level 8.

## Reproduce

```
npm install --prefix sculptcore/spike/napi          # cmake-js + node-addon-api
# build (see command above), then:
<electron-exe> sculptcore/spike/napi/electron-main.js --no-sandbox
cat sculptcore/spike/napi/spike-result.json
```
