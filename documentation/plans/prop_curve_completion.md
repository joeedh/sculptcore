# Plan: Flesh out `source/props/prop_curve.h`

Port the structural ideas of path.ux's `Curve1D` into the sculptcore C++ property
system, complete the stubbed curve kinds, and unify the result with the brush
DSL's `Curve1D`/falloff-LUT story. Duplication of the TypeScript is not a goal —
we port concepts (generator registry, control-point storage, bake-to-LUT,
evaluate/derivative/integrate), not the GUI/event/undo machinery, most of which
has no C++ consumer yet.

## 1. Current state assessment

`source/props/prop_curve.h` today:

- `PropCurves` enum: `STEP, LINEAR, SMOOTHSTEP, SHARP, SHARPER, SQRT, BSPLINE,
  GUASSIAN, TABLE` (note the misspelling `GUASSIAN` — kept by path.ux too, see
  open question Q6).
- `CurveGenBase` — virtual `evaluate(double)`, `hash()`, `operator==`. The base
  `evaluate` has a half-written `switch` (only LINEAR/SHARP/SMOOTHSTEP, and SHARP
  there is `pow(f,0.5)` which *disagrees* with `CurveGenSimple<SHARP>` = `f*f*f`).
  The base version is dead for the simple kinds (they override) but is the
  fallthrough for the stubs.
- `CurveGenSimple<type>` — `constexpr`-dispatched analytic kinds. Solid.
- `CurveGenTable` — `Array<double>` LUT, linear interpolation, content hash,
  content equality. This is the existing "baked"/authorable table.
- `CurveGenBSpline`, `CurveGenGuassian` — **empty structs**, no body. They
  inherit `CurveGenBase::evaluate`, so a BSPLINE curve currently returns `f`
  (identity) and a GUASSIAN curve also returns `f`. Both are silently wrong.
- `type_dispatch<Func>` — maps `PropCurves` → concrete type for the templated
  visitor. Drives construction/copy/delete in `CurveGen`.
- `CurveGen` — owning handle (`CurveGenBase*` + type-erased lifetime via
  `type_dispatch`). Move-only-ish (has copy ctor + move ctor).
- `curve_cache.cc` — `get_curve(CurveGen*)` interns by `hash()` + `operator==`,
  returns a `shared_ptr`, dedups identical curves. Hash collisions handled by the
  `operator==` recheck.
- `CurveGenProp : PropBase<CurveGenProp, CurveGenBase*>` — the prop wrapper, with
  `min/max/clamp` and an `evaluate(double)`.

Two bugs to fix while here (cheap, do in slice 1):
- `CurveGenProp::evaluate` clamp is inverted: `std::min(std::max(f, max), min)`
  clamps to `[max, min]` instead of `[min, max]`.
- `CurveGenProp::evaluate` calls `PropBase::get()` which returns
  `const CurveGenBase&` (value_type is the pointer, so `get()` yields
  `const ptr&`); the local is typed `CurveGenBase*` — confirm it compiles against
  the current `PropBase::get` signature and tighten the type.

Callers (grep results, excluding `path.ux/`):
- `prop_dynamics.h` — `DynamicDevice::curve` is a `CurveGenProp`; `evaluate`
  delegates to it. This is the **only real consumer** of `CurveGenProp` today,
  and `Dynamics` itself has no callers wired into the brush pipeline yet.
- `curve_cache.cc`, `props.cc`, `CMakeLists.txt` — plumbing.
- The brush falloff path does **not** use `CurveGenProp` at all — it uses the
  `Brush::falloff_curve` placeholder (see §3).

Conclusion: the prop side is low-traffic. We can reshape it fairly freely; the
binding/serialization surface for curves is not yet load-bearing (Q1).

## 2. Design comparison: path.ux Curve1D vs. the C++ side

path.ux splits a curve into:

- `Curve1D` (facade) — holds *all* generators, an `active` one, `xRange/yRange`,
  `clipToRange`, event callbacks, JSON/nstructjs (de)serialization, GUI draw.
- `CurveTypeData` (base generator) — `evaluate`, numeric `derivative`,
  `derivative2`, `integrate` (uniform quadrature), `inverse` (bisection),
  `calcHashKey`, `equals`, `toJSON/loadJSON`, GUI hooks (`makeGUI/killGUI/draw`).
- Concrete generators: `EquationCurve` (runtime eval — skip), `GuassianCurve`
  (height/offset/deviation), `BSplineCurve` (control points + knots + basis +
  hermite cache + ToolOp-driven editing + GUI).
