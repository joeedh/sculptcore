# Standalone texture scripts (`.stex`) — precompiled + runtime-compiled

Design for hoisting the sbrush `texture` block into standalone files that are
(a) shareable across brushes, (b) precompilable at build time like the extra
kernel dirs, and (c) compilable **at runtime** on both the CPU and the GPU.
Texture scripts may additionally call **host-registered sampler functions**
(e.g. a Blender-texture evaluator exposed over ctypes); the host registers a
CPU implementation and optionally a GPU one, and a missing GPU implementation
disables the GPU brush path for strokes using that texture. Autodiff
(`grad()`) must keep working through texture scripts — including through host
samplers, which therefore need a way to provide partial derivatives.

Relation to prior plans: `customSBrushScrips.md` (extra kernel dirs) was
deliberately "strictly compile-time — no runtime parsing/interpretation". This
plan relaxes that **for textures only**: a texture's `eval` is a pure
`(p, n) -> float` function with no uniforms, no attribute access, no stages,
and no undo interaction, so runtime compilation of textures carries none of
the risks that kept brushes compile-time. Brushes stay compile-time.
`sbrush_autodiff.md` defines the dual representation this plan builds on.

## Current state (verified)

- `texture Name { float eval(float3 p, float3 n) { ... } }` exists only
  **inline** in a brush body (`parser.cc` `parseBrush()` → `parseTexture()`;
  `ir.h` `Brush::textures`). One `.sbrush` = one brush; no import mechanism.
  `documentation/brush_compute_dsl.md` lists cross-brush sharing as deferred.
- Each emitter lowers a texture to a pure free function (`texRingsEval` in
  C++, `tex_rings_eval` in WGSL) — already the right unit for sharing.
