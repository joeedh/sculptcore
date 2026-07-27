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
    float s = strength(v.co) * masks();
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

### Brush attributes

Any number of bare attributes may sit alongside `@brush("name")`, in any order,
before the `brush` keyword — one per line by convention. They declare what *class*
of kernel this is; the executor and the GPU emitter both branch on them.

```sbrush
@brush("snakehook")
@incremental
brush Snakehook { … }
```

| Attribute | Meaning |
|---|---|
| `@paint` | writes a mesh attribute rather than displacing geometry |
| `@grabmode` | grab-class from-original kernel. Declares *capability* only — the host decides per stroke (`def.grabModeCapable && anchoredGrab`). The stage reads each vert's stroke-start base (derived as live − accumulated displacement, not a snapshot) and the write-back does per-dab first-touch arbitration, so the first symmetry image to touch a vert re-bases it and later images of the same dab add |
| `@relaxation` | relaxes the surface instead of displacing it, so it never contributes to accumulated brush displacement; runs live-from-live even in a non-accumulate stroke |
| `@unbounded` | the field has unbounded support and *is* its own falloff. `strength()` is then forbidden (sema error) and `unbounded_window()` required — the window is what makes the field vanish at the host's node-filter radius instead of tearing on a leaf boundary. Also emits `def.unbounded`, which floors that filter radius at `radius × unboundedExtent` |
| `@incremental` | a stage input is a per-dab **delta**, not an absolute stroke quantity (snakehook's `grabTo` is the step since the last dab), so there is no stroke-start base to re-derive a dab from |

`@paint`, `@unbounded`, and `@incremental` each emit `def.accumulable = false`.
The executor gates its whole non-accumulate path on that bit
(`brush_executor.h`), so such a kernel **always accumulates** on every backend —
the ACCUMULATE brush flag is inert for it, by construction rather than by
convention.

## Brush structure

A `.sbrush` file declares exactly one `brush`. Inside it, in any order:

- **fields** — `uniform`, `ctx`, and `attr` declarations (brush state and bound
  mesh attributes).
- **`save`** declarations — the CPU undo-capture set.
- **`struct`** blocks — user aggregate types.
- **`texture`** blocks — inline procedural textures.
- **stages** — `vertex`, `face`, `reduce`, `host`.

> **Reserved words.** Every keyword in the lexer table is reserved and can't
> name a local, uniform, or attribute handle: `brush`, `uniform`, `ctx`, `attr`,
> `save`, `struct`, `texture`, `vertex`, `face`, `edge`, `corner`, `reduce`,
> `host`, `in`, `out`, `inout`, `for`, `for_neighbor`, `if`, `else`, `return`,
> `continue`, `true`, `false`.

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
`Brush` reads `ctx.brush.X` (inside a `host` stage, which runs natively, the
same field spells as a bare `brush.X`). Declaring a field is how you opt a name
into identifier resolution; for an in-repo kernel the field must already exist on
the host side (`Brush` / `CommandCtxBase`) and be listed in
`Brush::builtinPropNames` — codegen errors out otherwise. Out-of-repo kernels
compiled with `sbrushc --extras` are the exception: there an unlisted *scalar
float* uniform falls through to a generic `Brush::namedFloats` slot, so no host
edit is needed. Other types still error. The GPU backends pack these into
fixed-schema uniform blocks so every kernel shares one bind-group layout.

#### Uniform metadata: default, `@range`, `@static`

A scalar `float uniform` may carry an authored default and bounds, written
**after** the name (per-name in a comma list):

```sbrush
uniform float mu = 1.0 @range(1e-6, 100.0);   // default + validation bounds
uniform float nu = 0.4  @range(0.0, 0.499);
uniform float wingAngle @static;              // opt OUT of device dynamics
uniform float planeoff, planeSide @static, radius;   // annotate one of many
```

| Syntax | Meaning |
|---|---|
| `= <number>` | authored default; codegen emits `.Default(n)` when it auto-registers the prop |
| `@range(a, b)` | inclusive bounds — **validation only, never a clamp**: checked once at stroke start (default inside the range, `a ≤ b`, neither NaN) |
| `@dynamic` | explicit opt *in* to device dynamics; this is already the default, so it is only ever written for emphasis |
| `@static` | the uniform is **not** dynamic-capable: codegen skips it entirely for prop registration and uniform loading, leaving it a plain host-set `Brush` member, and the executor rejects any device dynamic bound to it |

Every non-`@static` `float uniform` is automatically registered as a brush
**property** and is drivable by a device dynamic (pen pressure/tilt/etc.),
keyed by the uniform's name. This registration, the manifest, and the
pre-invocation validation are all generated from these declarations — see
[`addingSBrushUniforms.md`](addingSBrushUniforms.md) and the plan in
[`plans/sbrush-dynamic-uniforms.md`](plans/sbrush-dynamic-uniforms.md).

> **Adding a new field?** See
> [`addingSBrushUniforms.md`](addingSBrushUniforms.md) for the step-by-step
> (host member, prop registration, and the GPU host-mirror/marshal seam).

### Fields: `attr` — bound mesh attributes

Beyond brush state, a kernel can read and write typed mesh attribute layers:

```sbrush
attr vertex float4 color = "Col";     // fixed layer name
attr vertex float slayer;             // bound at runtime via Brush::attrBindings
attr face int group;
```

```
attr <vertex|face|edge|corner> <type> <name> [= "<layerName>"];
```

The handle becomes a member of the element bundle — `v.color`, `v.slayer`,
`f.group`, and on a neighbor `nb.color` — alongside the builtin `co`/`no`/`mask`.
With the optional string the handle binds to that fixed mesh layer; without it
the runtime binds the layer named by the handle itself through
`Brush::attrBindings`. Codegen emits the set as `def.attrs`.

### `save` — the undo-capture set

```sbrush
save vertex co, mask;
save face no;
```

Declares which attributes the CPU undo capture snapshots before a dab. Each name
is either a builtin (`co` / `no` / `mask`) or a declared `attr` handle; `co` is
vertex-domain only. **When a brush declares no `save` at all the default is
`{vertex co, vertex no, face no}`** — so a kernel that writes a custom attribute
must declare it explicitly or its edits won't undo.

### Stages

| Stage | Cadence | Lowering |
|---|---|---|
| `vertex` | per-vertex, parallel | the hot kernel — compute shader on GPU, `vertexIter` loop on CPU |
| `face` | per-face, parallel | the face-domain counterpart; first parameter is `inout Face f` |
| `reduce` | once before `vertex` | scalar block producing `out` values passed into `vertex` by name |
| `host` | once per dab, CPU only | never lowered to a backend; runs natively — param clamps, BVH queries, and computing `ctx` state the vertex stage then reads (`wingscrape` derives its two wing normals this way) |

The `vertex` stage's first parameter is always `inout Vertex v`, the `face`
stage's always `inout Face f`. Parameter directions are `in` (default), `out`,
`inout`.

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

### The `Vertex` and `Face` types

The `inout Vertex v` parameter exposes the per-vertex mesh attributes:

- `v.co` — `float3` position (write to displace the vertex)
- `v.no` — `float3` normal
- `v.mask` — `float` sculpt mask. Read it directly only when a kernel needs the
  raw painted value; to *apply* masking call `masks()`, which folds it in
  together with the automasks (see Builtins)

`inout Face f` in a `face` stage exposes `f.center` and `f.no`. Both bundles also
expose every `attr` handle declared on their domain.

## Types

| Category | Types |
|---|---|
| Scalars | `bool`, `int`, `float` |
| Vectors | `float2`, `float3`, `float4` |
| Aggregate | user `struct`, `Array<T, N>` (fixed size — **field declarations only**, see below) |
| Special | `Vertex` / `Face` (stage parameter only), `void` |

Vector constructors are calls: `float3(0.0, 0.0, 0.0)`. Members are `.x/.y/.z`
(and `.w`), readable and assignable on every backend; `Array<T,N>` is indexed
with `[i]`. (The C++ backend lowers a vector `.x` to `operator[]` since
`litestl::math::Vec` has no named members — an emitter detail, transparent to
kernel authors.) Layout matches `litestl::math` types so WASM-side mirrors are
zero-copy. No pointers, no recursion (WGSL/Vulkan/OpenCL-1.2 constraints).

`Array<T, N>` is accepted **only in a `uniform` / `ctx` field declaration** — the
parser has no array case for locals, parameters, or struct members. `pose` is the
only kernel that uses one.

```sbrush
struct KelvinletState { float a; float b; }     // user struct

ctx Array<float3, 4> poseCageNow;                // fixed array
... poseCageNow[i] ...                           // subscript
```

## Statements and expressions

Standard C-like control flow: `if`/`else`, `for (init; cond; step)`,
`return`, `continue`, blocks, local declarations (`float s = …;`), and
assignment with `= += -= *= /=`. Operators: arithmetic `+ - * /`,
comparison `== != < <= > >=`, logical `&& || !`, bitwise `& | ^` (used by the
flag-testing smooth kernels), unary `-`/`!`.

`continue` in a `vertex` stage skips the current vertex — the idiom for an
early-out when a vertex is outside the brush:

```sbrush
float s = strength(v.co) * masks();
if (s == 0.0) { continue; }
```

### `for_neighbor` — one-ring iteration

Brushes that average over connectivity (smooth, fair) use `for_neighbor`,
which the compiler legalizes per backend (a one-ring walk on CPU; a CSR
indirection buffer preloaded by the dispatcher on GPU):

```sbrush
vertex void apply(inout Vertex v) {
  float s = strength(v.co) * masks();
  if (s == 0.0) { continue; }
  float3 avg = float3(0.0, 0.0, 0.0);
  float n = 0.0;
  for_neighbor (nb in v) {
    avg += nb.co;          // nb exposes co / no / v plus any attr handles
    n += 1.0;
  }
  if (n > 0.0) { v.co += (avg / n - v.co) * s; }
}
```

## Builtins (intrinsics)

Intrinsics are declared in `kernels/ir/intrinsics.cc` with one lowering pattern
per backend; a name with no pattern for the active backend is a compile error.
(The SPIR-V slot is deliberately empty for every intrinsic — SPIR-V is produced
from the WGSL through tint, not emitted directly.) `grad` is the exception: it is
a per-emitter special form rather than a table entry. Current set:

| Intrinsic | Signature | Notes |
|---|---|---|
| `strength(co)` | `float3 → float` | brush falloff strength at a world position — the `strength*falloff` blend (`brush.strength × falloffEval(t)`). Radius is **not** folded in; a kernel that wants radius-proportional displacement multiplies by the `radius` uniform itself (e.g. `draw` does `… * radius * 0.5`). Spatial + scalar **only** — no masking term, so pair it with `masks()` and each factor applies exactly once. Sign-flipped when the brush is inverted (how `mask` erases). Forbidden in an `@unbounded` kernel |
| `masks()` | `→ float` | all per-vertex masking factors: `automasks() × (1 - painted mask)`. The common case — pair it with `strength(co)`. Argument-free on purpose: nothing here depends on position, so the signature cannot silently regrow a falloff |
| `automasks()` | `→ float` | cavity automask × view-normal only, *without* the painted mask. For kernels that themselves **write** `v.mask` |
| `unbounded_window(co)` | `float3 → float` | the C1 cutoff for an `@unbounded` field: 1 inside `0.8R`, smoothstepped to exactly 0 at `R = radius × unboundedExtent` — the same R the host filters spatial nodes against, so the field dies before the region boundary. `extent ≤ 0` disables it (returns 1). Required in every `@unbounded` kernel and forbidden everywhere else (both sema-enforced) |

> **Backend caveat.** CUDA/HIP/OpenCL have no automask binding yet, so
> `automasks()` lowers to a literal `1.0` there and `masks()` degrades to the
> painted mask alone. The formulas above are exact on C++/WGSL/SPIR-V.

In a `face` stage there is no vertex index, so `automasks()` and the painted-mask
term are both identity.
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
  float s = strength(v.co) * masks() * Rings.eval(v.co, surfaceNo);
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
    float s = strength(v.co) * masks();
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
