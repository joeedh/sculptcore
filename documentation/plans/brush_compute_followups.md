# Brush compute — follow-up slices

The six-wave sbrush DSL roadmap (`documentation/brush_compute_dsl.md`) is
implemented: all 10 shipping brushes compile to all six backends (C++, WGSL,
SPIR-V, CUDA, HIP, OpenCL), `sbrush-validate` gates per-backend compile, and
`sbrush-verify` gates C++-vs-GPU A/B bit-for-bit modulo fp. Real WGSL GPU
compute dispatch landed for the untextured DRAW brush (commit `998e426`,
`plans/wgsl_gpu_dispatch.md`).

This file collects every item the plan and its slices explicitly deferred, as
independently-schedulable follow-ups. None is required for the DSL/compiler to
be feature-complete; each extends breadth or moves a validate-only gate to a
real-execution gate.

Ordering below is rough priority (1 is the biggest open gap), not a dependency
chain — items are largely independent.

---

## 1. Real GPU dispatch for brushes beyond DRAW

**Status:** deferred. **Source:** `plans/wgsl_gpu_dispatch.md` (scoped that
slice to "DRAW brush only").

`plans/wgsl_gpu_dispatch.md` wired `set_backend wgsl` in the debug app to a
real Vulkan compute pipeline (`source/vulkan/vk_compute.{h,cc}`,
lavapipe in CI) for DRAW only. Every other brush already emits valid
WGSL/SPIR-V, but `sbrush-verify`'s "cross-backend" assertion still runs the
WGSL side through the C++ executor for them — so the real GPU path is unproven
past DRAW.

Scope to wire the rest:

- Brushes with extra DSL `uniform`s spill past DRAW's 10 builtin
  `BrushUniforms` fields — extend the marshaling in the debug-app GPU stroke
  path to pack the per-brush uniform tail (std140) from `scene.brush`.
- `for_neighbor` brushes (SMOOTH, and any fair-style brush) need the CSR
  one-ring indirection buffer preloaded and bound; confirm the binding-10+
  layout the WGSL emitter expects and build it host-side.
- `reduce`-stage brushes (KELVINLET, POSE) pass reduce outputs into the vertex
  kernel — confirm those arrive as uniforms/constants on the GPU path.
- Brushes that write `no` (recompute normals) need `no_buf` read back into
  `mesh.v.no`; DRAW leaves it a passthrough.
- TEXDRAW / textured brushes depend on item 3 (image textures) for a non-1×1
  `brush_tex` binding.
- Extend `tests/scripts/brush_backends/<name>_ab.txt` so the WGSL leg dispatches
  on the GPU (not the C++ shim) and add native assertions that cpp == wgsl
  within fp epsilon for each.

**Done when:** `sbrush-verify` (and `webgpu-verify`) execute every shipping
brush through the real GPU compute path, not the C++ shim, and pass.

---

## 2. Image-texture inputs (`Tex2D` + bilinear sample)

**Status:** deferred. **Source:** `brush_compute_dsl.md` inline-texture section;
verify-harness section ("`set_texture image=`/`proc=` verbs … deferred").

Today `set_texture` takes synthetic `pattern=` inputs only, and the only
texture modulation is the inline procedural `@texture`/`Name.eval(...)` path
(TEXDRAW). To support real bound image textures:

- Add a `Tex2D` DSL type and a bilinear `sampleTex2D`-style intrinsic, declared
  with one lowering per backend in `kernels/ir/intrinsics.cc` (C++ reference
  first, then the GPU backends; WGSL `texture_2d<f32>` + `sampler` already
  reserved at binding 8/9 in the dispatch layout).
- Debug-app `set_texture image=<path>` / `proc=<name>` verbs to bind a real
  image (or named procedural) rather than `pattern=`.
- `assert_png` per-pixel PNG diffing in the debug app for image-output
  regression (the verify harness currently fingerprints `co_sum`/`co_sqsum`
  only).

**Done when:** a brush can sample a bound image texture, it lowers + validates
on all six backends, and an `assert_png`-gated A/B script covers it.

---

## 3. Cross-brush texture sharing

**Status:** deferred. **Source:** `brush_compute_dsl.md` inline-texture section
("Cross-brush texture *sharing* … remains deferred").

An inline `@texture`/`texture Name { … }` block is currently local to the brush
that declares it and lowers to a per-brush free function. Allow one `@texture`
definition to be referenced by multiple brushes (shared free function / shared
binding) without copy-paste. Smaller than items 1–2; mostly a
compiler/name-resolution change plus a decision on where shared texture sources
live (`kernels/ir/` or a shared `kernels/textures/`).

---

## 4. Reverse-mode autodiff

**Status:** deferred. **Source:** `brush_compute_dsl.md` "Analytical
differentiation" section.

Forward-mode `grad` (Wave 6) is implemented as the shared `emitDual` rewrite.
Reverse-mode is a separate pass over the same IR: a tape per basic block.
Brushes have no loops over unbounded data except neighbors, so `for_neighbor`
loops are either unrolled-by-runtime or use accumulator tapes. The design was
kept open for this (single intrinsic-derivative table in
`kernels/ir/intrinsics.cc`; no raw pointer arithmetic; no `inout`/read
aliasing). Only worth doing if a brush needs the gradient of a many-output /
few-input function where reverse-mode wins — no current brush does.

---

## 5. CI workflow `.github/workflows/brush-backends.yml`

**Status:** not written. **Source:** `brush_compute_dsl.md` CI-image section
("The future GitHub Actions workflow … not yet written").

Per-backend validation runs locally via `node make.mjs sbrush-validate
<backend>`; there is no CI workflow yet. The `.devcontainer/Dockerfile` is
already provisioned with every backend toolchain (tint, spirv-tools + glslang,
nvcc, hipcc, clspv/pocl on clang-18) and clspv is split into the published
`.devcontainer/clspv-base.Dockerfile` base image, so the workflow is "wire it
up", not "build the image":

- `docker build -f .devcontainer/Dockerfile .` from the repo root (with
  `ARG CLSPV_BASE_TAG` matching `CLSPV_COMMIT` in `ci/versions.env`).
- Run the documented sequence: `install-emsdk`; `configure native
  --backends=cpp,wgsl,spirv,cuda,hip,opencl`; `build native`; `test native`;
  `configure wasm --backends=cpp,wgsl`; `build wasm`.
- Each `build` runs `sbrushc` for every enabled backend with
  `SBRUSH_VALIDATE_ALL=ON`; any external-validator failure fails the job with
  the tool output in the log.

**Done when:** a push runs all six backend validators (plus `sbrush-verify`) in
GitHub Actions on the shared devcontainer image.

---

## 6. Browser A/B backend flag (TS-side smoke)

**Status:** deferred. **Source:** `brush_compute_dsl.md` "TS-side smoke"
section (Wave 3+).

Add a `--sculpt-backend=wgsl|spirv|cpp` debug flag in
`scripts/editors/view3d/tools/sculptcore_ops.ts` mirroring the debug-app
`--backend` flag, so the same A/B comparison can run interactively in the
browser through the WASM module. The debug-app harness remains the gating
signal; this is for interactive exploration only.

---

## Out of scope here

Spatial-side speedups that overlap the DSL iteration shape (dirty-bounds queue,
per-vert spatial reject, incremental tri-count propagation, GPU node regen,
draw-batch caching) are tracked separately in
[`../proposed-spatial-speedups.md`](../proposed-spatial-speedups.md).