- All six backends run at **build time** via the `sbrushc` host tool
  (`source/brush/compiler/`, host-only target). But the WebGPU dispatcher
  consumes WGSL **text at runtime** (`wgpu_compute.h` "Read the .wgsl text,
  create the shader module"), so runtime GPU codegen only needs the compiler
  linked into the engine — no new GPU plumbing.
- Autodiff is a per-emitter forward-mode rewrite (`emitDual`), expression-only.
  Duals: `sbdual {float v; float3 d}`, `sbdual3 {float3 v; sb_mat3 j}`
  (Jacobian columns = ∂/∂var.{x,y,z}). WGSL/OpenCL use `sbd_*` **functions**
  (no operator overloading) — exactly the shape a C99 backend needs.
- **Existing gap**: `emitDual`'s Call case has no texture handling —
  `grad(Rings.eval(p, n), p)` emits invalid `sbd_Rings.eval(...)` today. No
  shipped kernel hits it. This plan fixes it (T2) rather than merely not
  regressing it.
- The host-state bitmap texture (`Brush::tex_pixels` + `sampleBrushTex`) is a
  separate mechanism and stays; a bound texture *program* takes precedence.
  `sampleBrushTex` is folded into the `strength()` intrinsic on every backend
  (the blender-brush-textures §4.1 fold), so a bound program reaches every
  kernel that calls `strength` — only `@unbounded` kernels (kelvinlet) sample
  it explicitly.

## Decision: CPU runtime backend — **embed TinyCC (libtcc), not an assembler**

Recommendation: add a C99 emitter (`emit_c.cc`) and JIT its output in-process
with vendored **libtcc** (`tcc_compile_string` → `tcc_relocate` →
`tcc_get_symbol`). Reasons, versus emitting assembly to a shipped assembler:

1. **Per-ISA cost.** An assembly emitter means register allocation, and
   codegen × {x86-64 SysV, x86-64 Win64, arm64 AAPCS64, arm64 Darwin} — the CI
   matrix ships all of ubuntu/windows/macos-arm64. Plus a runtime
   loader (page mapping, relocations, W^X). That is a permanent maintenance
   tax duplicating what libtcc *is* (~a few hundred KB, maintained elsewhere,
   with x86-64 and arm64 backends).
2. **Autodiff for free.** The dual rewrite happens at emit level; a C backend
   reuses the WGSL/OpenCL call-based `sbd_*` shape verbatim. An assembly
   backend would re-implement the whole prelude per ISA.
3. **Parity.** CPU/GPU bit-parity gates (`sbrush-verify`, `*_ab` scripts)
   compare implementations that share one emission shape. tcc does no
   fast-math reassociation and uses SSE/NEON scalar float ops — IEEE, stable.
   A hand-rolled codegen is a third numeric implementation to keep honest.
4. **Host samplers are trivial.** `tcc_add_symbol(state, "hs_noise3", fnptr)`
   binds a registered sampler; an assembler flow needs its own symbol
   resolution/GOT story.
5. **Packaging.** libtcc links **into** `sculptcore_capi.dll` — no extra
   executable through the addon vendoring path, `windows-deps.mjs`, or macOS
   codesigning. A shipped assembler is another binary on every platform and a
   `CreateProcess` per compile.
6. **Licensing.** TinyCC is LGPL-2.1(+), compatible with the project's
   GPL-2.0-or-later; vendor under `extern/tinycc` like glfw.

Known costs, accepted:
- tcc output is unoptimized (~2–4× slower than clang -O2 on scalar FP).
  Texture eval is one small pure call per in-radius vertex per dab; if it ever
  dominates, precompile (T1 path) or a cache-to-disk + system-clang tier can
  be added without changing the ABI.
- macOS hardened runtime: JIT pages need `MAP_JIT` + the `allow-jit`
  entitlement in the **host** process. Whether stock Blender carries it must
  be verified early (T3 gate); fallback if not: runtime CPU compile
  unavailable on macOS → textures fall back to precompiled ones and the
  runtime path reports a clear capability flag the host can query.
- WASM: no tcc. Runtime GPU compile still works (WGSL text). Runtime CPU
  compile is deferred there — a later `emit_wasm` backend (wasm is an easy,
  ISA-free target and the browser instantiates modules natively) or a small
  interpreter can close it. Precompiled textures work on WASM today via the
  generated C++ headers.

## File format and DSL

New extension **`.stex`** (same lexer/DSL). Rationale: every build glob and
registry treats `*.sbrush` as "one brush"; a distinct extension keeps those
invariants and lets texture files sit in the same dirs (`kernels/`,
`<addon>/brushes/`). One file may declare multiple textures plus the samplers
they use:

```
// marble.stex
sampler float noise3(float3 p);          // host-registered, by name
sampler float blender_tex(float3 p, float3 n);

texture Marble {
  float eval(float3 p, float3 n) {
    float t = noise3(p * 4.0);
    return 0.5 + 0.5 * sin(p.x * 10.0 + t * 6.0);
  }
}
```

- `sampler <ret> <name>(<params>);` declares an external function. Allowed
  signatures initially: `float f(float3 p)` and `float f(float3 p, float3 n)`.
  Calling an undeclared external name stays an error (today's behavior).
- IR: new `TextureUnit { Vector<SamplerDecl> samplers; Vector<TextureDef>
  textures; string sourceFile; }`. `parse()` gains a top-level dispatch:
  `brush` token → `parseBrush()` (unchanged); `texture`/`sampler` at file
  scope → `parseTextureUnit()`. `TextureDef` itself is unchanged and stays
  usable inline.
- Brush reference: `use texture Marble;` in a brush body imports a standalone
  texture into the brush's callable-texture namespace; thereafter
  `Marble.eval(...)` resolves exactly like an inline texture. Precompiled
  resolution happens in the registry step (collision checks mirror the extras
  registry: duplicate texture names across units/dirs are an error).

## Compilation model

**Precompiled (build time).** Mirrors the extras pipeline
(`source/brush/CMakeLists.txt`): glob `*.stex` from `kernels/` +
`SCULPTCORE_EXTRA_KERNEL_DIRS`, run `sbrushc --backend=cpp --texture-unit`
per file → `<stem>.tex.gen.h` (free functions + dual variants), plus a
registry (`sculptcore_textures.gen.h`) mapping name → C++ fn ptrs + embedded
WGSL string for each texture. Brushes with `use texture` compile against the
registry; standalone textures are also available at runtime by name (for
binding to `Brush` as the procedural brush texture, below).

**Runtime.** Link `lexer/parser/ir/emit_wgsl/emit_c` into the engine library
for native non-WASM builds (they are plain C++ with no host-tool
dependencies; `sbrushc` keeps wrapping the same objects). New API:

```cpp
/** Compiled standalone texture: CPU entry + dual entry + WGSL module. */
struct TextureProgram {
  string name;
  float (*eval)(const float p[3], const float n[3]);        // tcc or precompiled
  void  (*evalDual)(const SbDual3 *p, const SbDual3 *n, SbDual *out);
  string wgsl;              // module text: tex_<name>_eval (+ _dual)
  Vector<string> samplerDeps;
  bool gpuAvailable;        // all samplerDeps have GPU impls
};
TextureProgram *compileTextureScript(stringref source, string &error);
```

Bound to a brush via `Brush::texture_program` (reflected `setTextureScript` /
`clearTextureScript` on the binding, plus a c-api entry for the ctypes
bridge). `CommandCtx::sampleBrushTex` checks the program first: when set, it
evaluates `program->eval(co, no)` (object-space inputs, matching inline
textures; `coord_space` mapping continues to apply only to the bitmap path —
scripts do their own mapping and get raw `co`/`no`). GPU side: the brush
shader is regenerated with the texture's WGSL spliced in and
`brush_sample_tex` redirected to `tex_<name>_eval`; since WGSL modules are
compiled per-stroke-begin already, this is a string concat + the existing
`wgpu_compute` path. If `!gpuAvailable`, the stroke driver's GPU path reports
unsupported and the CPU executor runs — same fallback shape the grids path
already uses for unsupported kernels.

## Host samplers

Engine-global registry, name-keyed:

```cpp
struct HostSampler {
  string name;
  // CPU value fn — required.
  float (*fn)(void *user, const float p[3], const float n[3]);
  // CPU analytic gradient — optional. out = {value, ∂v/∂p.x, ∂v/∂p.y, ∂v/∂p.z}.
  void (*fn_grad)(void *user, const float p[3], const float n[3], float out[4]);
  void *user;
  // GPU: WGSL snippet defining `fn hs_<name>(p: vec3f, n: vec3f) -> f32` and
  // optionally `fn hs_<name>_grad(p: vec3f, n: vec3f) -> vec4f`.
  string wgsl;
  float fd_step;            // finite-difference step when no analytic grad
};
void registerHostSampler(const HostSampler &s);   // + c-api + binding
```

- CPU-only registration (empty `wgsl`) is legal — it just clears
  `gpuAvailable` on every texture that calls the sampler, which per the
  requirement disables the GPU brush path for those strokes.
- **Derivatives.** Inside a `grad()` rewrite, a sampler call becomes a chain
  rule against the sampler's spatial gradient `g = ∂value/∂p`: with
  `sbdual3 p = {v, j}` (columns ∂p/∂var), the result is
  `sbdual {value, jᵀ·g}`. `g` comes from `fn_grad` when registered; when only
  the value fn exists, the engine **synthesizes central differences**
  (6 taps, step `fd_step`) — and synthesizes the *same* FD wrapper in WGSL —
  so CPU/GPU parity holds whether or not the host provides analytic
  gradients. If the host registers analytic grads on both sides, matching
  them bitwise is the host's contract (documented).
- Blender addon use case: register `blender_tex` backed by a ctypes callback
  into `Texture.evaluate` — CPU-only, so textured strokes run the CPU
  executor (which is the addon's only stroke path today anyway,
  per `customSBrushScrips.md`). Note the per-vertex Python callback cost;
  the existing 128×128 bake remains the fast default, scripts the exact one.

## Parameters

Texture scripts need per-texture parameters (noise scale, distortion, …) and
color-ramp LUTs to port Blender's procedurals. Three existing patterns supply
the whole design: the falloff LUT (array transport — 256 floats already cross
to the GPU as `@binding(7) array<vec4<f32>, 64>`), the dynamic-uniforms
manifest (`sbrush-dynamic-uniforms.md` — introspection, defaults/ranges,
stroke-start validation), and the extras `namedFloats` store (dense generated
slots). Brush-scope `uniform` is **not** reused: it lowers to `ctx.brush.X`
members, which a standalone, shareable, runtime-compiled texture must not
depend on.

```
texture Clouds {
  param float scale = 4.0 @range(0.01, 100.0);
  param float distortion = 0.0;
  param int   basis = 0 @const;      // specialized: recompile on change
  param ramp  colors;                // 256-entry LUT, host-baked

  float eval(float3 p, float3 n) {
    float t = noise3(p * scale);
    return colors.sample(t);
  }
}
```

- **Storage: one dense float slab per program.** The compiler assigns offsets
  — scalars one float, ramps `kTexRampSize = 256` floats (the falloff-LUT
  size). `TextureProgram` carries `paramSlabSize` plus a manifest
  `{name, kind (float|ramp), offset, default, min/max, isConst}` mirroring
  `BrushUniformManifestEntry`; the index-keyed query surface
  (`textureParamCount` / `queriedTextureParamEntry`) mirrors
  `queryUniformManifest`, so a host UI generates controls the same way
  `engine_props.py` walks brush uniforms today.
- **Values are per-binding, not per-program.** Beside `Brush::texture_program`
  lives `Vector<float> texture_params`, seeded from manifest defaults at bind
  (ramps seed to the identity ramp). Name-keyed setters
  (`setTextureParam(name, v)`, `setTextureRamp(name, floats)`) resolve
  through the manifest; values clamp to `@range` and reject NaN at stroke
  begin, following the `validateUniformDynamics` precedent.
- **CPU ABI**: eval gains a trailing slab pointer —
  `float eval(const float p[3], const float n[3], const float *params)` —
  and generated code reads `params[kOff_scale]`. Identical for precompiled
  and tcc-JIT'd programs; `evalDual` gains the same argument.
- **GPU**: one new fixed binding
  `@group(0) @binding(26) var<storage, read> tex_params: array<f32>` (attr
  buffers grow 14→22; 23/24/25 are taken; 26 is free). Uploaded at stroke
  begin and when dirtied — params are stroke-constant for now. If parameter
  *dynamics* (pressure-driven scale) are ever wanted, they resolve CPU-side
  per dab like brush uniforms and re-marshal — same "dynamics are a scalar by
  marshal time" rule as `sbrush-dynamic-uniforms.md`.
- **`@const` params** compile as literals: enum-like ints (noise basis,
  metric) become constant-folded branches instead of needing int transport.
  Changing one on a **runtime** program marks it dirty — re-emit + tcc
  recompile (milliseconds) and a fresh WGSL module at next stroke begin,
  which is already when modules are created. On a **precompiled** program
  `@const` params are frozen at their defaults and the setter reports so.
  Non-`@const` params are floats and ramps only — no int uniform plumbing.
- **`ramp.sample(t)`** lowers per backend to the same clamp + piecewise-linear
  LUT fetch as the falloff curve, reading the slab — CPU and WGSL sample
  identical data with identical arithmetic, preserving bit-parity. Resolution
  uses the dotted-call path `findTextureCall` already established.
- **Autodiff**: params are constants w.r.t. `grad()`'s var — dual-lift as
  `sb_c(params[k])`, zero derivative, no new machinery. `sample` gets an
  `sbd_` chain rule: the lerp's exact derivative,
  `(lut[i+1] − lut[i]) · (N−1) · dt/dvar`, piecewise-constant and identical
  across backends.
- **Blender mapping** (addon slice): clouds → `scale`=`noise_scale`,
  `@const basis`=`noise_basis`, `colors` baked from `ColorRamp.evaluate`
  (256 samples); `texture_sample_bias` can ride as a plain param. Enum
  changes recompiling the program is acceptable — enums change rarely;
  sliders never recompile.

Milestone placement: grammar + manifest + CPU slab land in **T1** (they shape
the generated code from the start); `@const` re-specialization lands with
**T3** (it needs the runtime compile path); binding 26 + upload land with
**T5**.

## Map-mode seam (`mapPoint`)

The addon repo's `claudeMemory/design/blender-brush-textures.md` §3 maps every
non-3D Blender map mode (`VIEW`/`TILED`/`STENCIL`/`AREA`/`RANDOM`) as an
affine transform carried by `renderMatrix`, with the contract that
`(M·p).xy / (M·p).w` **is** Blender's texture coordinate (the bitmap path's
uv is that value under `0.5·c + 0.5`, applied inside `sampleViewUv`,
`brush_command.h:394`). Texture scripts get raw object-space `p` and can't
reach that mapping. Resolution: an **intrinsic, not a pre-transformed `p`** —
a script may legitimately mix domains (3D noise masked by a screen-pinned
stencil), which a single pre-transform per texture cannot express.

- **`mapPoint(p) -> float3`**, texture scope (inline and standalone):
  `q = M·(p, 1); return q.xyz / (|q.w| > 1e-6 ? q.w : 1)` — the same guard
  and matrix as `sampleViewUv`, *without* the `0.5·c + 0.5` remap, so the
  result is Blender's texture coordinate directly and the addon doc's
  per-mode matrix recipes apply unchanged. `z` is meaningful only under an
  affine matrix (fourth row `[0 0 0 1]`, e.g. `AREA`), where the divide is a
  no-op; document that, don't guard it. `M` defaults to identity when the
  host never pushes one, so `mapPoint` degrades to a pass-through.
- **Purity is preserved by threading, not by reaching into ctx.** Emitted
  texture free functions stay ctx-free (the property that makes them
  tcc-compilable and backend-identical): using `mapPoint` sets a `usesMap`
  manifest flag and the eval ABI's context argument carries the matrix —
  `eval(p, n, params, texctx)` with `TexEvalCtx { float map_matrix[16]; }`
  (extendable). Call sites have it: `CommandCtx::renderMatrix` on the CPU,
  the per-dab ctx uniform block's `render_matrix` on the GPU (already
  marshalled per dab — `compute_layout.h:59` / `gpu_marshal.cc:189`).
  `mapPoint` lowers to a pure prelude helper taking the matrix explicitly.
