# `debug_app` — native scripted harness

A small native-only CLI that drives the full sculptcore engine from a
plain-text script: load a mesh, build a spatial accelerator, run brush
strokes, render screenshots, dump JSON state, assert on geometry. It is
the canonical way for Claude (and humans) to exercise sculptcore end-to-end
without the WASM / JS / browser layers. WASM builds do not include it.

Sources live under [`source/debug/`](../source/debug/), with sample
scripts in [`tests/scripts/`](../tests/scripts/).

## Build

```
node make.mjs configure native
node make.mjs build native
```

Produces `build/native/source/debug/debug_app.exe` (or `debug_app` on
non-Windows). The C++ side also exposes `script::run(scene, src, outDir)`
and `script::runFile(...)` so tests can drive the same script engine
in-process without spawning the binary — see
[`tests/test_debug_script.cc`](../tests/test_debug_script.cc).

## CLI

```
debug_app --script PATH [--out DIR] [--headless] [--width N] [--height N]
          [--no-headless] [--interactive]
```

- `--script PATH` — required; line-oriented script file.
- `--out DIR` — base directory for relative `screenshot` / `dump_state`
  paths. Default `.`.
- `--headless` (default) — renders into an offscreen FBO; no visible
  window. GL is initialized lazily on the first `screenshot` verb.
- `--no-headless` — opens a window for rendering.
- `--interactive` — implies `--no-headless`; after the script finishes
  the window stays open until the user closes it. Useful for visual
  inspection of an intermediate state.
- `--width N --height N` — framebuffer / window size (default 1024×768).

Exit code is `0` on success, `1` on script error (with line number and
verb error on stderr), `2` on bad CLI args.

## Scripting language

Plain text. One verb per line, `#` introduces a comment, blank lines are
skipped. Args are space-separated `key=value` pairs. Values containing
spaces aren't supported — vectors use `=x,y,z`.

| verb            | args                                              | effect |
|---|---|---|
| `make_cube`     | `subdivs=N size=F sphere=F`                       | replaces the active mesh with a subdivided cube; `sphere` ∈ [0,1] morphs toward a sphere |
| `build_spatial` | `leaf_limit=N depth_limit=N`                      | (re)builds the spatial accelerator on the current mesh |
| `set_brush`     | `radius=F strength=F invert=0/1`                  | tweaks the active `brush::Brush` props and re-syncs them through `props::StructProp` |
| `stroke`        | `origin=x,y,z normal=x,y,z`                       | one-step `DRAW` stroke through `brush::CommandExecutor::execBrush` |
| `stroke_path`   | `p1=x,y,z p2=x,y,z normal=... steps=N`            | sweeps N draw steps along the segment, sharing one `beginStep/endStep` |
| `view`          | `preset=front\|top\|side\|persp\|free`            | re-frames the camera on the mesh AABB |
| `screenshot`    | `view=... out=relpath [leaves=0/1]`               | renders headless + writes PNG; `leaves=1` overlays spatial-leaf AABBs |
| `dump_state`    | `out=relpath [mesh=1 spatial=1 brush=1]`          | writes JSON snapshot of selected sections |
| `assert_verts`  | `n=N`                                             | exits non-zero on mismatch |
| `assert_aabb`   | `min=x,y,z max=x,y,z eps=F`                       | exits non-zero on mismatch |
| `undo` / `redo` | -                                                 | drives `meshlog::MeshLog` against the active mesh + tree |
| `checkpoint`    | -                                                 | no-op marker for readability in long scripts |
| `echo`          | `msg=...`                                         | prints `[script] msg` to stdout |

Relative `out=` paths join against the `--out` directory; absolute paths
(`/foo`, `C:\foo`) bypass it.

Example — single brush stroke on a 12-subdiv cube, JSON + PNG:

```
make_cube subdivs=12 size=0.5
build_spatial leaf_limit=256 depth_limit=8
set_brush radius=0.25 strength=0.5
stroke origin=0,0,0.5 normal=0,0,1
dump_state out=cube_draw.json
screenshot view=persp out=cube_draw.png
```

## Architecture