- `BSplineCache` — interns baked curves by hash key (analogous to our
  `curve_cache.cc`).
- `Ease` — a static library of closed-form easing functions (Penner set).
- `Curve1DProperty` — ToolProperty wrapper (analogue of `CurveGenProp`).

Recommendation per feature:

| path.ux feature | Recommendation |
|---|---|
| `CurveTypeData::evaluate` | **Already have it** (`CurveGenBase::evaluate`). Keep. |
| `derivative` / `derivative2` (finite diff) | **Port** to `CurveGenBase` as virtuals with a default finite-difference body. Cheap, useful for DSL gradient work later, ~15 lines. |
| `integrate` (uniform quadrature) | **Port** as a default `CurveGenBase` virtual. Trivial, used by spacing/arc-length math. |
| `inverse` (bisection) | **Port a stripped version** to `CurveGenBase` default. Needed if falloff inversion ever moves off the `Inverse` preset. Low cost. |
| Generator-registry pattern (dynamic `CurveConstructors`) | **Skip the dynamic registry.** The C++ side already has a compile-time registry via `type_dispatch` + `PropCurves`. Adding a new kind = add an enum value + a `type_dispatch` arm. Keep that; it's simpler and WASM-friendly. |
| `xRange/yRange/clipToRange` | **Port `clipToRange` only**, folded into the existing `CurveGenProp::min/max/clamp` (already present). Domain remapping (xRange != [0,1]) is **skipped** — sculptcore falloff input is always normalized 0..1. |
| Control points + tangent/handle types (`Curve1DPoint`: co/rco/sco/tangent/flag) | **Port a stripped-down version.** Store `co` (Vector2) + `tangent` mode per point. Drop `rco/sco/startco/eid/flag` (those are transform-scratch + selection + GUI state). See §4. |
| B-spline eval (knot vector, basis tables, 2D root-find on x, hermite approx cache) | **Port the math, not the caching layers.** sculptcore needs: control points → evaluate(x). The path.ux trick (2D spline + root-find x) is what makes arbitrary control points work; port that core. The hermite-table fast path is just a bake — we already get that for free via the LUT bake (§3), so **drop the in-generator hermite cache**. |
| `BSplineCache` | **Skip** — `curve_cache.cc` already interns. Reuse it. |
| `Ease` (Penner easing set) | **Skip for v1.** sculptcore's analytic kinds (smoothstep/sharp/sqrt/gaussian) cover current falloff needs. Revisit if a brush wants `backOut`/`elastic`. Note in Q5. |
| GUI hooks (`makeGUI/killGUI/draw/on_mouse*`) | **Skip entirely.** No C++ UI layer exists; the editor will be path.ux's when it ports over (Q5). Keep `CurveGenBase` GUI-free. |
| ToolOp editing ops (add/delete/select/transform point) | **Skip.** Editing happens TS-side; C++ stores/evaluates. |
| Snapshot/restore for undo (`undoPre`/`undo` copy of Curve1D) | **Skip in prop_curve.h.** Undo is the brush/scene layer's concern; `CurveGen` is already copyable, which is all undo needs. |
| `EquationCurve` (runtime `eval()` of a string) | **Skip.** Security/portability nightmare in C++/WASM; no consumer. |
| JSON (de)serialization | **Defer** to whatever serial format props adopts (Q1). Out of scope for completing the math. |

Net: the C++ side stays a thin, GUI-free, compile-time-dispatched evaluator
library. We add the missing math (gaussian, bspline), the numeric helpers
(derivative/integrate/inverse), control-point storage for bspline, and a
bake-to-LUT helper.

## 3. Harmonization with the brush DSL `Curve1D`

The DSL plan (`brush_compute_dsl.md` §Falloffs) says `Curve1D` ships to the GPU
as a 256-entry `Buffer<float>`, and `falloff(x)` is one IR intrinsic that lowers
to LUT-fetch or analytic. path.ux's `Curve1D` is an authorable control-point
object. These reconcile by separating *authoring* from *baking*:

- **Authoring representation = `CurveGen`** (control points + handles + type, or
  an analytic kind). This is the source of truth, lives on the prop/brush, is
  what gets serialized and edited.
- **Baked artifact = a 256-entry `std::array<float, N>` LUT**, regenerated by
  sampling `CurveGen::evaluate` at `N` uniform points. This is what the GPU reads
  and what `falloffEval` consults for the `Curve` kind.