- **The matrix is shared with the bitmap path, per-consumer remaps stay
  internal.** `renderMatrix` has exactly one consumer today (`sampleViewUv` —
  verified in the addon doc), so scripts reading it raw add a second consumer
  without a convention conflict: the bitmap path keeps its `[0,1]` remap,
  scripts get the native `[-1,1]` domain.
- **`tex_extend` becomes script logic on this path.** `TILED` repeat is
  `fract()`; `STENCIL`'s zero-outside-rect is a bounds check — e.g. the
  placed-2D wrapper the T4 `blender_tex` sampler needs is just:

  ```
  texture BlenderTex {
    float eval(float3 p, float3 n) {
      float3 q = mapPoint(p);
      if (abs(q.x) > 1.0 || abs(q.y) > 1.0) { return 0.0; }
      return blender_tex(q);
    }
  }
  ```

  The engine-side `tex_extend` enum (addon doc §4.2) remains bitmap-path-only
  work.
- **Autodiff**: `mapPoint` is a rational function of `p` with constant `M` —
  `sbd_mapPoint` is the exact quotient-rule dual (columns of `p`'s Jacobian
  through `M`'s upper 3×4, then per-component quotient rule), all `+ − × ÷`,
  so backends stay bitwise-identical. No finite differences needed.
- **Per-dab matrices are the addon doc's P2, not this seam.** The seam
  contract is only "`mapPoint` reads the matrix current for this dab/mirror
  pass". Stroke-constant modes (`TILED`/`STENCIL`) work with today's one
  pre-stroke push; `VIEW`/`RANDOM`/`AREA` rake/mirror composition inside the
  C++ dab batch (batch-record scalars → engine-composed matrix) lands as P2
  and feeds both consumers — the bitmap path and `mapPoint` — through the
  same `renderMatrix`, which is the point of sharing it.

Milestone placement: grammar + `usesMap` + the `TexEvalCtx` ABI land in
**T1** (they shape the eval signature from the start, alongside the params
slab); `sbd_mapPoint` lands in **T2**; nothing new lands in T5 — the GPU
matrix transport already exists.

## Autodiff (T2 — the load-bearing compiler work)

1. **Statement-level dual transform.** Extend each emitter to also emit
   `tex<Name>EvalD(sbdual3 p, sbdual3 n) -> sbdual`: the eval body re-emitted
   with float locals → `sbdual`, float3 locals → `sbdual3`, expressions via
   `emitDual`, `if`/`return` operating on `.v` for conditions. Eval bodies
   are pure and single-function, which keeps this tractable; it deliberately
   does **not** generalize to brush stages.
2. **Texture calls inside `grad()`.** `emitDual`'s Call case learns the same
   dotted-name resolution `emitExpr` has (`findTextureCall`) and dispatches
   to the `EvalD` variant — fixing the pre-existing invalid-emission gap for
   inline textures too. Params arrive as duals already seeded by the caller;
   the body propagates without reseeding.
3. **Sampler calls inside `grad()`** lower to the chain-rule wrapper above
   (`sbd_hs_<name>`), emitted per-backend next to the sampler binding.
4. Gates: extend `sbrush-verify` / the debug-app `*_ab` scripts with a
   texture-script case whose kernel takes `grad(Tex.eval(v.co, n), v.co)`,
   A/B'd C++-vs-WGSL, plus a tcc-vs-precompiled-C++ A/B of the same script.

## Milestones

- **T1 — standalone units, precompiled** *(done 2026-08-13)*. Parser top-level dispatch +
  `TextureUnit`; `sbrushc --texture-unit` → `.tex.gen.h` + registry; `use
  texture` in brushes; CMake wiring beside the extras block; port
  `texdraw.sbrush`'s Rings to `rings.stex` as the proof (keep the inline form
  in a test to cover both). Docs: `brush_compute_dsl.md` sharing note flips
  to done.
