# Claude-Code-assisted debugging

A scripted native debug app + gdb bundle that lets Claude reproduce, render,
and diff sculptcore bugs without a human at the keyboard. Native-only;
WASM is unaffected.

The debug app itself — CLI flags, the full verb table, scripting language,
and how to add new verbs — is documented separately in
[`debugApp.md`](debugApp.md). This page covers the debugging *workflow*
that uses it.

## Build & run (quick reference)

```
node make.mjs configure native
node make.mjs build native
debug_app --script tests/scripts/cube_draw.txt --out build/shots
```

See [`debugApp.md`](debugApp.md) for the complete CLI and verb reference.

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

- Scripts that only do mesh + assert work do **not** open a Vulkan
  device. `ensureGPU()` is lazy; only `screenshot` (and `--interactive`)
  trigger GLFW + Vulkan init. Use the headless-only paths for unit-style
  scripts.
- Engine shaders are authored in WGSL under
  `source/spatial/shaders/*.wgsl` and compiled to SPIR-V at build time
  via `naga` (`tools/wgsl-to-spirv.mjs`, pinned in `nagaVersion.txt`).
  Install the toolchain once with `node make.mjs install-tools`.
- Vulkan headers + loader come from the system Vulkan SDK. Set
  `VULKAN_SDK` if CMake's `find_package(Vulkan REQUIRED)` can't locate
  it.

## Layout

- `source/vulkan/` — Vulkan backend (`vk_context`, `vk_backend`,
  `vk_overlay`, `vk_screenshot`). Walks `gpu::GPUManager` resources;
  caches `Buffer*` → VkBuffer and `ShaderDef*` → graphics pipeline.
- `source/debug/` — `Scene` (mesh+tree+brush+GPU+camera+window),
  `script` (parser + verb dispatcher), `state_dump` (JSON writer),
  `debug_app.cc` (CLI entry).
- `tools/gdb/` — pretty-printers, helpers, launch wrappers.
- `tests/scripts/` — canned fixtures. `tests/test_debug_script.cc`
  exercises the parser headlessly via `script::run`.
