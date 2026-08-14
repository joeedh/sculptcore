# Texture scripts (`.stex`) — reference

Standalone texture units for the sbrush DSL: a `.stex` file holds top-level
`texture Name { float eval(float3 p, float3 n) { ... } }` blocks, pure
`(p, n) -> float` functions with no uniforms, attribute access, stages, or
undo interaction. That purity is what lets textures — unlike brushes, which
stay strictly compile-time — be compiled **at runtime** on both CPU and GPU.

This is the durable reference for the shipped system. Design history and
rationale (backend choice, rejected alternatives, milestone log) live in
[`plans/texture-scripts.md`](plans/texture-scripts.md); the DSL syntax for
`texture` blocks and `use texture` imports is in
[`brush_compute_dsl.md`](brush_compute_dsl.md).

## Two compilation paths

**Precompiled** (build time): `sbrushc --texture-unit` lowers each texture in
`kernels/*.stex` to `kernels/generated/<stem>.tex.gen.h` (guarded C++ eval +
embedded WGSL module text); `--texture-registry` emits the row table behind
`texture_registry.h`. A brush imports one with `use texture <Name>;` and the
definition is emitted exactly as if it were inline. Extra kernel dirs may add
their own `.stex` units, which shadow the checked-in registry.

**Runtime** (`texture_program.cc/.h`): `compileTextureScript(source, error)`
parses the same grammar and returns a `TextureProgram` —

- **CPU**: the C99 emitter (`compiler/emit_c.cc`) feeds vendored tinycc
  (`texture_jit.cc/.h`); the JIT'd TU exports `tex_<lower>_eval`,
  `tex_<lower>_eval_d` (dual twin), and `tex_<lower>_param_defaults`.
