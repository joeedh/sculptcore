# Claude-Code-assisted debugging

A scripted native debug app + gdb bundle that lets Claude reproduce, render,
and diff sculptcore bugs without a human at the keyboard. Native-only;
WASM is unaffected.

## Build

```
node make.mjs configure native
node make.mjs build native
```

Produces `build/native/source/debug/debug_app.exe`.

## Running a script

```
debug_app --script tests/scripts/cube_draw.txt --out build/shots [--headless] [--width 1024 --height 768] [--interactive]
```

- `--headless` (default): renders into an offscreen FBO, no visible window.
- `--no-headless --interactive`: opens a window after the script runs and
  spins until you close it. Useful for poking around manually.

Scripts are line-oriented, `#` for comments, one verb + `key=value` args
per line:

| verb            | args                                              | effect |
|---|---|---|
| `make_cube`     | `subdivs=N size=F sphere=F`                       | replaces the active mesh |
| `build_spatial` | `leaf_limit=N depth_limit=N`                      | (re)builds spatial accel |
| `set_brush`     | `radius=F strength=F invert=0/1`                  | tweaks the brush props |
| `stroke`        | `origin=x,y,z normal=x,y,z`                       | one-step DRAW stroke through `CommandExecutor::execBrush` |
| `stroke_path`   | `p1=x,y,z p2=x,y,z normal=... steps=N`            | sweep N draw steps along a segment |
| `view`          | `preset=front\|top\|side\|persp\|free`            | re-frames the camera on the mesh AABB |
| `screenshot`    | `view=... out=relpath [leaves=0/1]`               | renders headless + writes PNG |
| `dump_state`    | `out=relpath [mesh=1 spatial=1 brush=1]`          | writes JSON snapshot |
| `assert_verts`  | `n=N`                                             | exit 1 on mismatch |
| `assert_aabb`   | `min=x,y,z max=x,y,z eps=F`                       | exit 1 on mismatch |
| `undo` / `redo` | -                                                 | drive `meshlog::MeshLog` |
| `echo`          | `msg=...`                                         | prints to stdout |

Relative `out=` paths join against the `--out` directory.

## The Claude workflow

1. User reports: *brush draw on a 4-subdiv cube duplicates corners near the +Z face.*
2. Claude writes `tests/scripts/repro_dup_corner.txt`:
   ```
   make_cube subdivs=4 size=1.0
   build_spatial leaf_limit=256 depth_limit=8
   set_brush radius=0.25 strength=0.5
   stroke origin=0,0,0.5 normal=0,0,1
   dump_state out=repro.json
   screenshot view=persp out=repro.png
   assert_verts n=56
   ```
3. Runs `debug_app --script tests/scripts/repro_dup_corner.txt --out build/shots`.
4. Reads `build/shots/repro.png` with the `Read` tool (multimodal) and
   `build/shots/repro.json` (vert / face / per-leaf counts) to localize.
5. If a crash: `tools/gdb/run-gdb.sh tests/scripts/repro_dup_corner.txt`.
   The pretty-printers in `tools/gdb/sculptcore.py` make `Mesh*`,
   `SpatialNode*`, `Vector<T>`, `float3`, `AABB`, and `Buffer*` legible
   at a glance; the helpers in `helpers.gdb` add commands:
   - `sc-mesh-summary <Mesh*>` — counts at a glance.
   - `sc-spatial-walk <SpatialTree*>` — one line per node.
   - `sc-break-bad-topo` — breakpoints on `Mesh::kill_*`.
6. Fix → rerun → diff JSON → commit the script into `tests/scripts/` so
   the fix carries a regression case forward.

## Gotchas

- Scripts that only do mesh + assert work do **not** open a GL context.
  `ensureGL()` is lazy; only `screenshot` (and `--interactive`) trigger
  GLFW/GLEW init. Use the headless-only paths for unit-style scripts.
- The engine's GLSL is GLSL-ES style. `opengl/gl_backend.cc` prepends
  `#version 120` and strips `precision` qualifiers so a desktop 2.1
  context accepts the same source. If you add a new shader and see
  `gl_FragColor` undeclared on desktop, that preamble is the place to
  look.
- `glew32s.lib` (static) is linked under `extern/glew`. The vendored
  static archive was built with an older toolchain so you'll see the
  benign `LNK4099` "no pdb" warning; ignore it.

## Layout

- `source/opengl/` — minimal GL backend (`gl_backend`, `gl_overlay`,
  `gl_context`, `gl_screenshot`). Walks `gpu::GPUManager` resources;
  caches `Buffer*` → VBO and `ShaderDef*` → program.
- `source/debug/` — `Scene` (mesh+tree+brush+GPU+camera+window),
  `script` (parser + verb dispatcher), `state_dump` (JSON writer),
  `debug_app.cc` (CLI entry).
- `tools/gdb/` — pretty-printers, helpers, launch wrappers.
- `tests/scripts/` — canned fixtures. `tests/test_debug_script.cc`
  exercises the parser headlessly via `script::run`.
