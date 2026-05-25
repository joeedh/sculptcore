# sbrush Wave 6 — forward-mode autodiff (`grad`)

## Goal
Add `grad(expr, var)` to the DSL: forward-mode gradient of a scalar `expr`
w.r.t. a `float3 var`, returning `float3 = (∂expr/∂var.x, ∂expr/∂var.y,
∂expr/∂var.z)`. Slang-inspired (dual numbers / DifferentialPair) but the dual
state is generated, not a DSL type.

Use: gradient-driven displacement, e.g. `v.co += normalize(grad(field, v.co)) * s`.

## Representation
- scalar → `sbdual { float v; float3 d; }` (d = ∂/∂var)
- float3 → `sbdual3 { float3 v; sb_mat3 j; }` (j = 3×3 Jacobian, columns ∂/∂var.{x,y,z})
- constants/uniforms: d/j = 0; `var` seeds j = identity. Member `.x/.y/.z` picks a row.

## Slices
1. **cpp + parser + IR** — done. `grad(expr, var)` parses as a `Call` (no
   lexer/parser change); `emitExpr` intercepts by name and rewrites the first
   arg inline via `emitDual` (no lambda — WGSL has none), reading back `.d`.
   Dual prelude (duals + ops + chain rules) emitted only when `brushUsesGrad()`.
   Demo brush `graddraw.sbrush`.
2. **wgsl** — done. Same rewrite; ops are `sbd_add/sub/mul/div`/`sbd_neg`
   functions (no operator overloads). tint + spirv-val pass on `graddraw`.
3. **cuda/hip/opencl** — done. Dual prelude in each emitter (`emit_cuda.cc`
   covers cuda+hip with overloaded operators; `emit_opencl.cc` uses `sbd_*`
   functions + an `sb_idx` accessor since OpenCL vectors lack `[]`). All three
   validate `graddraw` (clang device-only for cuda/hip; clspv + spirv-val for
   opencl).
4. **docs** — done. Backend section + Wave 6 marked implemented in
   `brush_compute_dsl.md`.

The forward-mode rewrite shape is identical across all six backends, so the
gradient is bit-identical modulo fp; correctness was confirmed by reading the
emission (`grad(sin(length(p)*40), p)` → `40·cos(40|p|)·p̂`, the analytic
gradient) and by each backend's compile/validate gate. Full debug-app A/B for
`graddraw` is deferred — it is a demo brush, not wired as a `SculptBrushes`
tool, and wiring it adds no coverage beyond the shared-emission guarantee.

Intrinsic derivatives table-driven where possible; min/max/clamp use selection.