Decisions:

- **Where the LUT lives:** *not* on `CurveGenBase` (analytic kinds shouldn't pay
  for a 1KB array, and the cache interns by curve hash so the LUT would
  duplicate). Introduce a free function

  ```cpp
  // in a new curve_bake.cc / declared in prop_curve.h
  void bake_curve_lut(CurveGenBase &curve, float *out, int n);   // generic
  template <int N> std::array<float,N> bake_curve_lut(CurveGenBase &curve);
  ```

  The owner of a baked curve (the brush) holds the `std::array`. For the brush,
  that array is exactly today's `Brush::falloff_curve`.

- **Who triggers re-bake:** whoever mutates the authoring `CurveGen`. On the
  brush side that's `loadProps`/the `set_falloff_curve` verb. Add a
  `Brush::rebakeFalloff()` that calls `bake_curve_lut` into `falloff_curve`. The
  brush keeps a `props::CurveGenProp falloffCurve` (or a `CurveGen`) as the new
  source of truth; `falloff_curve` becomes its baked output. `falloffEval(t)`'s
  `Curve` branch is unchanged (still reads `falloff_curve`), so the WGSL path and
  the C++ path stay in lockstep.

- **Bit-equality of the baked LUT (cross-backend regression):** the bake must be
  deterministic and identical on host and any backend that bakes (currently only
  host bakes; GPU reads the host-baked buffer). Specify: sample at
  `t_i = i/(N-1)`, `i in [0,N-1]`, store `float(curve.evaluate(double(t_i)))`,
  round-to-nearest (default). Because only the host bakes and ships the buffer,
  the WGSL LUT-fetch is bit-identical to the C++ `Curve`-branch interpolation by
  construction — the existing `test_debug_script.cc` `kind=curve ≈ kind=smoothstep`
  check is the regression guard. Keep N == `kFalloffCurveSize` == 256 so no
  re-tuning of goldens.

- **Retiring `Brush::falloff_curve` the placeholder:** done in two steps (see
  §5). Step A keeps `falloff_curve` as a plain array but populates it via
  `bake_curve_lut(CurveGenSimple<...>)` instead of the hand-inlined loops in
  `setFalloffCurvePreset` — proves the bake matches the placeholder bit-for-bit.
  Step B replaces the `CurvePreset`/`falloff_kind` authoring with a
  `CurveGen falloffCurve`; `falloff_curve` survives only as the baked buffer, and
  `setFalloffCurvePreset` becomes "construct a `CurveGen` of the right kind, then
  rebake". The analytic `falloff_kind` fast-path can stay for the branchless
  GPU/CPU forms (smoothstep/linear/gaussian map to analytic `CurveGen` kinds, so
  `falloff_kind` becomes a cache of "is this curve analytic?" — or is dropped in
  favor of always-LUT; decide in Q3).

## 4. Concrete deliverables

### A. Additions inside `prop_curve.h`

1. Fix `CurveGenBase::evaluate` base body: either make it `= 0` (pure virtual,
   forcing every kind to implement) or correct the SHARP/fallthrough mismatch.
   Prefer pure-virtual now that no kind relies on the base body once the stubs
   are filled.
2. Add `CurveGenBase` virtuals with default bodies (port from
   `curve1d_base.ts`): `virtual double derivative(double)`,
   `virtual double derivative2(double)`,
   `virtual double integrate(double s1, int quadSteps = 64)`,
   `virtual double inverse(double y)`. Finite-difference / bisection defaults;
   analytic kinds may override later (not required v1).
3. **Complete `CurveGenGuassian`**: fields `height=1, offset=1, deviation=0.3`;
   `evaluate(s) = height * exp(-((s-offset)^2)/(2*deviation^2))` (mirror
   `GuassianCurve.evaluate`). Implement `hash()` (mix the three doubles) and
   `operator==` (type + fp-eq the three fields). NOTE: this is a bump/gaussian
   shape centered at `offset`, which differs from `Brush::falloffEval`'s gaussian
   (`exp(-9*(1-t)^2)`). Reconcile in Q4 — likely add a dedicated
   "edge gaussian" parameterization or set defaults so the brush's gaussian is
   expressible.
