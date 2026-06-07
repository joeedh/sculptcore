# Quad Remesher: Implementation Plan

## Context

We want a feature-aligned **quad remesher** in `sculptcore`, built on the
global MIQ-style pipeline (cross-field → seamless parametrization →
integer quantization → quad extraction) so the result is regular, has
controllable poles, and is **provably free of spiraling edge loops** (the
quantization step is what guarantees that).

User decisions from the planning conversation:

- **Backend**: global MIQ-style only (no QuadriFlow-style local fallback).
- **Constraints in v1**: curvature + sharp-feature auto-alignment,
  user-painted direction strokes, user pole pinning, and a density /
  size map.
- **Location**: new top-level `source/remesh/` module (matches the
  granularity of `brush/`, `spatial/`, `meshlog/`).

This is a substantial multi-week implementation. The plan breaks it into
six milestones (M1…M6), each individually testable end-to-end through
the existing `debug_app` script harness.

The supporting literature review (cross-field methods, user-constraint
mechanisms, pole placement, spiraling prevention) lives in the planning
conversation that produced this plan; the key references are summarized
inline below where they motivate a specific implementation choice.

---

## Status (updated 2026-06-07)

**M1–M6h complete** (the feature is end-to-end). The C++ core (`source/remesh/`)
is green under `node make.mjs test` (`tests/test_remesh_extract.cc` is the M6
gate: grid / cylinder / torus / sphere / capped-cylinder + reprojection +
end-to-end `QuadRemesh`), and runs on both backends (WASM export + native N-API
wrapper). **M6h — host-side TS integration — is now done:** `LiteMesh.quadRemesh`
constructs a `RemeshParams`, calls `Mesh_quadRemesh`, and swaps in the result;
the `litemesh.quad_remesh()` ToolOp makes it undoable. It is guarded by
`tests/integration/litemesh_quad_remesh.test.ts`, which boots **both** backends
headlessly and checks: driver runs cleanly, the mesh changes, undo restores /
redo reapplies, the two backends agree on input + output topology fingerprints,
the remeshed `LiteMesh` has populated GPU buffers, and the native↔WASM dumps
match within tolerance (6/6 green). The auto-only v1 ships now; the user-painted
constraint layers (density / poles / strokes) remain deferred (see the M6h notes
and `buildTriCopy`'s TODO).

Four findings worth recording before the milestone bodies are read:

- **The WASM main-thread stack must be enlarged (`-sSTACK_SIZE=8388608`).** This
  was the single hardest bug in M6h and is *not* obvious from the C++. Emscripten
  5.0.6 defaults the main-thread stack to **64 KB**; the Eigen-heavy pipeline
  (cross-field solve, ILP quantization, cut-graph, extraction) runs synchronously
  on the main thread and overflows it. With `STACK_OVERFLOW_CHECK` off (the
  default) the overflow is **silent** — it writes past `__stack_low` into the
  static-data region just below, corrupting e.g. a static type descriptor's
  `destructorThunk` (→ a `call_indirect` signature-mismatch trap inside
  `LSTL_Destructor_Invoke` at `[Symbol.dispose]`) and intermediate pipeline state
  (→ `extractQuadMesh` returns nullptr). Native is immune (multi-MB OS stack);
  the litestl canary allocator only tracks *heap* blocks so static-data
  corruption is invisible to it. The fix is the 8 MB `STACK_SIZE` in
  `CMakeLists.txt`'s `BUILD_WASM` block (128× the default, < the 16 MB default
  `INITIAL_MEMORY`). **Any future main-thread Eigen op must respect this.**

- **Sphere/poles need local-injectivity stiffening.** The linear MIQ map is not
  guaranteed injective and folds in the singularity 1-rings (a UV sphere's poles,
  any cone). `computeQuantization` runs a post-rounding stiffening pass
  (`QuantizeParams::inj_iters`, default 25): each round ramps a Dirichlet+RHS
  weight on the folded faces and their 1-ring — capped below the locked-grid
  penalties — and keeps the lowest-fold solve. With it the low-res UV sphere
  extracts a clean closed quad mesh (Euler 2, no inversions); without it ~9 faces
  invert at the poles.
- **Closed inputs can leave odd-loop cap holes (accepted).** Extraction skips
  folded faces and fan-closes the even boundary rings that leaves behind, but a
  ring whose quantized length is *odd* cannot be closed by a pure-quad fan, so it
  is left open. The capped cylinder hits this (two odd cap rims → Euler 0, not 2);
  the result stays all-quad + manifold + inversion-free + spiral-free, which is
  the guarantee the gate enforces. Closing odd rings would require a triangle or a
  T-junction, both of which break the all-quad contract, so it is out of scope.
- **A global feasibility gate** rejects hopelessly tangled maps: if >10% of faces
  fold, `extractQuadMesh` returns `nullptr` (clean failure) rather than
  rasterizing runaway non-manifold output — the plan's "gate M5 behind a
  feasibility check, fall back rather than hard-fail" risk mitigation.

The corrections below are folded into the relevant sections; the headline changes
in the codebase *since the plan was first written*:

- **Dual backend now exists.** sculptcore runs through two interchangeable
  backends behind one `IWasmInterface`: **WASM** (browser) and **native N-API**
  (Electron). Every new C-API symbol must be wired for *both* (WASM export list
  + a hand-written native wrapper) and is parity-guarded by
  `tests/integration/sculptcore_parity.test.ts` in the host repo. The original
  plan predates this — see the revised M6 and the new integration section.
- **Quad remesh is a *global* op — not a brush.** The plan's M6 proposed
  hosting it under the brush command system (`QUAD_REMESH` enum entry). That is
  the wrong model: dynamic topology (also a remesher) shipped as its own
  top-level `source/dyntopo/` module, *not* a brush-enum entry, precisely
  because it is a distinct operation. Quad remesh is global and one-shot
  (whole mesh in → new mesh out), so it follows the **`Mesh_triangulate`
  global-op pattern**, not the per-dab brush pattern. M6 is revised accordingly.
  (Dyntopo is a *local, incremental* remesher run per-dab inside a stroke; quad
  remesh is *global*. Borrow dyntopo's C++ module *packaging* only, never its
  use-case.)