- **T2 — autodiff through textures** (above). Independent of runtime compile;
  fixes the existing inline gap first.
- **T3 — runtime CPU.** `emit_c.cc` (C99: struct float3 + fns, `sbd_*`
  prelude shared shape with OpenCL emitter); vendor libtcc under
  `extern/tinycc`; compiler linked into the engine lib;
  `compileTextureScript` + `Brush::texture_program` + `sampleBrushTex`
  integration; c-api + binding; debug verb `set_texture script=<path>`.
  **Early gate:** MAP_JIT/entitlement probe on macOS; capability query
  (`textureScriptCpuAvailable()`) so hosts can degrade gracefully.
- **T4 — host samplers.** Registry + c-api + `sampler` decls + chain
  rule/FD synthesis + GPU gating (`gpuAvailable`). Addon slice: ctypes
  registration API; optional `blender_tex` sampler.
- **T5 — runtime GPU.** WGSL splice into the brush shader at stroke begin;
  `gpuAvailable` gating through the stroke driver; A/B gates.

## Open questions / risks

- Whether stock Blender's macOS bundle carries `allow-jit` — determines if
  T3 works there or degrades to precompiled-only (probe first, design holds
  either way).
- `.stex` texture ids: none persist (mirrors brush-id story) — binding is by
  name per stroke; runtime programs are anonymous handles.
- Vulkan-native GPU path consumes SPIR-V via tint at build time; runtime
  splice there needs tint at runtime or the WGSL→wgpu path only. Initial
  scope: WebGPU/wgpu dispatcher only (the one the sessions use today).
- Sampler `user` pointers + ctypes callbacks are per-thread-unsafe on the
  Python side; the CPU executor is multithreaded — Blender-backed samplers
  must either take the GIL in the callback (slow) or be evaluated on the
  main thread via a per-dab tile cache. Flagged for the T4 addon slice.