4. **Complete `CurveGenBSpline`**: port the b-spline core from
   `curve1d_bspline.ts` (control points + knot vector + basis eval + the
   2D-spline-with-x-root-find evaluate). Storage: `Vector<ControlPoint>` where
   `ControlPoint { math::float2 co; uint8_t tangent; }` (stripped `Curve1DPoint`).
   Add `int deg = 3`. Methods: `updateKnots()`, `evaluate(double)` (root-find x,
   return y), `hash()` (mix points + deg), `operator==`. Drop the hermite cache,
   GUI, eid/flag/selection. Provide `loadTemplate(SplineTemplate)` seeding the
   control points from the path.ux `templates` table (CONSTANT/LINEAR/SHARP/
   SQRT/SMOOTH/SMOOTHER/SHARPER/SPHERE/REVERSE_LINEAR/GUASSIAN) so presets carry
   over. Consider its own TU (`curve_bspline.cc`) since it's the largest body —
   keep the class decl in the header, the heavy method bodies in the `.cc`.
5. Declare `bake_curve_lut` (free function + templated convenience) in the header.

### B. New files

6. `source/props/curve_bake.cc` — `bake_curve_lut(CurveGenBase&, float*, int)`
   implementation. (Could live in `curve_cache.cc`; a dedicated TU is cleaner.)
7. `source/props/curve_bspline.cc` — `CurveGenBSpline` heavy method bodies
   (basis, knot build, root-find). Wire both into `source/props/CMakeLists.txt`
   `SRC` list.

### C. Brush-side integration

8. `source/brush/brush.h` — Step A: route `setFalloffCurvePreset` and the
   default-init lambda through `bake_curve_lut(CurveGenSimple<...>{})`. Step B:
   add `props::CurveGen falloffCurve` (or `CurveGenProp`), make `falloff_curve`
   its baked output, add `rebakeFalloff()`. Keep `falloffEval` and the
   `falloff_kind` analytic fast-path (or collapse per Q3).
9. `source/brush/brush_command.h:55/58` — no change required; still calls
   `brush.falloffEval(t)`.
10. `source/debug/script.cc` — `set_falloff_curve preset=...` now constructs a
    `CurveGen` and calls `rebakeFalloff()` (Step B). `set_falloff kind=...`
    unchanged or mapped to choosing an analytic `CurveGen` (Q3).
11. `source/brush/compiler/emit_wgsl.cc` (~530-565) — no semantic change; the
    `falloff_lut: array<f32,256>` binding and `brush_falloff` body still mirror
    the host. Add a comment noting the LUT is now bake-produced from `CurveGen`.

### D. Tests

12. New `source/litestl/tests/test_curve.cc` (or `tests/test_props_curve.cc`) —
    unit tests: each analytic kind's known values; gaussian peak/symmetry;
    bspline through a LINEAR template == identity within eps; `bake_curve_lut`
    of smoothstep matches the old hand-inlined table **bit-for-bit** (locks the
    bake against the placeholder); derivative/integrate sanity. Wire via the
    `test(...)` macro in the appropriate `tests/CMakeLists.txt`.
13. `tests/test_debug_script.cc` — extend the existing curve cases: after Step B,
    `set_falloff_curve preset=smoothstep` then `kind=curve` must still match
    `kind=smoothstep` within fp tolerance, and `preset=inverse` must still
    measurably differ (these already exist — keep them green through the
    refactor; that's the whole point of the bit-identical bake).

## 5. Sequencing (independently landable slices)

1. **Slice 1 — math + bake, zero API churn (the unblocker).**
   `prop_curve.h`: fix the clamp bug + base-`evaluate` mismatch; add
   derivative/integrate/inverse defaults; **fill `CurveGenGuassian`**; add a few
   missing analytic kinds if needed; add `bake_curve_lut` + `curve_bake.cc`. No
   brush changes. Add `test_curve.cc`. This lands the gaussian and the bake with
   no consumer churn and unblocks the brush switchover.
2. **Slice 2 — brush bake passthrough (Step A).** `brush.h`:
   `setFalloffCurvePreset` and default-init go through `bake_curve_lut`. Prove
   `test_debug_script.cc` stays green (bit-identical). No new authoring type yet.
3. **Slice 3 — b-spline implementation.** Fill `CurveGenBSpline` +
   `curve_bspline.cc` + `loadTemplate` + templates table. Unit tests for it.
   Still no brush wiring; standalone-testable.
4. **Slice 4 — brush authoring switchover (Step B).** Replace
   `Brush::CurvePreset`/`falloff_kind` authoring with a `CurveGen falloffCurve`;
   `falloff_curve` becomes the baked buffer; add `rebakeFalloff()`; update
   `script.cc` verbs. Decide Q3 here. Keep regression tests green.
5. **Slice 5 (optional) — numeric polish + presets.** Wire `inverse` to replace
   the `Inverse` preset hack; expose more bspline templates; add curve unit
   coverage for derivative/integrate accuracy.

Slices 1–2 are the high-value, low-risk core. 3–4 are the structural payoff. 5
is gravy.

## 6. Risks / open questions

> **Resolutions (2026-05-24, before implementation):**
> - **Q1** — Defer binding/serialization. Completing the math does not need it;
>   out of scope for this plan (revisit before any curve-persistence UI work).
> - **Q2** — Yes, make `CurveGenBase::evaluate` pure virtual. Every kind
>   implements; the broken base fallthrough goes away.
> - **Q3** — **Keep the `falloff_kind` analytic fast-path.** Smoothstep / Linear
>   / Gaussian stay branchless closed forms on both CPU and WGSL; only the true
>   `Curve` kind bakes + LUT-fetches. Do *not* collapse to always-LUT.
> - **Q4** — Support both gaussian forms via parameters: `CurveGenGuassian`
>   carries height/offset/deviation (centered bump, path.ux form), but ensure the
>   brush's edge-weighted `exp(-9(1-t)^2)` is expressible (pick defaults or a flag
>   so slice 4 can construct it as a `CurveGen`). The brush's analytic Gaussian
>   fast-path is unchanged per Q3.
> - **Q5** — B-spline GUI editor is out of scope; presets/templates drive curves
>   until path.ux's editor ports over.
> - **Q6** — Leave the `GUASSIAN` spelling as-is (single repo, no external API).


