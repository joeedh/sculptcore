# `sbrush` — the brush DSL

`sbrush` is a small statically-typed DSL for authoring sculpt brushes once
and compiling them to every backend (C++, WGSL, SPIR-V, CUDA, HIP, OpenCL).
Brushes live as `.sbrush` files in `source/brush/kernels/`. This document is
the language reference; the compiler and build/verify machinery are in
[`brush_compute.md`](brush_compute.md), and the brush runtime in
[`brush.md`](brush.md).

The surface syntax is C/HLSL-flavored. The C++ emitter is the reference, so
the semantics of any construct are "what `emit_cpp.cc` produces" — every
other backend matches it bit-for-bit modulo floating point.

## A complete example

```sbrush
// Draw — push the surface along its normal by strength * falloff.
@brush("draw")
brush Draw {
  ctx float3 surfaceNo;
  uniform float radius;

  vertex void apply(inout Vertex v) {
    float s = strength(v.co);
    s *= sampleBrushTex(v.co, surfaceNo);  // 1.0 when no texture is bound
    if (s == 0.0) {
      continue;                            // skip this vertex
    }
    v.co += surfaceNo * s * radius * 0.5;
  }
}
```

`@brush("draw")` sets the registered name; `brush Draw { … }` names the
generated C++ symbol. The body is one or more *stages*.

## Brush structure

A `.sbrush` file declares exactly one `brush`. Inside it, in any order:

- **fields** — `uniform` and `ctx` declarations (brush state).
- **`struct`** blocks — user aggregate types.
- **`texture`** blocks — inline procedural textures.
- **stages** — `vertex`, `reduce`, `host`.

> **Reserved words.** The attribute-domain keywords `vertex`, `face`, `edge`,
> and `corner` are reserved by the lexer — you can't name a local, uniform, or
> attribute handle `face`/`edge`/`corner` (no current kernel does).

### Fields: `uniform` and `ctx`

```sbrush
uniform float strength, radius;     // brush properties (from props::StructProp)
uniform float mu, nu;
ctx     float3 surfaceNo;           // per-dab context (from CommandCtxBase / Brush)
ctx     float3 grabFrom, grabTo;
ctx     Array<float3, 4> poseCageRest, poseCageNow;
```

`uniform` and `ctx` differ only in cadence/marshalling, not syntax:
`uniform` is brush properties marshalled once; `ctx` is per-stroke-dot state.
A field named `X` resolves to the corresponding host value — on the C++
backend a `ctx float3 surfaceNo` reads `ctx.surfaceNo`, and a field backed by
`Brush` reads `ctx.brush.X`. Declaring a field is how you opt a name into
identifier resolution; the field must already exist on the host side
(`Brush` / `CommandCtxBase`). The GPU backends pack these into fixed-schema
uniform blocks so every kernel shares one bind-group layout.

### Stages

| Stage | Cadence | Lowering |
|---|---|---|
| `vertex` | per-vertex, parallel | the hot kernel — compute shader on GPU, `vertexIter` loop on CPU |
| `reduce` | once before `vertex` | scalar block producing `out` values passed into `vertex` by name |
| `host` | once per dab, CPU only | never lowered to a backend; runs natively (e.g. param clamps, BVH queries) |

The `vertex` stage's first parameter is always `inout Vertex v`. Parameter
directions are `in` (default), `out`, `inout`.

```sbrush
host void clampParams() {              // CPU-only sanitization
  if (nu > 0.499) nu = 0.499;
  if (mu < 1e-6) mu = 1e-6;
}

reduce void prep(out float a, out float b) {   // computed once per dab
  a = (1.0 + nu) / (2.0 * mu);
  b = a / (4.0 * (1.0 - nu));
}

vertex void apply(inout Vertex v, in float a, in float b) {
  // `a`, `b` arrive from the reduce stage, matched by name.
  ...
}
```

### The `Vertex` type

The `inout Vertex v` parameter exposes the per-vertex mesh attributes:

- `v.co` — `float3` position (write to displace the vertex)
- `v.no` — `float3` normal
- `v.mask` — `float` sculpt mask (kernels typically gate by `1.0 - v.mask`)

## Types

| Category | Types |
|---|---|
| Scalars | `bool`, `int`, `float` |
| Vectors | `float2`, `float3`, `float4` |
| Aggregate | user `struct`, `Array<T, N>` (fixed size) |
| Special | `Vertex` (vertex-stage parameter only), `void` |

Vector constructors are calls: `float3(0.0, 0.0, 0.0)`. Members are `.x/.y/.z`
(and `.w`); `Array<T,N>` is indexed with `[i]`. Layout matches
`litestl::math` types so WASM-side mirrors are zero-copy. No pointers, no
recursion (WGSL/Vulkan/OpenCL-1.2 constraints).

```sbrush
struct KelvinletState { float a; float b; }     // user struct

ctx Array<float3, 4> poseCageNow;                // fixed array
... poseCageNow[i] ...                           // subscript
```

## Statements and expressions

Standard C-like control flow: `if`/`else`, `for (init; cond; step)`,
`return`, `continue`, blocks, local declarations (`float s = …;`), and
assignment with `= += -= *= /=`. Operators: arithmetic `+ - * /`,
comparison `== != < <= > >=`, logical `&& || !`, unary `-`/`!`.

`continue` in a `vertex` stage skips the current vertex — the idiom for an
early-out when a vertex is outside the brush:

```sbrush
float s = strength(v.co);
if (s == 0.0) { continue; }
```

### `for_neighbor` — one-ring iteration

