# Quad Remesher: Implementation Plan

## Context

We want a feature-aligned **quad remesher** in `sculptcore`, built on the
global MIQ-style pipeline (cross-field → seamless parametrization →
integer quantization → quad extraction) so the result is regular, has
controllable poles, and is **provably free of spiraling edge loops** (the
quantization step is what guarantees that — see research synthesis in
git history of this plan file for the full literature review).

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

---

## Critical files and existing utilities to reuse

From the exploration of the codebase:

- **Mesh + attributes**: `source/mesh/mesh.h`, `source/mesh/mesh_types.h`,
  `source/mesh/attribute.h`, `source/mesh/attribute_builtin.h`.
  Use `BuiltinAttr<T, ".remesh.f.theta">`-style declarations for all
  per-element field/parametrization state.
- **Existing utilities** to imitate (header-only, in
  `source/mesh/utils/`): `edge_collapse.h`, `delaunay.h`,
  `triangulate.h`. Same pattern for the new small-op helpers we add.
- **Eigen sparse**: already vendored at
  `source/litestl/extern/eigen/include/eigen5/Eigen/`. Use
  `Eigen::SparseMatrix<double>` + `SimplicialLDLT` for SPD field/param
  solves; `SparseLU` where needed.
- **Spatial BVH**: `source/spatial/spatial.h` (has `castRay` only — we
  add closest-point queries in M1).
- **Brush command model** for the user-facing "remesh" op:
  `source/brush/brush_command.h`,
  `source/brush/brush_executor.h::CommandExecutor::createCommand`,
  `source/brush/brushes/types.h` (add `QUAD_REMESH` to the enum).
- **Debug app verb registry**: `source/debug/script.cc::execVerb` — every
  milestone gets one or more verbs here so we can drive it from `.txt`
  scripts.
- **C API + bindings**: `source/mesh/c-api/mesh_c_api.cc` for `extern "C"`
  WASM symbols; module bindings pattern in `source/brush/bindings.cc`.
  New module gets `source/remesh/bindings.{h,cc}` registered via
  `source/core/bindings.cc::initBindings`.
- **Tests**: `tests/test_mesh.cc` is the reference shape. Each milestone
  adds a `tests/test_remesh_<phase>.cc`.

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
  test_remesh_extract.cc         -- M6 end-to-end
source/mesh/utils/
  closest_point.h                -- BVH closest-point (lives with mesh utils since it's a general op, called from extract/reproject.cc) (M1)
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
- `source/remesh/c-api/remesh_c_api.cc` — `extern "C"` WASM surface:
  `quadRemesh(Mesh *in, Mesh *out, params...)`.
- `source/remesh/bindings.{h,cc}` — TS/Embind surface mirroring the
  brush module pattern. Wire into `source/core/bindings.cc`.
- Brush integration: add `QUAD_REMESH` to `source/brush/brushes/types.h`
  enum; add a case in
  `source/brush/brush_executor.h::CommandExecutor::createCommand` that
  invokes `QuadRemesh` over the operating spatial nodes. (Even though
  the operation is global, hosting it under the brush command system
  matches how other mesh-level ops are exposed.)
- Debug-app verb: `remesh` (runs the full pipeline end-to-end).
- Test: `tests/test_remesh_extract.cc` runs the full pipeline on:
  - a sphere (closed, expect 8 singularities, valid quad mesh, all
    quads, no T-junctions, Euler characteristic = 2);
  - a cylinder with caps (validates sharp + boundary alignment);
  - a torus (validates zero-singularity case);
  - a noisy organic mesh (smoke test — pipeline must complete).

**Exit criterion**: end-to-end remesh of a 100k-face sphere produces a
valid all-quad mesh in under 30s on a developer laptop, with no
T-junctions, every face a quad, Euler characteristic correct, and
visible regularity. **No spiraling edge loops** verified by checking
that every (u,v)-isoline closes within one trip around the surface.

---

## Verification (end-to-end)

For each milestone, the test plan is the same shape:

1. **Unit test** under `tests/test_remesh_<phase>.cc` driving the C++
   API directly on small canonical inputs (cylinder, sphere, torus,
   plane); wired into `tests/CMakeLists.txt` and run via
   `node make.mjs test native`.
2. **Debug-app script** under (e.g.)
   `source/debug/scripts/remesh_<phase>.txt` running the same scenario
   end-to-end through the verb system; produces screenshot output for
   visual review (`debug_app` already supports headless screenshots).
3. **WASM smoke** — once M6 lands, `node make.mjs build wasm` plus a
   trivial JS harness that calls `quadRemesh` on a mesh loaded in the
   browser, confirming the binding surface.

Run the full suite with:
```
node make.mjs build native && node make.mjs test native
node make.mjs build wasm
```

Each milestone is independently mergeable; the only consumer of the M1
closest-point query before M6 is the test suite, so the early
milestones can ship behind progressively-larger script verbs without
exposing an unfinished `quadRemesh()` entry point publicly.

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
