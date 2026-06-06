# Add `viewNo` / `viewDist` to the sbrush + brush-ctx system

## Context

Brush kernels (`.sbrush`) can already reference engine-level per-dab state such
as `surfaceNo`, `surfacePos`, and `renderMatrix` — these are *builtin* `ctx`
fields that live on `CommandCtxBase` and lower to the bare `ctx.X` spelling on
every backend. There is currently no way for a kernel to know how the **camera**
relates to the dab. This change adds two new builtin ctx values so view-aware
brushes become possible:

- **`viewNo`** (`float3`) — unit direction from the eye **toward** the brush
  center (points *into* the surface).
- **`viewDist`** (`float`) — length of that eye→surface vector (distance from the
  camera to the brush center).

Both are per-dab constants, modeled exactly on the existing `renderMatrix`
builtin: **threaded in from the caller** (debug `Scene` → `exec.ctx.*`), not
derived inside the executor. Wired for **full CPU + GPU parity** so a kernel that
declares them compiles and runs identically on the C++ executor and every GPU
backend.

The canonical recipe is [`addingSBrushUniforms.md`](../addingSBrushUniforms.md);
this change is the "genuine engine-level per-dab state" case it describes (extend
`CommandCtxBase` + `isCtxBase`, and — because the values are global to every
brush like `surfaceNo`/`render_matrix` — extend the **base block** of
`ComputeCtxUniforms`, not the per-kernel union).

## Design decisions (confirmed with user)

| Question | Decision |
|---|---|
| Semantics | `viewNo` = unit eye→surface direction; `viewDist` = its length |
| Population | Threaded from the caller, exactly like `renderMatrix` |
| Backend scope | Full parity: CPU executor + all GPU emitters + `compute_layout.h` + marshal |

## Implementation

### 1. Core ctx struct + C++ emitter (CPU path)

- **`source/brush/brush_command.h`** — add to `struct CommandCtxBase` (after
  `mouseDir` / `renderMatrix`, ~line 66):
  ```cpp
  float3 viewNo;        // unit eye->surface direction (per dab)
  float  viewDist = 0;  // distance eye->surface center
  ```
  (`viewNo` left default-constructed like `surfaceNo`; the caller fills it.)

- **`source/brush/compiler/emit_cpp.cc`** — add `"viewNo"` and `"viewDist"` to
  the `isCtxBase` name list (~lines 171-178) so they lower to `ctx.viewNo` /
  `ctx.viewDist` rather than `ctx.brush.*`.

### 2. GPU emitters (WGSL / CUDA / OpenCL)

In each emitter, (a) add the two names to its `isBuiltinCtxName` lambda, and
(b) append them to the hardcoded `CtxUniforms` prelude **immediately after
`render_matrix`** (before the custom-field loop), so they sit in the global base
block ahead of any per-kernel ctx fields:

- **`emit_wgsl.cc`** (`isBuiltinCtxName` ~674; prelude ~742-747):
  `viewNo: vec3<f32>,` then `viewDist: f32,`
- **`emit_cuda.cc`** (`isBuiltinCtxName` ~686; prelude ~730-733):
  `float3 viewNo;` then `float viewDist;`
- **`emit_opencl.cc`** (`isBuiltinCtxName` ~430; prelude line ~452):
  add `float3 viewNo; float viewDist;` to the struct line.

No camelCase→snake rename is needed (only `renderMatrix`→`render_matrix` is
special-cased; `viewNo`/`viewDist` pass through verbatim, like `surfaceNo`).

### 3. GPU host mirror + marshal

