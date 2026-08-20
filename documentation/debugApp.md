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
  the window stays open and accepts mouse + keyboard input until closed.
  See [Interactive mode](#interactive-mode) below.
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
| `triangulate`   | -                                                 | triangulates the active mesh (dyntopo operators are triangle-only; `make_cube` builds quads) |
| `build_spatial` | `leaf_limit=N depth_limit=N`                      | (re)builds the spatial accelerator on the current mesh. Refused while multires is active — the level trees belong to the stack; pass the same knobs to `multires_init` instead |
| `multires_init` | `levels=N [level=L] [budget=B]`<br>`[leaf_limit=N depth_limit=N gpu_tri_target=N]` | turns the current mesh into a multires cage and attaches level `L` (default: finest, max 7). `mesh`/`tree` become views of that level's slot; the tree knobs override `multiresAutoTune` for every materialized level |
| `multires_level`| `level=L`                                         | writes the active level back into the store, then materializes and attaches level `L` |
| `set_brush`     | `radius=F strength=F spacing=F invert=0/1`        | tweaks the active `brush::Brush` props and re-syncs them through `props::StructProp`; `spacing` is the per-stroke fraction of the brush *diameter* between successive dabs (default 0.25) |
| `set_brush_tool`| `tool=draw\|clay\|inflate\|pinch\|...`            | selects the active sculpt brush |
| `set_texture`   | `script=PATH\|clear` or `image=PATH` or<br>`proc=NAME` or `pattern=NAME\|clear` | binds a grayscale brush texture, sources checked in that priority order: `script=` compiles a `.stex` texture script at runtime (tcc JIT) and binds it as `Brush::texture_program` — it takes precedence over any bitmap in `sampleBrushTex`, does its own point mapping (so `set_coord_space` does not apply to it), and is CPU-only until T5 (the WGSL stroke path refuses and falls back); `image=` decodes to luminance via stb_image; `proc=` bakes an analytic function onto the UV grid; `pattern=` (default `rampx`) uses the simple synthetic test patterns (texel value varies across the surface so a golden can assert displacement tracks UV, e.g. `rampx` grows with `co.x` under the Global space). `script=clear` unbinds the program, `pattern=clear` the bitmap |
| `set_coord_space` | `space=global\|viewplane\|viewrepeat\|`<br>`stroke_curved\|projected repeat=F` | sets the bitmap texture's `Brush.coord_space` mapping and/or `tex_repeat` (either arg alone is fine; ignored by a bound texture script) |
| `dyntopo`       | `enabled=0/1 detail=F [min=F] [grade=F] [flip=0/1]`<br>`[smooth=0/1] [max_splits=N] [mode=both\|subdivide\|collapse]` | configures dynamic-topology remesh applied as a pre-pass to subsequent strokes (`source/dyntopo/`) |
| `bench_dyntopo` | `detail=F radius=F center=x,y,z [grade=F flip=0/1`<br>`smooth=0/1 max_splits=N spatial=0/1 rebuild=0/1]` | A/B one dyntopo dab vs a full tree rebuild; prints splits/flips/rounds/leftover/maxValence/CV + ops/update/total ms. The M7 measurement tool |
| `stroke`        | `origin=x,y,z normal=x,y,z`                       | one-step stroke (current `set_brush_tool`) through `brush::CommandExecutor::execBrush`; runs the dyntopo pre-pass first when `dyntopo enabled=1` |
| `stroke_path`   | `p1=x,y,z p2=x,y,z normal=... steps=N`<br>or `... spacing=F` | sweeps a segment of `DRAW` dabs. With `steps=N` (default 8): N evenly-distributed dabs by parameter `t`. With `spacing=F`: dabs every `radius * spacing` world-space units along the straight segment (a fixed-step sweep, *not* the interactive sampler — see `stroke_screen` for that) |
| `stroke_screen` | `p1=x,y [p2=x,y] [steps=N]`<br>`[method=path\|anchored\|dragdot] [rough=0/1]` | a stroke expressed as a pointer drag: `steps` events (default 8) interpolated `p1`→`p2` in view-local pixels (top-left origin, at the current framebuffer size), fed through `brush::BrushStrokeDriver` — the same sampler the interactive mode and the TS app use, so a scripted stroke lands where a user's drag would. The driver raycasts each event and spaces dabs along a centripetal Catmull-Rom spline, so unlike `stroke_path` nothing here names a world position or a normal. Set the camera first (`view`). Events that miss the surface are dropped; a drag that misses entirely leaves no undo step. CPU executor only, one meshlog step |
| `preview_stroke_path` | `p1=x,y,z p2=x,y,z normal=... steps=N`      | one meshlog step of N dabs along the segment where every dab but the last is wrapped in `beginPreviewDab`/`rollbackPreviewDab` (undone before the next), and only the final dab is committed — exercises the Anchored/Drag Dot live-preview rollback primitive (`meshlog::MeshLog::beginPreviewDab`/`rollbackPreviewDab`) in isolation. CPU executor only |
| `view`          | `preset=front\|top\|side\|persp\|free`            | re-frames the camera on the mesh AABB |
| `screenshot`    | `view=... out=relpath [leaves=0/1]`               | renders headless + writes PNG; `leaves=1` overlays spatial-leaf AABBs |
| `dump_state`    | `out=relpath [mesh=1 spatial=1 brush=1]`          | writes JSON snapshot of selected sections |
| `save_mesh`     | `path=FILE`                                       | serializes the scene mesh (`serial::writeMesh` blob — the LiteMesh `_data` container format) |
| `load_mesh`     | `path=FILE`                                       | replaces the scene mesh with a `serial::readMesh` blob (running format migrations), then `validateAndRepair` and prints the problem count. Rebuild spatial/dyntopo state afterwards (`build_spatial`, …) |
| `assert_verts`  | `n=N`                                             | exits non-zero on mismatch |
| `assert_aabb`   | `min=x,y,z max=x,y,z eps=F`                       | exits non-zero on mismatch |
| `save_pos`      | `id=NAME`                                         | snapshots every live vert's `(index, co)` under `id` (default `default`) for a later `assert_pos`. Mesh indices are persistent ids (IDMap disabled), so a vert restored by undo lands back at the same index |
| `assert_pos`    | `id=NAME eps=F soft=0/1`                          | diffs live verts against the `id` snapshot; reports `dead`/`moved`/`worst`. Exits non-zero on any divergence unless `soft=1` (then it just prints). The undo-fidelity check (see example below) |
| `set_weights`   | `group=N [value=F] [name=weights]`                | writes a one-influence run into the vertex `AttrType::WEIGHTS` layer `name` (created on demand). Default weight is a z-gradient normalized over the mesh AABB — a constant survives any interpolator, so it would not catch a merge handler that lost a run across a dyntopo split; `value=F` forces one anyway |
| `save_weights`  | `id=NAME [name=weights]`                          | snapshots every live vert's whole weight run under `id` (default `default`). Runs, not slot indices: the pool interns by value, so an index is not comparable across a sweep |
| `assert_weights`| `id=NAME [name=weights] eps=F soft=0/1`           | diffs live verts against the `id` snapshot; reports `dead`/`reshaped`/`changed`/`worst`. Same exit contract as `assert_pos`. `save_weights … stroke … undo … assert_weights` is the weights half of the undo-fidelity check |
| `undo` / `redo` | -                                                 | drives `meshlog::MeshLog` against the active mesh + tree |
| `grid_stroke`   | `origin=x,y,z normal=x,y,z [dabs=1] [step=x,y,z]`<br>`[level=L] [backend=cpp\|wgpu\|wgsl\|gpu]` | one grids-native stroke through `brush::GridBrushExecutor` at a multires level (default: active; requires `multires_init`), then mirrors the touched region into the resident slot mesh. Refuses tools `supportsBrush` declines; a GPU backend additionally requires the tool's `@gpu` kernel (`GridGpuStrokeSession::kernelFor`) and shares the same grid undo log. Prints `moved= undo_bytes=` |
| `grid_undo` / `grid_redo` | -                                       | seeks the `GridStrokeLog` one step, then mirrors the *whole* level back into the slot mesh — seek granularity is leaf blocks, so the full sync is what keeps the debug mirror correct |
| `grid_bench`    | `[subdivs=26] [levels=4] [strokes=19] [dabs=57]`<br>`[radius=0.1] [strength=0.5] [leaf=512] [backend=...]` | self-contained grids-native benchmark: builds its own cube cage + `Multires` (the scene is untouched), marches draw strokes across the top face, prints per-phase stats + the G3 perf gates |
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

Example — undo-fidelity check (every vert must return to its pre-stroke
position after undo, the regression workflow behind the dyntopo-undo fixes):

```
make_cube subdivs=12 size=0.5
build_spatial leaf_limit=256 depth_limit=8
dyntopo enabled=1 detail=0.04 smooth=1
save_pos id=base
set_brush radius=0.25 strength=0.5
stroke origin=0,0,0.5 normal=0,0,1
undo
assert_pos id=base
```

The same shape covers vertex-group weights, which undo has to restore through
the meshlog's raw-byte rows *and* the DeformPool's refcounts. `dump_state`'s
`weight_attrs` block is the matching golden signature (per-layer `entries`,
`group_sum`, `sum`, `sqsum`, and the pool's live `slots`):

```
make_cube subdivs=8 size=0.5
triangulate
build_spatial leaf_limit=256 depth_limit=8
set_weights group=3
save_weights id=base
dyntopo enabled=1 detail=0.04
set_brush radius=0.25 strength=0.5
stroke origin=0,0,0.5 normal=0,0,1
undo
assert_weights id=base
```

## Interactive mode

`--interactive` presents the scene through a Vulkan swapchain attached to
the GLFW window. The script still drives initial scene setup (mesh,
spatial tree, brush params, view) — once it finishes the user takes over
with mouse + keyboard:

| input                          | action |
|---|---|
| LMB drag                       | brush stroke, sampled by `brush::BrushStrokeDriver` (the same sampler the TS app uses): pointer events are fitted to a centripetal Catmull-Rom spline and walked at `2 * radius * brush.spacing` world-space units per dab, with the driver raycasting each event itself. A press that misses the surface opens no stroke |
| Shift+LMB drag                 | brush stroke with the smooth brush (`currentTool` is overridden for the drag) |
| Ctrl+LMB drag                  | pan |
| Alt+LMB drag, RMB drag, MMB drag | orbit around `Camera::target` |
| scroll                         | zoom (eye→target distance, clamped) |
| Ctrl+Z                         | undo last brush step (refused mid-stroke) |
| Ctrl+Y or Ctrl+Shift+Z         | redo |
| window resize                  | swapchain is recreated automatically |

The brush dabs share one `meshlog::MeshLog::beginStep` / `endStep` pair per
stroke, so a single undo reverts the whole drag.

A Dear ImGui panel (`source/debug/ui.cc`) gives live controls for brush
params and the dynamic-topology settings (enable, goal edge length, grade,
geometric flips, tangential smoothing, split budget, mode, and a
`triangulate` button) — so a dyntopo stroke can be tuned interactively, not
just from the `--script`.

Internally, `Scene` keeps two `vulkan::VulkanBackend` instances:
`backend` (offscreen target, used by `screenshot`) and `backendWindow`
(swapchain render pass). The interactive loop in `debug_app.cc` attaches
an `InputDispatcher` to the GLFW window and feeds events to an
`InteractiveController`; the controller owns the active stroke, casts
rays via `spatial::SpatialTree::castRay`, and routes Ctrl+Z/Y into
`meshLog`.

## Architecture

```
source/debug/
  debug_app.cc       CLI entry, arg parsing, headless/interactive loop.
  scene.{h,cc}       Owns Mesh + SpatialTree + Brush + MeshLog + GPUManager
                     + camera + window + Vulkan offscreen target + swapchain
                     + overlay. All optional pieces are lazily created — a
                     script that never asks for screenshots never opens a
                     Vulkan device.
  script.{h,cc}      Lexer + verb dispatcher. Pure C++; no STL containers
                     in hot paths (uses litestl::util::Vector).
  state_dump.{h,cc}  JSON writer for mesh / spatial / brush sections.
  camera.h           View-preset helpers.
  input.{h,cc}       GLFW callback trampolines + InputDispatcher
                     (InputEvent / InputHandler).
  interactive.{h,cc} InteractiveController — translates mouse/key events
                     into brush strokes, camera nav, undo/redo.
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
- Relative paths in scripts resolve against `--out`, not against the
  script file's directory — keep that in mind when sharing scripts
  between local runs and CI runs.