```
source/debug/
  debug_app.cc      CLI entry, arg parsing, headless/interactive loop.
  scene.{h,cc}      Owns Mesh + SpatialTree + Brush + MeshLog + GPUManager
                    + camera + window + Vulkan offscreen target + overlay.
                    All optional pieces are lazily created — a script that
                    never asks for screenshots never opens a Vulkan device.
  script.{h,cc}     Lexer + verb dispatcher. Pure C++; no STL containers
                    in hot paths (uses litestl::util::Vector).
  state_dump.{h,cc} JSON writer for mesh / spatial / brush sections.
  camera.h          View-preset helpers.
```

Static lib `debug_core` exposes `Scene` / `script::run` to tests;
executable `debug_app` is just a thin wrapper. Vulkan setup lives behind
`Scene::ensureGPU()` so headless-only scripts skip the GLFW/Vulkan path
entirely.

## Adding a verb

1. Implement the work in the appropriate engine module (`source/brush/`,
   `source/mesh/`, …). The verb dispatcher should stay thin.
2. Add a branch in `execVerb()` in
   [`source/debug/script.cc`](../source/debug/script.cc):
   ```cpp
   if (verb == "my_verb") {
     int n = getInt(args, "n", 4);
     // ... call into engine ...
     return true;
   }
   ```
   Use the existing `getInt` / `getFloat` / `getBool` / `getArg` /
   `parseFloat3` helpers — don't roll your own parsing.
3. If the verb produces a file artifact, accept `out=relpath` and pass
   through `joinPath(out_dir, rel)`.
4. Document it in the verb table above and (when it makes sense) write a
   sample script under `tests/scripts/`.
5. Add a coverage case to
   [`tests/test_debug_script.cc`](../tests/test_debug_script.cc) — the
   in-process runner gives fast, GPU-free assertions on the verb's effect.

When a new verb needs scene state that doesn't fit on `Scene` yet, extend
`Scene` rather than adding global state — the script engine is expected
to be reentrant per-`Scene`.

## Asserts and golden comparison

Today's `assert_verts` / `assert_aabb` cover topology / extent. For
pixel-level / state-level golden checks (e.g. brush-DSL backend
A/B testing) the intended pattern is:

- Emit `dump_state out=foo.json` and diff against a reference JSON.
- Emit `screenshot out=foo.png` and diff against a reference PNG via an
  external image-diff (the brush DSL plan adds an `assert_png` /
  `assert_dump` verb pair when those land — see
  [`brush_compute_dsl.md`](brush_compute_dsl.md)).

State dumps are deterministic given a deterministic engine; PNG diffing
needs an epsilon because GL rasterization isn't bit-exact across drivers.

## Use in tests

[`tests/test_debug_script.cc`](../tests/test_debug_script.cc) exercises
the parser + verb dispatch in-process (no subprocess, no GL). Pattern:

```cpp
Scene scene(64, 64, /*headless=*/true);
auto r = script::run(scene, "make_cube subdivs=4\n", ".");
test_assert(r.ok);
```

This is the right shape for regression tests that don't need rendering.
When a regression needs visuals, commit a script under `tests/scripts/`
and run it through the binary in CI.

## Gotchas

- `ensureGPU()` is lazy. Scripts that touch only mesh / spatial / brush
  state and never `screenshot` won't pull in GLFW or the Vulkan device.
- Engine shaders live as WGSL under `source/spatial/shaders/*.wgsl` and
  are compiled to SPIR-V at configure/build time via `naga` (see
  `tools/wgsl-to-spirv.mjs` and `nagaVersion.txt`). Install the toolchain
  once with `node make.mjs install-tools`.
- Vulkan headers and the loader come from the system Vulkan SDK
  (`find_package(Vulkan REQUIRED)`); set `VULKAN_SDK` if CMake can't find
  it.
- Interactive (`--interactive`) currently renders into the offscreen
  target only — a swapchain-presented window is a follow-up. The GLFW
  window appears but stays blank.
- Relative paths in scripts resolve against `--out`, not against the
  script file's directory — keep that in mind when sharing scripts
  between local runs and CI runs.