- **GPU**: the program carries WGSL text; `spliceTextureProgramWgsl(kernelSrc,
  p, error)` renames the kernel's own `fn brush_sample_tex(` to `_bitmap`,
  appends the program's WGSL plus a wrapper that calls `tex_<lower>_eval(co,
  no[, ctx_u.render_matrix when usesMap])`. Splicing happens at stroke begin
  (`GpuStrokeSession::begin`), **WebGPU/wgpu-native dispatcher only** — the
  SPIR-V/CL precompiled backends refuse programs, and the marshal-only c-api
  session returns null with a program bound (the WASM host loads its own
  WGSL). Failure at any gate falls back to the CPU executor.

A bound program takes precedence over the host-state bitmap texture:
`sampleBrushTex` (folded into the `strength()` intrinsic on every backend)
short-circuits to the program. `strength()` returns before `sampleBrushTex`
when falloff already produced zero — the spatial walk hands kernels every
vertex of overlapping leaf nodes, and evaluating a texture for out-of-radius
vertices was pure waste (`0 × tex == 0`, so A/B bit-exactness is unaffected).
`@unbounded` kernels (kelvinlet) window their own `sampleBrushTex` calls.

## Parameters and ramps

Textures declare `param` values (with defaults) and may use a color-ramp LUT.
`Brush::texture_params` is a live float slab — `setTextureParamAt` /
`setTextureRampAt` update it between dabs with no recompile, so host UI
changes are cheap (a warm param push is ~0.1 ms vs ~80–90 ms for a cold
compile). On the GPU the slab crosses in **binding mode**: the spliced module
declares `@group(0) @binding(26) var<storage, read> sb_tex_params`
(`brush::kTexParamsBinding` in `compute_layout.h`) and non-const param reads
index the slab instead of a baked defaults const. Ramps travel as a 256-float
LUT in the same slab.

## Host samplers (`host_sampler.cc/.h`)

Engine-global, name-keyed registry of host-provided sample functions a script
can call after declaring them: `sampler float vnoise(float3 p);` at top level,
then call like a function. Entries are heap-allocated, never freed
(`PermanentGuard` — JIT'd TUs bind `sb_hs_<name>` slots straight to the entry
pointer, so it must outlive every program), updated in place by name;
unregister clears the callbacks but keeps the entry.

- `fn` (CPU value) is required; `fn_grad` (analytic gradient) is optional —
  without it the engine synthesizes 6-tap central differences at `fd_step`,
  on the CPU and in WGSL alike, so CPU/GPU parity holds either way. Inside a
  `grad()` rewrite a sampler call chain-rules the spatial gradient against
  the dual's Jacobian.
- `wgsl` is a snippet defining `fn hs_<name>(p: vec3f, n: vec3f) -> f32`
  (helper functions are legal — snippets are prepended verbatim). An empty
  `wgsl` clears `gpuAvailable` on every texture calling the sampler, which
  disables the GPU brush path for those strokes. Precompiled backends error:
  samplers are runtime-only.
- C API: `sc_host_sampler_register` / `sc_host_sampler_unregister`. The
  tcc-side binding resolves through TU-defined `void *sb_hs_<name>` slots
  filled via `tcc_get_symbol` after relocate (extern data symbols don't
  resolve through tcc's PE path; see `emit_c.cc`).

### Builtin samplers

`registerBuiltinHostSamplers()` (idempotent, called at the top of
`compileTextureScript`, so every compiling process has the builtins with no
init-order dependency) registers natively-compiled noise bases. Currently:

- `vnoise(float3 p)` — one octave of 3D lattice value noise in `[0, 1)`:
  integer-hashed corners (multiply-xor mix + lowbias32-style avalanche),
  smoothstep-faded trilinear blend, with a term-for-term WGSL twin so spliced
  programs stay `gpuAvailable`. FD gradients (`fd_step` 1e-3).

Builtins exist because of tcc's codegen (below): a noise basis inlined in the
DSL pays unoptimized per-op cost at every octave of every vertex, while a
builtin runs at native `-O2`. Host features that scripts need hot should
become builtins, not host callbacks (a ctypes callback also takes the GIL per
call, serializing the executor).

## Performance: what tcc does to your script

tinycc does **zero optimization** — every DSL op is memory-to-memory, roughly
10× clang `-O2` per operation. Three consequences for authoring `.stex`:

1. **Intrinsic macros substitute arguments textually.** `fract` and `mix`
   expand their argument twice, so `fract(f(x))` evaluates `f(x)` twice.
   Bind compound expressions to a local first.
2. **`sinf` / `floorf` etc. are real extern libm calls per use** — no
   intrinsic lowering, no CSE.
3. **Heavy per-vertex math belongs in a builtin sampler.** The clouds
   procedural went from 21.0 ms/dab with a DSL-inlined fract-sin basis to
   2.66 ms/dab calling `vnoise` per octave (263k-vert grid, depth 2 —
   ~0.375 ms/dab per octave, ~2× the bitmap-bake path; the strength()
   zero-gate is part of that ratio).

## Autodiff

`grad()` works through texture programs: every emitter has dual twins
(`sbdual` / `sbdual3`), `sbd_mapPoint` is exact, and sampler calls chain-rule
as above. Texture-gradient brushes are forced `accumulable = false`. Gate:
the `TEXGRAD` builtin kernel + `texgrad_ab.txt` golden. Known gap: a
precompiled unit imported into a runtime splice does not get a WGSL dual
prelude (`brushUsesGrad` checks brush stages only) — recorded in the plan's
Open questions.

## Tests and gates

- `test_texture_program` — compile, `testSamplers` (host registry, FD grads),
  `testBuiltinVnoise` (range, non-constancy, cell-border continuity, WGSL
  twin present), `testSplice`.
- `test_texture_c_jit` — the tcc path end to end.
- `test_texture_stroke` — DRAW + JIT'd Rings vs the precompiled TEXDRAW
  kernel at 1e-5.
- `draw_script_tex_ab.txt` (+ `tests/assets/draw_script_tex.stex`) — params
  and ramp through binding 26, `mapPoint` through the ctx render matrix.
  Runs under **wgpu-native-verify only** (`SCRIPT_TEX_BRUSHES` in
  `make.mjs`); sbrush-verify and webgpu-verify skip it.

Note `make.mjs test <name>` does **not** rebuild — build first or a stale
binary passes silently.

## Host integration (Blender addon)

`sculptcore_addon/texture.py` routes 3D-mapped procedural brush textures to
runtime programs (`sculptcore_addon/stex/clouds.stex` for `CLOUDS`; other
types slot into `_SCRIPT_TYPES`) instead of the 128×128 bake — infinite
extent, settings live in the param slab, ramp as a 256-float LUT; non-3D
mappings fall back to the bake. It also exposes `register_sampler` /
`unregister_sampler` over the c-api with a keep-alive dict for the ctypes
trampolines.