- **Q1 — Binding/serialization surface.** Should `CurveGen`/`CurveGenProp`
  participate in the TS binding + serial system (so curves round-trip to disk and
  cross the WASM boundary)? path.ux serializes via nstructjs; sculptcore props
  have their own binding. Completing the *math* doesn't require this, but the
  brush UI will. Decision needed before slice 4 if curves must persist.
- **Q2 — Pure-virtual `evaluate`.** OK to make `CurveGenBase::evaluate` pure
  virtual (every kind must implement)? Cleaner, but changes the base contract.
- **Q3 — Keep `falloff_kind` analytic fast-path or go always-LUT?** Options:
  (a) keep `falloff_kind` for branchless analytic forms and only bake for true
  `Curve`; (b) drop `falloff_kind`, always bake (even smoothstep) and always
  LUT-fetch — simpler, one code path, costs one texture fetch. GPU perf vs.
  simplicity call.
- **Q4 — Gaussian parameterization mismatch.** `CurveGenGuassian` (path.ux:
  centered bump `exp(-(s-offset)²/2σ²)`) vs. `Brush::falloffEval` gaussian
  (edge-weighted `exp(-9(1-t)²)`). Pick one canonical form, or support both via
  parameters. Affects whether the brush's gaussian is expressible as a `CurveGen`
  (needed for slice 4).
- **Q5 — B-spline GUI editor scope.** Confirmed out of scope for now? Plan
  assumes presets/templates drive curves until path.ux's editor ports over and
  edits the authoring `CurveGen` through the binding layer.
- **Q6 — Fix the `GUASSIAN` spelling?** path.ux also misspells it; renaming the
  enum is a trivial but cross-cutting churn. Recommend leaving it for now (single
  repo, no external API) unless doing a broader cleanup.

## 7. Out of scope

- Animation-curve features: keyframes, time tangents, `curve1d_anim.ts`. No
  consumer.
- The in-DSL `Falloff` tagged union (`Spherical/Cube/Linear/Analytic`) — that's a
  separate brush-side slice (DSL Wave 2) that *consumes* `Curve1D` once it exists;
  this plan only delivers the curve + bake it depends on.
- The Vulkan/WebGPU LUT-buffer *dispatch* (binding the storage buffer at draw
  time) — stub until the WGSL dispatcher lands (DSL Wave 3). This plan only
  guarantees the buffer's *contents* are bake-correct and bit-stable.
- path.ux's event system, ToolOps, GUI draw, undo snapshot, `EquationCurve`,
  `Ease` library — none have a C++ consumer.

## Critical files for implementation

- /workspaces/sculptcore/source/props/prop_curve.h
- /workspaces/sculptcore/source/props/curve_cache.cc
- /workspaces/sculptcore/source/brush/brush.h
- /workspaces/sculptcore/source/brush/compiler/emit_wgsl.cc
- /workspaces/sculptcore/tests/test_debug_script.cc