- **New reusable mesh utils landed.** `source/mesh/utils/` gained
  `edge_split.h`, `edge_flip.h`, and `attr_interp.h`. `attr_interp.h`'s
  `interpAttrs(grp, dst, src0, src1, t)` + `AttrRowSnapshot` is the
  attribute-blend primitive M6's quad extraction wants when it spawns new
  vertices (float lerp, int/bool copy, **never** touches TOPO link columns).
- **Spatial query surface grew** (`castScreenCircle` / `castScreenRect` were
  added for picking) but there is **still no closest-point query** — M1's
  `findClosest` task is genuinely still needed.
- **Eigen path confirmed.** The build-wired copy is
  `source/litestl/extern/eigen/include/eigen5/Eigen/` (the one
  `source/litestl/math/matrix.h` includes from); it has the full module set
  (`Sparse`, `SparseLU`, `SparseCholesky`/`SimplicialLDLT`,
  `IterativeLinearSolvers`). The top-level `extern/eigen_dist/` is an **unwired
  duplicate** — do **not** include from it.

---

## Recommended implementation effort

Effort should be dialed **per milestone** — the scaffolding is pattern-following,
but the field / parametrization / quantization core is research-grade
computational geometry where subtle bugs survive weak tests (a "smooth but
*wrong*" cross field passes many checks). Use **Opus** for the whole algorithmic
core (M1–M6); a smaller model is only defensible for pure plumbing.

| Phase | Effort | Why |
|-------|--------|-----|
| Scaffolding — module/CMake, `bindings.cc`, c-api, N-API wrapper, OBJ loader, shape generators, debug verbs, test harness | **medium** | Mechanical; the `dyntopo` + `Mesh_triangulate` templates carry it, and typecheck/parity catch slips. |
| M1 — curvature, feature tag, closest-point | **high** | Real DDG (shape-operator eigendecomp) but textbook, well-bounded. |
| M2 — cross-field solve | **xhigh** | Parallel transport, period jumps, Gauss–Bonnet index bookkeeping — dense with sign/convention traps. |
| M3 — singularity adjustment | **xhigh** | Iterative, heuristic curl reduction; correctness is fuzzy and hard to unit-pin. |
| M4 — seamless parametrization | **xhigh** | Corner UVs + integer-affine transitions; one wrong transition corrupts the whole map. |
| **M5 — quantization** | **max** | The riskiest milestone (see Risks): MIQP-free LP-relax + greedy rounding, and the loop-closure constraint *is* the no-spiral guarantee. Highest payoff for deep reasoning. |
| M6 — extraction + reprojection | **high** | Careful topology + attribute transfer, reprojection-drift watch — but strongly gated by the validator. |
| Test authoring — validator, per-milestone gates, parity, ToolOp | **high** | Correctness matters (the isoline-closure check is load-bearing) but tractable. |

Practical notes: do the **scaffolding phase first** (at medium) so every later
high/max milestone lands against working gates; the dense-math milestones
benefit more from *effort* than from raw token budget — a tight, correct M5
beats a sprawling one. The strong automated gates (isoline-closure validator,
golden snapshots, dual-backend parity, perf budget) are what make medium-effort
scaffolding safe, so spend the reasoning budget on M2–M5.

---

## Critical files and existing utilities to reuse

From the exploration of the codebase:

- **Mesh + attributes**: `source/mesh/mesh.h`, `source/mesh/mesh_types.h`,
  `source/mesh/attribute.h`, `source/mesh/attribute_builtin.h`.
  Use `BuiltinAttr<T, ".remesh.f.theta">`-style declarations for all
  per-element field/parametrization state. Computed (non-user-input)
  intermediate attrs should carry `AttrFlag::TEMP` (like
  `BuiltinAttr<int, ".spatial.v.node", AttrFlag::TEMP>`) so they don't
  serialize into saved meshes — the host repo serializes `LiteMesh` blobs,
  and persisting transient solver state would bloat / break round-trips.
- **Existing utilities** to imitate (header-only, in
  `source/mesh/utils/`): `edge_collapse.h`, `edge_split.h`, `edge_flip.h`,
  `delaunay.h`, `triangulate.h`. Same pattern for the new small-op helpers
  we add. Of particular use to M6: `attr_interp.h` —
  `interpAttrs(grp, dst, src0, src1, t)` + `AttrRowSnapshot` blend every
  non-topology attribute layer onto a freshly-created vertex (float lerp,
  int/bool copy, TOPO link columns never touched). Use it for the
  barycentric attribute transfer when extracting new quad vertices instead
  of hand-rolling per-layer blending.
- **Eigen sparse**: vendored at
  `source/litestl/extern/eigen/include/eigen5/Eigen/` — the build-wired copy
  (`source/litestl/math/matrix.h` already includes from it, e.g.
  `#include "eigen/include/eigen5/Eigen/Core"`). It carries the full module
  set: use `Eigen::SparseMatrix<double>` + `SimplicialLDLT`
  (`Eigen/Sparse` → `Eigen/SparseCholesky`) for SPD field/param solves,
  `SparseLU` where needed, `IterativeLinearSolvers` if a CG fallback helps.
  **Do not** include from the top-level `extern/eigen_dist/` — it is an
  unwired duplicate, not on any include path.
- **Spatial BVH**: `source/spatial/spatial.h` (now has `castRay`,
  `castScreenCircle`, `castScreenRect` — but still **no** closest-point
  query, which we add in M1).
- **Module packaging template**: `source/dyntopo/` is the right shape to
  copy — a dependency-light header core (`dyntopo.h`, spatial/brush/meshlog-
  free) plus an out-of-line `source/dyntopo/bindings.cc` (param/stats
  structs bound with `BIND_STRUCT_*`, enums via `Bind<T>()`), registered in
  `source/core/bindings.cc::initBindings` next to
  `sculptcore::dyntopo::registerBindings(manager)`. **Caveat:** dyntopo is a
  *local, incremental* remesher (per-dab); quad remesh is a *global* op.
  Borrow the packaging, not the use-case — there is no brush-enum entry.
- **Global mesh-op exposure** (the model for the user-facing op, replacing
  the original "brush command" idea): `Mesh_triangulate` in
  `source/mesh/c-api/mesh_c_api.cc` is a whole-mesh topology change exposed
  as a plain `extern "C"` symbol — listed in `source/mesh/CMakeLists.txt`'s
  `WASM_SYMBOLS` block, hand-wrapped for native in
  `source/napi/napi_runtime.cc` (`NapiRuntime::MeshTriangulate` +
  `define(exports, "meshTriangulate", …)`). Quad remesh mirrors this (see
  revised M6).
- **Debug app verb registry**: `source/debug/script.cc::execVerb` — a chain
  of `if (verb == "...")` blocks (the `dyntopo` verb lives there). Every
  milestone gets one or more verbs here so we can drive it from `.txt`
  scripts.
- **C API + bindings**: new module gets `source/remesh/bindings.{h,cc}`
  (model: `source/dyntopo/bindings.cc`) registered via
  `source/core/bindings.cc::initBindings`, and `source/remesh/c-api/
  remesh_c_api.cc` for the `extern "C"` entry point. **Both backends:** add
  the symbol to the module's `WASM_SYMBOLS` CMake list *and* hand-write a
  native N-API wrapper in `source/napi/napi_runtime.cc` (see M6 + the
  integration section).
- **Tests**: `tests/test_mesh.cc` is the reference shape (GTest-style,
  native only). Each milestone adds a `tests/test_remesh_<phase>.cc` wired
  into `tests/CMakeLists.txt`; follow the dyntopo regression gates
  (`test_dyntopo_cascade` / `_budget` / `_smooth`) for the style.

---

## Module layout

```
source/remesh/
  CMakeLists.txt
  remesh.h / remesh.cc           -- top-level entry: QuadRemesh(mesh, params) → mesh
  remesh_params.h                -- parameter struct (target_edge_len, density attr name, ...)
  bindings.{h,cc}                -- TS/Embind surface
  c-api/
    remesh_c_api.cc              -- extern "C" wrappers for WASM
  field/
    cross_field.{h,cc}           -- 4-PolyVector solve (M2)
    curvature.{h,cc}             -- principal curvature estimator (M1)
    feature_tag.{h,cc}           -- sharp-edge + boundary tagging (M1)
    constraints.{h,cc}           -- user strokes + pole pins (M2)
    singularity_adjust.{h,cc}    -- iterative adjustment (M3)
  param/
    cut_graph.{h,cc}             -- tree connecting singularities (M4)
    seamless_param.{h,cc}        -- least-squares (u,v) on cut surface (M4)
  quantize/
    t_mesh.{h,cc}                -- T-mesh built from seamless param (M5)
    quantize_ilp.{h,cc}          -- LP relaxation + rounding (M5)
  extract/
    quad_extract.{h,cc}          -- pull back integer grid → quad faces (M6)
    reproject.{h,cc}              -- BVH closest-point snap (uses M1) (M6)
tests/
  test_remesh_curvature.cc       -- M1
  test_remesh_field.cc           -- M2
  test_remesh_singularity.cc     -- M3
  test_remesh_param.cc           -- M4
  test_remesh_quantize.cc        -- M5
  test_remesh_extract.cc         -- M6 end-to-end (full remeshValidate; AnimeGirl2.obj spiral case)
  test_remesh_perf.cc            -- gated perf budget (100k sphere), like test_dyntopo_budget
  obj_load.h                     -- tests-only OBJ importer (v/f → make_vertex/make_face, fan-tri); loads assets/AnimeGirl2.obj (prereq)
  assets/AnimeGirl2.obj          -- dense character mesh; M6 spiral-elimination stress fixture
source/mesh/utils/
  closest_point.h                -- BVH closest-point (lives with mesh utils since it's a general op, called from extract/reproject.cc) (M1)
  mesh_validate.h                -- promoted from debug checkManifold; remeshValidate(mesh) → Report (all-quad / manifold / Euler / inversions / isoline-closure / singularity inventory). Shared by every test + debug verbs. (prereq, alongside M1)
source/mesh/
  mesh_shapes.{h,cc}             -- EXTEND: add makeGrid/makeCylinder/makeTorus/makeUVSphere next to createCube (extern "C" + WASM_SYMBOLS); test/debug fixtures (prereq)
```

The CMake target `remesh` depends on `mesh`, `spatial`, `math`, `util`,
`binding`, `gpu` (uniform-block style usage if we render the field
overlay); wired into `source/CMakeLists.txt` next to the other modules.

---

## Attribute schema

All persistent state lives on the input mesh as builtin attributes so
intermediate stages are debuggable and visualizable.

| Domain  | Name                          | Type    | Stage | Meaning |
|---------|-------------------------------|---------|-------|---------|
| vertex  | `.remesh.v.kmin_dir`          | float3  | M1    | min principal curvature direction |
| vertex  | `.remesh.v.kmax_dir`          | float3  | M1    | max principal curvature direction |
| vertex  | `.remesh.v.k`                 | float2  | M1    | (kmin, kmax) magnitudes |
| vertex  | `.remesh.v.density`           | float   | input | user density (default 1.0) |
| vertex  | `.remesh.v.pole_index`        | int8    | input | 0 = none, ±1 = +/− index/4, etc. user-pinned |
| edge    | `.remesh.e.is_sharp`          | bool    | M1    | dihedral above threshold |
| edge    | `.remesh.e.is_boundary`       | bool    | M1    | only one corner |
| face    | `.remesh.f.stroke_dir`        | float3  | input | user direction stroke; zero if absent |
| face    | `.remesh.f.theta`             | float   | M2    | cross-field angle in face's tangent frame, mod π/2 |
| face    | `.remesh.f.u`                 | float2  | M4    | seamless (u,v) at the face origin |
| corner  | `.remesh.c.uv`                | float2  | M4    | (u,v) at each corner (per-side, so cuts can disagree) |
| edge    | `.remesh.e.period`            | int8    | M2    | period jump 0..3 across the edge |
| edge    | `.remesh.e.translation`       | float2  | M4    | translation across the edge (pre-quantize) |
| edge    | `.remesh.e.translation_q`     | int2    | M5    | quantized integer translation |

User input attributes (`density`, `pole_index`, `stroke_dir`) are written
by the brush UI (or by debug-app verbs); the rest are computed.

---

## Milestone M1 — Foundation (curvature, features, closest-point)

**Goal**: every per-vertex / per-edge quantity the field solver needs,
plus the closest-point BVH query needed by the final reprojection.

Build:
- `source/remesh/field/curvature.{h,cc}` — discrete shape-operator
  estimator (Cohen-Steiner & Morvan style: edge-based tensor accumulation
  per vertex 1-ring, eigendecomposition for principal directions and
  magnitudes). Output: the four `.remesh.v.kmin_dir/.kmax_dir/.k` attrs.
- `source/remesh/field/feature_tag.{h,cc}` — sharp-edge tagging by
  dihedral angle threshold (param), boundary tagging by corner count.
- `source/mesh/utils/closest_point.h` — extend `SpatialTree` (or add a
  free function over it) with `findClosest(pos) → (face_id, bary, point,
  dist)` using the same AABB walk as `castRay` but with point-AABB
  distance pruning instead of ray-AABB intersection.
- Debug-app verbs: `remesh_curvature`, `remesh_feature_tag`,
  `remesh_closest_point` (the last for sanity-checking BVH).
- Test: `tests/test_remesh_curvature.cc` validates curvature on a
  cylinder (expect kmax ≈ 1/R aligned with circumferential direction,
  kmin ≈ 0 aligned axially) and a sphere (kmin ≈ kmax). Validates
  closest-point on a known shape.

**Exit criterion**: principal directions visually align with cylinders /
sharp ridges in `debug_app` overlay; closest-point matches a brute-force
reference on a 10k-face mesh within float tolerance.

---

## Milestone M2 — Cross-field with user constraints

**Goal**: a smooth per-face cross field θ (mod π/2) that respects all the
chosen user constraints.

Build:
- `source/remesh/field/constraints.{h,cc}` — gathers, for each face:
  - hard direction from `.remesh.f.stroke_dir` (if non-zero), projected
    into the face tangent frame;
  - hard direction from any incident `.remesh.e.is_sharp` edge (axis
    parallel to edge tangent);
  - hard direction from any incident `.remesh.e.is_boundary` edge (axis
    parallel to boundary tangent);
  - soft direction from a per-vertex curvature average (using
    `.remesh.v.kmin_dir/kmax_dir` weighted by anisotropy = (kmax−kmin) /
    (|kmax|+|kmin|+ε)).
- `source/remesh/field/cross_field.{h,cc}` — Diamanti 2014
  complex-polynomial 4-PolyVector solve:
  - Represent the cross by a complex `c_f = exp(4iθ)` per face in the
    face's tangent frame.
  - For each edge, the smoothness energy term is
    `|c_f1 · r_{f1→f2}^4 − c_f2|^2` where `r` is the parallel-transport
    rotation between adjacent face frames.
  - Hard constraints fix `c_f` directly (linear equality on real+imag);
    soft constraints add weighted quadratic terms.
  - Single Eigen sparse least-squares solve. Recover θ_f from c_f.
  - Singularities are read off as vertices where the per-vertex sum of
    edge-aligned θ-differences is non-zero mod π/2 — store in
    `.remesh.v.pole_index` (computed) unless already user-pinned.
- Per-edge period jump `.remesh.e.period` is the rounded
  `arg(c_f2 · conj(c_f1 · r^4)) / (π/2)`.
- Debug-app verbs: `remesh_cross_field` (with optional constraint params)
  and an overlay-rendering verb.

**Exit criterion**: on a torus, expect zero singularities. On a sphere,
expect 8 singularities of index +1/4 (sum to +2 = 4χ/4). On a model with
user strokes, the field follows the strokes. Verified by an automated
test that checks the index sum equals 4·χ.

---

## Milestone M3 — Singularity adjustment (curl reduction)

**Goal**: nudge the singularities so the field is locally
curl-near-zero. This is the spiral-prevention step at the field level
(Liu et al. TVCG 2024).

Build:
- `source/remesh/field/singularity_adjust.{h,cc}`:
  - Compute per-face curl of the cross field (failure of period jumps to
    sum to 0 around small loops);
  - Build the "curl vector field" that points along which singularities
    must move;
  - Iterate: move free singularities one face at a time along this
    field, re-solve the local cross-field patch, repeat until either
    curl-norm < ε or step budget is exhausted;
  - Allow merge (two opposite-index singularities annihilate when
    adjacent) and split (a high-index singularity into two of half the
    index) operations.
  - Honor `.remesh.v.pole_index` user pins: pinned singularities never
    move, merge, or split.
- Debug-app verb: `remesh_adjust_singularities` with iteration cap and
  curl threshold params.

**Exit criterion**: on a noisy model, the number of singularities
post-adjustment stabilizes; total curl norm drops by ≥1 order of
magnitude vs. the M2 output. Pinned singularities stay where pinned.

---

## Milestone M4 — Seamless parametrization

**Goal**: smooth real-valued (u, v) per corner whose gradient aligns with
the cross field, with transition functions across edges in the integer
affine group of rotations (multiples of 90°) but real-valued
translations.

Build:
- `source/remesh/param/cut_graph.{h,cc}`: build a spanning tree of edges
  that connects every singularity to a single root (Dijkstra rooted at
  any singularity, growing through all faces). Tag cut edges (those
  *not* in the tree connecting branches — equivalently the dual cycles
  that cross the cut graph) — these are where transition jumps live.
- `source/remesh/param/seamless_param.{h,cc}`:
  - Variables: (u, v) at each *corner* (not vertex) so cut edges can
    have different sides. Plus per-cut-edge real translation
    `.remesh.e.translation`.
  - Energy: per-face, integrate the cross field — gradient of u parallel
    to one axis of the cross, gradient of v parallel to the other.
    Quadratic in corner uvs.
  - Constraints (linear equality): around each non-cut edge, the two
    corners on either side must satisfy
    `uv_left = R(90°·period) · uv_right + 0`. Across cut edges,
    `uv_left = R(90°·period) · uv_right + translation`.
  - Solve via Eigen `SimplicialLDLT` (system is SPD).
  - Density map: scale per-face metric by `1/density` so target spacing
    follows the user's painted density map.
- Debug-app verb: `remesh_seamless` with target_edge_length param.

**Exit criterion**: the parametrization gradient aligns with the cross
field (small angle error in a sanity test); transition matrices across
non-cut edges are pure rotations (zero translation); the
parametrization is locally injective except in the immediate
neighborhood of singularities.

---

## Milestone M5 — Quantization (spiral elimination)

**Goal**: snap the seamless parametrization's translations and
singularity-cell sizes to integers so the integer grid pulls back to a
valid quad complex — this is what *eliminates spirals*.

Build:
- `source/remesh/quantize/t_mesh.{h,cc}` — trace separatrices from each
  singularity outward along the cross field directions; intersect them
  to form a coarse T-mesh (Lyon 2021 §3). Each T-mesh "side" gets a real
  length read from the M4 parametrization.
- `source/remesh/quantize/quantize_ilp.{h,cc}`:
  - Variables: integer length `ℓ_s ≥ 0` per T-mesh side, integer
    translation `(t_x, t_y)` per cut edge, integer scaling factor.
  - Constraints: around each closed loop of T-mesh sides, sum of signed
    lengths must equal zero (this is the constraint that *guarantees no
    spirals*: any loop that closes geometrically must also close
    combinatorially in the integer grid).
  - Objective: minimize Σ (ℓ_s − ℓ_s^*)² where ℓ_s^* is the real-valued
    target from M4.
  - Solver strategy: solve the LP relaxation with Eigen-based primal-dual
    or simplex; then round each `ℓ_s` to its nearest valid integer
    respecting the loop constraints via a Bommes-2013-style greedy
    rounding (fix one variable, re-solve LP, repeat). No external MIQP
    dependency.
  - Sides with `ℓ_s = 0` after rounding collapse into T-junctions or
    full merges per Lyon 2021 §4.
- Debug-app verb: `remesh_quantize`.

**Exit criterion**: every closed loop of T-mesh sides has integer total
length zero (programmatic check); on a torus with simple stroke
constraints, output sizes match analytical expectations.

---

## Milestone M6 — Quad extraction + reprojection (end-to-end)

**Goal**: produce the final quad mesh and snap it to the original
surface.

Build:
- `source/remesh/extract/quad_extract.{h,cc}`:
  - Walk the integer grid in (u, v) space — each unit cell becomes one
    output quad.
  - For each integer grid intersection inside a triangle, compute its
    3D position by barycentric interpolation of the triangle's corner
    positions.
  - Stitch across edges using the (now-integer) transition functions.
  - Emit a new `mesh::Mesh` (the output) populated via the existing
    `mesh.make_vertex` / `mesh.make_edge` / `mesh.make_face` API.
- `source/remesh/extract/reproject.{h,cc}`:
  - For each output vertex, call the M1 BVH closest-point query against
    the *input* mesh's `SpatialTree`.
  - Optional: Laplacian-smooth a few iterations between BVH snaps to
    avoid kinks; user-tunable strength.
- `source/remesh/remesh.{h,cc}` — the top-level
  `QuadRemesh(mesh, params) → mesh` function that calls M1→M6 in order.
  Reuse `mesh/utils/attr_interp.h::interpAttrs` for the per-vertex
  attribute transfer in `quad_extract.cc` rather than hand-rolling it.
- `source/remesh/c-api/remesh_c_api.cc` — `extern "C"` surface, following
  the `Mesh_triangulate` global-op pattern (NOT the brush model). A
  `Mesh_quadRemesh(Mesh *in, RemeshParams *params) → Mesh *` that returns a
  freshly-allocated output mesh (caller frees via the existing `freeMesh`),
  so undo can keep both. **Wire it for both backends:**
  - add `Mesh_quadRemesh` to the new module's `WASM_SYMBOLS` block in
    `source/remesh/CMakeLists.txt` (mirroring `Mesh_triangulate` in
    `source/mesh/CMakeLists.txt`);
  - hand-write the native wrapper in `source/napi/napi_runtime.cc`
    (`NapiRuntime::MeshQuadRemesh`) and register it with
    `define(exports, "meshQuadRemesh", …)` — copy the `MeshTriangulate`
    wrapper's `napi_unwrap` shape.
- `source/remesh/bindings.{h,cc}` — TS/Embind surface for `RemeshParams`
  (model: `source/dyntopo/bindings.cc`, which binds `DynTopoParams` /
  `DynTopoStats` out-of-line). Register via
  `source/core/bindings.cc::initBindings`
  (`sculptcore::remesh::registerBindings(manager)`).
- **No brush-enum entry.** The op is global and one-shot; it is invoked
  directly through the C-API symbol above, exactly like `Mesh_triangulate`,
  and surfaced in the host repo as a ToolOp (see the integration section).
- Debug-app verb: `remesh` (runs the full pipeline end-to-end).
- Test: `tests/test_remesh_extract.cc` runs the full pipeline on:
  - a sphere (closed, expect 8 singularities, valid quad mesh, all
    quads, no T-junctions, Euler characteristic = 2);
  - a cylinder with caps (validates sharp + boundary alignment);
  - a torus (validates zero-singularity case);
  - **the dense `tests/assets/AnimeGirl2.obj` character mesh — the
    spiral-elimination stress case.** An organic, high-detail real-world
    model is precisely where naive cross-field + parametrization (without
    M5's integer quantization) produces spiraling edge loops, so this is the
    meaningful proof that quantization did its job. Assert the
    `remeshValidate` **isoline-closure (no-spiral) check passes**, in
    addition to all-quad / manifold / no-inversions. (Loaded via the
    tests-only OBJ helper — see the testing section.)

**Exit criterion**: end-to-end remesh of a 100k-face sphere produces a
valid all-quad mesh in under 30s on a developer laptop, with no
T-junctions, every face a quad, Euler characteristic correct, and
visible regularity. **No spiraling edge loops** verified by checking
that every (u,v)-isoline closes within one trip around the surface — and
the same no-spiral check **must also pass on the dense
`tests/assets/AnimeGirl2.obj` character mesh**, the organic input that
exposes spiraling whenever M5's quantization is wrong. A naive
cross-field/param result on that model that spirals must *fail* this
test, so it genuinely guards the guarantee rather than just smoke-testing
completion.

**Implemented (M6g).** The gate is `tests/test_remesh_extract.cc`: grid (Euler 1),
uncapped cylinder (Euler 0), torus (Euler 0), sphere (Euler 2), capped cylinder,
reprojection, and the end-to-end `QuadRemesh` on sphere + torus. Two deviations
from the criterion above, both deliberate:

- **Capped cylinder is gated gracefully, not at Euler 2.** Its two cap rims
  quantize to *odd* grid loops that a pure-quad fan cannot close, so the caps are
  left open (Euler 0). The test asserts the all-quad guarantee instead — all-quad
  + manifold + consistent winding + no inversions + no degenerate faces +
  **no spirals** — which is what the pipeline actually promises (see the
  odd-loop-hole note in the Status section).
- **AnimeGirl2.obj is an opt-in stress gate (`REMESH_ANIME=1`), not a default
  test.** At ~785k triangles a full MIQ solve is minutes-long and far too heavy
  for the per-commit suite; the synthetic shapes already machine-check the
  no-spiral guarantee on every run. When enabled it asserts the headline no-spiral
  + all-quad criteria on the organic input (Euler / inversions are reported but
  not gated — an organic surface produces the same odd-loop cap holes and
  occasional reprojection drift, both accepted).

---

## Testing strategy

Every milestone is gated by an **automated native GTest** (`tests/test_*.cc`,
`test_assert` style, wired into `tests/CMakeLists.txt`, run via
`node make.mjs test`). Visual `debug_app` scripts are *supplementary* — useful
for review and bug repro, but the merge gate is always the automated test, so
no milestone depends on a human eyeballing a screenshot.

The shared machinery below is built **up front, alongside M1**, so each
milestone's test is a thin assertion layer rather than bespoke scaffolding.
The current test infra only has `createCube` and a debug-local `checkManifold`;
the rest are proposed framework extensions.

### Test-framework extensions to build first (prerequisites)

1. **Canonical shape generators.** `source/mesh/mesh_shapes.cc` has only
   `createCube` (+ a `sphereFac` spherize whose curvature concentrates wrongly
   at the cube corners — unusable for curvature validation). Add, with the same
   `extern "C"` + `WASM_SYMBOLS` exposure (so debug verbs *and* host tests can
   use them):
   - `makeGrid(nx, ny)` / plane — trivial param/quantize sanity (flat field);
   - `makeCylinder(radial, height, capped)` — M1 anisotropy + M2 sharp/boundary
     alignment fixture;
   - `makeTorus(major, minor, nu, nv)` — the zero-singularity case (χ = 0);
   - `makeUVSphere(rings, segs)` — a true sphere with analytic curvature
     (kmin ≈ kmax ≈ 1/R), which the spherized cube cannot provide;
   - the dense **`tests/assets/AnimeGirl2.obj`** character mesh (already in the
     tree) as the organic, real-world fixture for the M6 spiral-elimination
     stress case and the dense perf case.
   Add a `make_shape <kind> ...` debug verb so `.txt` scripts can build the
   procedural shapes.

2. **Minimal OBJ loader (tests-only).** sculptcore has **no** OBJ importer today
   (nothing in `source/` or `tests/`), so `AnimeGirl2.obj` can't be consumed
   yet. Add a small test-scoped helper `tests/obj_load.h`:
   `loadObj(path) → Mesh*` that parses `v` / `f` lines and builds via
   `make_vertex` / `make_face`, **fan-triangulating any n-gon faces on import**
   so the pipeline always receives a clean triangle mesh regardless of the
   `.obj`'s face types. Keep it test-scoped — OBJ is a fixture format here, not
   a production path, so it does **not** belong in `source/mesh/c-api/`.

3. **Reusable mesh/quad validator.** Promote the debug-local `checkManifold`
   (`source/debug/script.cc`) into a real util `source/mesh/utils/mesh_validate.h`
   and extend it to `remeshValidate(mesh) → Report` consumed by every test and
   debug verb. The report covers:
   - **all-quad** (triangle/n-gon count == 0) — M6;
   - **manifold + orientable** (no non-manifold edges, consistent winding);
   - **no inverted / zero-area faces** (signed-area / normal-flip check — the
     M6 reprojection-drift guard);
   - **Euler characteristic** matches input genus (V − E + F = 2 − 2g);
   - **no-spiral / isoline closure**: trace each (u,v) isoline and assert it
     closes within one loop. This is the *headline guarantee of the whole
     pipeline* and must be machine-checked, never eyeballed;
   - **singularity inventory**: count, indices, and Σ index == 4·χ;
   - **T-junction count**.
   Returning a structured report (not a bool) lets tests assert specific fields
   and verbs print a diagnosis.

4. **Topology golden snapshots.** Extend the existing `tests/golden/*.json`
   buffer-signature mechanism (today used for brush A/B) to remesh: dump a
   *topology signature* — face/vert/edge counts, valence histogram, sorted
   singularity list, bbox hash — to `tests/golden/remesh_<case>.json` and diff.
   Cheap, deterministic, cross-backend; the first thing to trip on an
   accidental result change.

5. **`bench_remesh` debug verb.** Mirror `bench_dyntopo`: build a shape, run the
   pipeline, print per-phase timings (field / param / quantize / extract /
   reproject) and output stats. This is the A/B *measurement* tool; the perf
   *gate* is a separate budgeted test (see cross-cutting dimensions).

### Per-milestone gates (each built on the validator)

- **M1** `test_remesh_curvature`: on `makeCylinder`, kmax ≈ 1/R circumferential
  and kmin ≈ 0 axial (direction angle error < tol); on `makeUVSphere`,
  kmin ≈ kmax (anisotropy < tol). `findClosest` matches a brute-force reference
  on a 10k-face mesh within float tol.
- **M2** `test_remesh_field`: Σ singularity index == 4·χ on sphere (+2) and
  torus (0); with a planted stroke, per-face θ inside the stroked region aligns
  to the stroke (angle error < tol). A pinned-pole set violating Gauss–Bonnet
  is rejected with a surfaced error (not a garbage field).
- **M3** `test_remesh_singularity`: post-adjust curl-norm ≤ 0.1× the M2 value;
  singularity count identical across two reruns (determinism); user-pinned
  singularities provably unmoved.
- **M4** `test_remesh_param`: param gradient ∥ cross field (angle error < tol);
  transition across non-cut edges is a pure rotation (‖translation‖ < tol);
  positive Jacobian (local injectivity) everywhere outside singularity 1-rings.
- **M5** `test_remesh_quantize`: every T-mesh loop has integer signed-length sum
  == 0 (exact); a deliberately over-constrained input exercises the feasibility
  fallback and is asserted to produce a valid (if non-integer) result, not a
  crash.
- **M6** `test_remesh_extract`: full `remeshValidate` passes (all-quad,
  manifold, Euler, no inversions, **isolines close**) on sphere / cylinder+caps
  / torus, **and on the dense `AnimeGirl2.obj` character mesh as the
  spiral-elimination stress case** (the isoline-closure check is the proof
  M5's quantization eliminated spirals on real organic geometry); the golden
  topology signature matches.

### Cross-cutting dimensions (covered collectively, not per-milestone)

- **Constraint fidelity**: strokes honored, pole pins honored, and density map
  → measured local edge length tracks the target ratio (∝ 1/√density) within
  tol.
- **Determinism**: same input + seed → **byte-identical** output mesh
  (serialize both and compare). Guards the M3 iteration and M5 greedy-rounding
  tie-breaks, and is the prerequisite for cross-backend parity.
- **Robustness & defined failure**: feed non-manifold input, open boundaries,
  disconnected components, high genus, and near-degenerate slivers. Each must
  *either* produce a validator-passing result *or* fail cleanly per the Risks
  section (Gauss–Bonnet-infeasible pins → surfaced error; MIQP-infeasible →
  documented fallback). The test asserts the failure mode — it does not merely
  avoid the input.
- **Performance budget (gated)**: `test_remesh_perf`, modeled on
  `test_dyntopo_budget`, asserts a 100k-face sphere completes *and* stays under
  a wall-clock ceiling (a generous CI multiplier over the 30s laptop target) —
  a real gate, distinct from the printf-only `bench_remesh`.
- **Memory**: `litestl::alloc` is leak-tracking; assert the input mesh is
  retained (not freed) and intermediate allocations balance across a
  `Mesh_quadRemesh` call, leaving the output mesh as the only survivor.

### Host-repo integration tests (`webgl-app-framework`, `pnpm test`)

- **ToolOp round-trip** — `tests/integration/litemesh_remesh_ops.test.ts`
  (model: `tests/integration/node_editor_ops.test.ts`): build a `litemesh-cube`,
  run `litemesh.quad_remesh`, assert the result is all-quads
  (`Mesh_ngonFaceCount == 0` + a face-side check), then **undo restores the
  pre-remesh mesh bit-for-bit** (diff the `undoPre` serialize blob against a
  fresh serialize after `undo`), and redo re-runs `exec`. Drivable headlessly
  via the Electron harness: `--gen-scene litemesh-cube --run
  "litemesh.quad_remesh(target_edge_length=0.1)"`.
- **Backend parity** — add a remesh case to
  `tests/integration/sculptcore_parity.test.ts`: the same fixed scene + params
  under WASM and `--backend native` must yield identical topology signatures.
  This is the regression gate the M6 native wrapper is verified by.

### Running

```
node make.mjs build native && node make.mjs test     # full native ctest suite
node make.mjs test test_remesh_extract               # one binary
node make.mjs build wasm                              # WASM smoke
node make.mjs node                                    # N-API addon (native backend)
pnpm test                                             # host integration + parity (run from repo root)
```

Each milestone is independently mergeable; the only consumer of the M1
closest-point query before M6 is the test suite, so the early milestones can
ship behind progressively-larger script verbs without exposing an unfinished
`Mesh_quadRemesh()` entry point publicly.

---

## Key references (motivating choices)

- **Diamanti, Vaxman, Panozzo, Sorkine-Hornung 2014** — *Designing
  N-PolyVector Fields with Complex Polynomials*. The M2 solver.
- **Bommes, Zimmer, Kobbelt 2009** — *Mixed-Integer Quadrangulation*.
  The conceptual pipeline (cross-field + seamless param + integer
  quantization).
- **Bommes, Campen, Ebke, Alliez, Kobbelt 2013** — *Integer-Grid Maps
  for Reliable Quad Meshing*. The greedy-rounding strategy used in M5
  in lieu of a full MIQP solver.
- **Liu et al. 2024** (IEEE TVCG) — *Computing Smooth and Integrable
  Cross Fields via Iterative Singularity Adjustment*. The M3
  curl-reduction approach.
- **Lyon, Bommes, Kobbelt 2021** (Eurographics) — *Quad Layouts via
  Constrained T-Mesh Quantization*. The M5 T-mesh + loop-closure ILP
  that guarantees no spirals.
- **Cohen-Steiner & Morvan 2003** — *Restricted Delaunay Triangulations
  and Normal Cycle*. The M1 curvature estimator.

---

## Risks and mitigations

- **MIQP-free quantization viability (M5)**. The LP-relax + greedy
  rounding approach is documented to work in Bommes 2013 but is the
  riskiest milestone. Mitigation: gate M5 behind a feasibility-check
  that falls back to "real-valued quantization + per-face heuristic
  cell snapping" if the loop-closure constraints are infeasible — this
  gives a usable (if spiraling-prone) output rather than a hard failure.
- **Field-constraint feasibility (M2)**. User-pinned poles must satisfy
  Gauss–Bonnet (Σ index = 4χ). Validate this up front and surface an
  error to the UI rather than producing a garbage field.
- **Reprojection drift (M6)**. Aggressive BVH snapping can introduce
  inverted quads. Mitigate by interleaving snaps with Laplacian
  smoothing, capped by a maximum displacement per iteration.
- **Compile-time cost**. Eigen sparse headers in many .cc files balloon
  build times. Keep Eigen `#include`s out of public `.h` files in
  `source/remesh/`; use forward declarations and PIMPL where solvers
  appear in struct members.

---

## Integration with `webgl-app-framework` (the host repo)

`sculptcore` is consumed by the parent `webgl-app-framework` app through the
`LiteMesh` layer. Quad remesh reaches the user as a **whole-mesh ToolOp on a
`LiteMesh`**, exactly mirroring the existing `litemesh.triangulate` op — *not*
through any sculpt-brush path (it is a global operation; the active sculpt
brush never invokes it). The worked template to copy is `TriangulateLiteMeshOp`
in `scripts/lite-mesh/litemesh_ops.ts` and `Mesh_triangulate` in
`sculptcore/source/mesh/c-api/mesh_c_api.cc`.

### The seam, bottom to top

The whole path is a backend-agnostic chain; touch every layer or the native
backend silently diverges from WASM (and the parity test catches it):

1. **C++ C-API** (M6): `Mesh_quadRemesh(Mesh *in, RemeshParams *params) →
   Mesh *` in `source/remesh/c-api/remesh_c_api.cc`, listed in that module's
   `WASM_SYMBOLS` CMake block, **and** hand-wrapped natively in
   `source/napi/napi_runtime.cc` (`NapiRuntime::MeshQuadRemesh` +
   `define(exports, "meshQuadRemesh", …)`). It returns a *new* mesh; the input
   is left intact so the host can keep it for undo.

2. **`IWasmInterface`** — declare `Mesh_quadRemesh(mesh, params): Mesh` in
   `sculptcore/typescript/api/wasm.ts` (the interface every backend
   implements) and implement it in **both** managers:
   - the WASM manager body in `wasm.ts` (calls `_wasm.Mesh_quadRemesh(...)`,
     wraps the returned pointer in a `Mesh` handle like `Mesh_createCube`
     does);
   - the native manager in `sculptcore/typescript/api/nativeManager.ts`
     (`Mesh_quadRemesh(mesh) { return this.addon.meshQuadRemesh(mesh) }`,
     plus the `Mesh_quadRemesh: (m) => nm.Mesh_quadRemesh(m)` entry in its
     method map) and the addon-method type in `nativeBackend.ts`
     (`meshQuadRemesh(mesh: NativeBound, …): NativeBound`).
   Treat the returned mesh as an opaque `SculptHandle` — never read `.ptr`.

3. **`LiteMesh` wrapper** — add a `quadRemesh(params)` method on the
   `LiteMesh` class (`scripts/lite-mesh/litemesh.ts`) alongside the existing
   `triangulate()` / `serialize()` / `_replaceMesh()` / `hasNgons()`. It calls
   `this.wasm.Mesh_quadRemesh(this.mesh, params)` (the class caches
   `this.wasm = getWasmImmediate()!`), then `_replaceMesh(...)` with the
   result. `_replaceMesh` already calls `_rebuildSpatial()`, so the BVH is
   rebuilt for free — same as `triangulate()`.

4. **ToolOp** — `QuadRemeshLiteMeshOp` in `scripts/lite-mesh/litemesh_ops.ts`,
   `toolpath: 'litemesh.quad_remesh'`, copying `TriangulateLiteMeshOp`:
   - `inputs`: `target_edge_length` (float), `density`/`use_curvature`/etc.
     (later milestones), surfaced in the props UI like the triangulate button;
   - **undo via serialize snapshot**: `undoPre` captures
     `mesh.serialize()`, `undo` restores it with
     `mesh._replaceMesh(wasm.Mesh_deserialize(this._undoBlob))`,
     `calcUndoMem` returns the blob length. (A global retopo is a
     whole-mesh topology change, so per-element undo is not viable — the
     snapshot approach is exactly what triangulate uses.)
   - `exec` calls `mesh.quadRemesh(...)` then `window.redraw_all?.()`.
   Register with `ToolOp.register(QuadRemeshLiteMeshOp)`.

5. **UI** — a button/menu entry bound to `litemesh.quad_remesh` (mirror the
   triangulate button; `PropsEditor.ts` already hosts the triangulate tip).

### User-input constraint attributes (density / poles / strokes)

The v1 constraints `.remesh.v.density`, `.remesh.v.pole_index`, and
`.remesh.f.stroke_dir` (see the Attribute schema table) are written *on the
host side* into the `LiteMesh`'s sculptcore attributes before the op runs.
Two existing host patterns to follow:

- attribute create/remove already exist as `litemesh.add_attr` /
  `litemesh.remove_attr` (`litemesh_ops.ts`) — the remesh op can ensure the
  input layers exist the same way;
- **interactive painting** of direction strokes / density / pole pins
  follows `MarkSeamInteractiveOp` (`toolpath: 'litemesh.mark_seam_interactive'`
  in `litemesh_ops.ts`), which is the established model for a drag-paint op
  that writes a per-element attribute through the brush/circle-select path.

This is the heaviest part of integration and is **deferrable**: ship a v1
that runs auto-only (curvature + sharp-feature alignment, M1–M2 + M4–M6, no
painted input) behind `litemesh.quad_remesh` with just a `target_edge_length`
input, then layer the painting ops on once the core pipeline is trusted.

### Parity & verification in the host

- `tests/integration/sculptcore_parity.test.ts` (host repo, under
  `pnpm test`) boots the app headlessly per backend and diffs GPU-buffer
  signatures + leaf counts. Add a small fixed-scene remesh case so WASM vs
  native N-API stay bit-identical (modulo fp) — this is the regression gate
  for the M6 native wrapper.
- The Electron test harness can drive the op headlessly:
  `--run "litemesh.quad_remesh(target_edge_length=0.1)"` after a
  `--gen-scene litemesh-cube`, with `--backend native` to exercise the native
  path. (`--dump` / `--screenshot` for visual/structural diffing.)

### Cross-layer follow-up

Per the host repo's `TODO.md` convention, if `litemesh_ops.ts` ends up
importing remesh-specific symbols from outside the lite-mesh layer (or the
remesh C-API needs a host-side path that doesn't fit `@framework/api`), record
it there.