Brushes that average over connectivity (smooth, fair) use `for_neighbor`,
which the compiler legalizes per backend (a one-ring walk on CPU; a CSR
indirection buffer preloaded by the dispatcher on GPU):

```sbrush
vertex void apply(inout Vertex v) {
  float s = strength(v.co) * (1.0 - v.mask);
  if (s == 0.0) { continue; }
  float3 avg = float3(0.0, 0.0, 0.0);
  float n = 0.0;
  for_neighbor (nb in v) {
    avg += nb.co;          // nb exposes co / no / v like Vertex
    n += 1.0;
  }
  if (n > 0.0) { v.co += (avg / n - v.co) * s; }
}
```

## Builtins (intrinsics)

Intrinsics are declared in `kernels/ir/intrinsics.cc` with one lowering
pattern per backend; a name with no pattern for the active backend is a
compile error. Current set:

| Intrinsic | Signature | Notes |
|---|---|---|
| `strength(co)` | `float3 → float` | brush falloff strength at a world position — the `strength*falloff` blend (`brush.strength × falloffEval(t)`). Radius is **not** folded in; a kernel that wants radius-proportional displacement multiplies by the `radius` uniform itself (e.g. `draw` does `… * radius * 0.5`) |
| `falloff(t)` | `float → float` | raw curve sample of normalized distance `t`; lower-level than `strength` |
| `sampleBrushTex(co, no)` | `float3, float3 → float` | brush-texture modulation per the brush's coord space; returns `1.0` when no texture is bound |
| `length` / `distance` | `float3[,float3] → float` | |
| `dot` | `float3, float3 → float` | |
| `normalize` / `cross` | `float3[,float3] → float3` | |
| `mix` | `float, float, float → float` | linear interpolate |
| `min` / `max` | `float, float → float` | |
| `clamp` | `float, float, float → float` | |
| `abs` / `sqrt` | `float → float` | |
| `sin` / `cos` / `floor` / `fract` | `float → float` | `fract` lowers to `x - floor(x)` everywhere |
| `grad(expr, var)` | `(scalar expr, float3 var) → float3` | forward-mode gradient — see below |

## Inline procedural textures

A `texture` block declares a pure `eval(p, n)` function callable as
`Name.eval(...)`. It sees only its own parameters plus intrinsics — no
`ctx`/`uniform` state — so it lowers to a free function on every backend and
is bit-identical across them.

```sbrush
texture Rings {
  float eval(float3 p, float3 n) {
    float d = length(p);
    float rings = 0.5 + 0.5 * sin(d * 40.0);
    float bands = floor(fract(d * 6.0) * 4.0) * 0.25;
    return rings * bands;
  }
}

vertex void apply(inout Vertex v) {
  float s = strength(v.co) * Rings.eval(v.co, surfaceNo);
  if (s == 0.0) { continue; }
  v.co += surfaceNo * s;
}
```

## Falloff and brush textures (host state)

The `strength`/`falloff`/`sampleBrushTex` intrinsics read brush state the DSL
doesn't declare directly — the falloff curve, its kind/shape, and the bound
texture all live on `Brush` and are configured at runtime (debug-app verbs
`set_falloff`, `set_texture`, `set_coord_space`). The two orthogonal falloff
axes (`FalloffKind` curve shape × `FalloffShape` spatial metric) and the five
texture coord spaces (`GLOBAL`, `VIEWPLANE`, `VIEW_REPEAT`, `STROKE_CURVED`,
`PROJECTED`) are documented in [`brush_compute_dsl.md`](brush_compute_dsl.md).

## `grad` — forward-mode autodiff

`grad(expr, var)` returns the `float3` gradient of a scalar `expr` with
respect to a `float3 var`: `(∂expr/∂var.x, ∂expr/∂var.y, ∂expr/∂var.z)`.
`expr` is any scalar expression built from `var`, constants, arithmetic, and
the differentiable intrinsics (`sin`, `cos`, `sqrt`, `abs`, `dot`, `length`,
`mix`, and the arithmetic operators).

```sbrush
// graddraw.sbrush — ridge the surface along a ripple field's gradient.
@brush("graddraw")
brush GradDraw {
  ctx float3 surfaceNo;
  vertex void apply(inout Vertex v) {
    float s = strength(v.co);
    if (s == 0.0) { continue; }
    float3 g = grad(sin(length(v.co) * 40.0), v.co);   // = 40·cos(40|p|)·p̂
    v.co += normalize(g) * s;
  }
}
```

It is forward-mode dual-number autodiff realized in the emitters: scalars
become `(value, derivative)` pairs and float3 carry a 3-column Jacobian, with
`var` seeded to the identity. No DSL type or annotation is needed — just call
`grad`. The same rewrite shape runs on all six backends, so the gradient is
bit-identical modulo fp. Mechanism and limits are in
[`brush_compute.md`](brush_compute.md#autodiff-grad); reverse-mode is
deferred.

## Adding a brush

1. Write `source/brush/kernels/<name>.sbrush`.
2. `node make.mjs codegen` to emit `kernels/generated/<name>.brush.gen.h`.
3. Add the include to `brushes/all.h`, an enum entry to `brushes/types.h`,
   and a dispatch case in `CommandExecutor::createCommand()` (see
   [`brush.md`](brush.md)).
4. For GPU dispatch and the A/B harness, add a `runBrushStrokeGPU` case and a
   `tests/scripts/brush_backends/<name>_ab.txt` script (see
   [`brush_compute.md`](brush_compute.md#verification)).

A validate-only demo brush (like `graddraw`) can stop after step 2 — it is
exercised by `sbrush-validate`/`sbrush-verify`'s per-backend compile gate
without being wired as a tool.