- **`source/brush/compute_layout.h`** — extend the base block of
  `struct ComputeCtxUniforms`, inserted **before** the `union global` (after
  `render_matrix`):
  ```cpp
  float render_matrix[16] = {...};  // @32, ends @96
  float viewNo[3] = {0, 0, 1};      // @96
  float viewDist  = 0.0f;           // @108 — packs into viewNo's vec3 tail
  union { ... } global = {};        // now @112 (16-aligned)
  ```
  **std140 detail (the one tricky part):** a `vec3` is align-16/size-12, so the
  following `f32` packs at offset 108 (no pad), and the next 16-aligned member
  (the union's first `vec3`, e.g. kelvinlet `grabFrom`) lands at **112**. This
  shifts the union tail from offset 96 → 112; base block 96 → 112 bytes; total
  224 → 240. Update the struct's header comment and the in-union offset comments
  accordingly (kelvinlet `grabFrom` 96→112 / `grabTo` 112→128; pose
  `poseCageRest` 96→112 / `poseCageNow` 160→176). This same packing pattern
  already exists in `ComputeBrushUniforms` (`coord_space` at offset 60, packed
  into `falloff_extent`'s tail) — follow it.

- **`source/debug/gpu_stroke.cc`** — in the `GpuStrokeSession` marshal where
  `cu.surfaceNo`/`render_matrix` are filled (~653-668), add:
  ```cpp
  cu.viewNo[0] = scene.viewNo[0]; cu.viewNo[1] = scene.viewNo[1]; cu.viewNo[2] = scene.viewNo[2];
  cu.viewDist  = scene.viewDist;
  ```
  No vk/wgpu dispatcher changes — they `memcpy(sizeof(ComputeCtxUniforms))`, so
  the larger struct is handled automatically (there is no hardcoded size
  constant; `sizeof` is the source of truth).

### 4. Caller plumbing (debug Scene + tests) — mirrors `renderMatrix`

- **`source/debug/scene.h`** — add next to `renderMatrix` (~line 78):
  `float3 viewNo;` and `float viewDist = 0.0f;`.
- **`source/debug/scene.cc`** — initialize in the ctor (next to
  `renderMatrix.identity();`, ~line 16), e.g. `viewNo = {0, 0, -1}; viewDist = 0;`.
- **`source/debug/script.cc`** — two edits:
  - At both sites that do `exec.ctx.renderMatrix = scene.renderMatrix;`
    (~788 and ~881) add `exec.ctx.viewNo = scene.viewNo;` and
    `exec.ctx.viewDist = scene.viewDist;`.
  - Add a `set_view` script verb modeled on `set_render_matrix` (~682-705):
    parse `no=<x,y,z>` and `dist=<f>` into `scene.viewNo` / `scene.viewDist`,
    so the debug harness / A-B scripts can drive the values deterministically.

The C++ executor (`brush_executor.h`) needs **no change**: like `renderMatrix`,
these persist on `ctx` from the caller's assignment across dabs; `execBrush` only
overwrites the per-dab `surfaceNo`/`surfacePos`.

### 5. Documentation

- **`documentation/addingSBrushUniforms.md`** — add `viewNo` / `viewDist` to the
  builtin `CommandCtxBase` list (~line 49) and to the examples row (~line 34).

## Files touched (summary)

Core/CPU: `brush_command.h`, `compiler/emit_cpp.cc`.
GPU emit: `compiler/emit_wgsl.cc`, `compiler/emit_cuda.cc`, `compiler/emit_opencl.cc`.
GPU host: `compute_layout.h`, `debug/gpu_stroke.cc`.
Caller: `debug/scene.h`, `debug/scene.cc`, `debug/script.cc`.
Docs: `documentation/addingSBrushUniforms.md`.

## Verification

1. **`node make.mjs codegen`** — regenerates kernels. Expected: **no diff** in the
   committed `source/brush/kernels/generated/*.brush.gen.h` (no kernel references
   the new fields yet, and the C++ path emits no `CtxUniforms` struct). Confirms
   the change is additive and inert for existing CPU kernels.
2. **`node make.mjs build native`** then **`node make.mjs test`** — the native
   build must compile (validates the `CommandCtxBase` + emit_cpp changes) and the
   brush/dyntopo ctest suite must stay green.
3. **`node make.mjs sbrush-verify`** — the key parity gate. It regenerates the
   GPU shaders fresh (now with `viewNo`/`viewDist` in `CtxUniforms`) and runs the
   C++-vs-WGSL A/B for every brush. This is what proves the std140 base-block
   shift (kelvinlet/pose custom tails moving 96→112) is mirrored correctly in
   `compute_layout.h`; a padding/offset mistake surfaces here as an A/B mismatch.
4. **Smoke a kernel using the new fields** — temporarily add
   `ctx float3 viewNo; ctx float viewDist;` to a scratch/test kernel (or
   `draw.sbrush` locally), `codegen`, confirm it compiles on CPU and validates
   under `sbrush-validate wgsl`, then revert. Confirms the DSL-referenceable end
   of the feature actually works on all backends. (No new brush kernel is shipped
   by this change — it is plumbing only.)

## Out of scope

- No new brush kernel that *consumes* `viewNo`/`viewDist` is added; this change
  only makes them available.
- No TypeScript / N-API surface change: like `renderMatrix`, these are threaded
  via the C++ debug `Scene`, not passed across the TS C-API. Exposing them to the
  TS app would be a separate follow-up if/when an app brush needs them.
