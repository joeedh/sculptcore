# Paper notes: Embedding Optimization of Layouts via Distortion Minimization

Heuschling, Lim, Kobbelt — Eurographics 2026, Computer Graphics Forum 45(2).
PDF: <https://graphics.rwth-aachen.de/media/papers/360/layout-optimization.pdf>
Code: <https://github.com/7-AlexH/layout-embedding-optimization> (C++ / CMake /
clang / Eigen / OpenMP / **LibTorch 2.4 CPU** for autodiff; **no license file
as of 2026-06** — read as reference, do not vendor until clarified).

Why these notes exist: our long-term goal is an **embedded-layout + curvature
field quad remesher**, and this paper is almost exactly the missing middle
stage of that architecture (field → layout → **optimize embedding** →
per-patch integer-grid map → extract). Read alongside the shipped pipeline
([../quad-remeshing.md](../quad-remeshing.md)) and the filtering ladder
([../plans/quad-remeshing-filtering.md](../plans/quad-remeshing-filtering.md)).

---

## What the method is

A connectivity-preserving **layout embedding optimizer**. Inputs: target tri
mesh `T`, layout connectivity `L` (nodes / arcs / patches — an abstract cell
complex), and a *valid* initial embedding. The layout's combinatorial
structure is strictly preserved; only its geometry moves. Output: a new
embedding minimizing per-patch parametrization distortion — i.e. it
repositions singularities/corners and re-routes patch boundaries.

- **Representation (intrinsic, resolution-independent).** Every node lives as
  `(face, barycentric)` on `T`. Arcs are sampled with valence-2 nodes and
  embedded as piecewise geodesics between consecutive nodes (Noma et al. 2024
  style). Corner nodes are `deg(n) ≥ 3`. The barycentric coordinates of all
  nodes are the only optimization variables; geodesics are implied.
- **Differentiable evaluation.** Each iteration builds an *overlay* mesh (the
  common refinement of `T` and the embedded layout): unfold the triangle strip
  under each geodesic to the plane, compute arc/edge intersection parameters
  `α ∈ (0,1)` there, re-tessellate intersected target faces (ear clipping),
  flood-fill patch labels. All steps keep derivatives w.r.t. the node
  barycentrics.
- **Objective.** Per quad patch: prescribe a **rectangular parameter domain
  whose shape is fixed but whose dimensions float** — dimensions = average of
  opposite geodesic arc-sequence lengths (differentiable, so implicitly
  optimized). Solve two cotan-Laplace harmonic systems with arc-length
  boundary conditions → per-overlay-face 2×2 Jacobians `J` with singular
  values `σmin, σmax`. Energy integrated over **3D area**:

  ```
  e_iso  = (1/σmin)² + σmax²          (isometry, Aigerman 2014 style)
  e_area = (1 − σmin·σmax)²           (area preservation / regularizer)
  E_dist = Σ_patches Σ_faces |f| (ω_iso·e_iso + ω_area·e_area)
  ```

  Plus a **soft 4-RoSy alignment regularizer** on arc edges: per overlay edge
  from a layout arc, compare the edge direction raised to the 4th power
  against the face's field value `z*` (Knöppel 2013 representation),
  length-weighted, normalized by total arc length. Weights used:
  `ω_dist = 1.0` (`ω_iso = ω_area = 0.5`), `ω_align ∈ [0, 0.5]`, typically
  **0.1**. Distortion drives; curvature only biases.
- **Optimizer.** VectorAdam-style momentum descent with the Adam moments
  **parallel-transported in the tangent space** (unfold-based Levi-Civita);
  ambient-space Adam is wrong on a surface. After every node update the
  geodesics are re-traced from scratch. Periodic uniform **arc resampling**
  (first at iter 5, interval +15 after each resample) with optimizer-state
  reset + step-size warm-up; sample nodes closer than a threshold are merged
  (corners never merged); a **minimum rectangle side length** prevents patch
  collapse. No formal validity guarantee — small steps make backtracking
  rarely needed, but it is not implemented.
- **Hyperparameters** (documented, scale-normalized): total surface area
  normalized to 1; `β₁ = β₂ = 0.9`, `ε = 1e-8`, max step `s = 1.5e-3` with
  warm-up; resample target edge length ≈ 0.03 (each arc subdivided at least
  once). Adopt scale invariance from the start.

## Their quad-meshing pipeline (§7.5) — the blueprint for our layout milestone

1. Layout connectivity from Lyon et al. 2021 (relaxed-singularity T-mesh
   quantization); embed it on `T`.
2. **Optimize the embedding** with this method.
3. Choose integer subdivisions per dual loop.
4. Per-patch **harmonic parametrization with rectangular boundary** — this *is*
   the integer-grid map.
5. Extract quads by mapping integer lattice points back to the surface.

Compared to our current global MIQ stack, this factorization moves the pain:

- **The global fold problem becomes local and mostly disappears.** Our
  injectivity stiffening + `lam_seam` ARAP-untangle continuation
  (`quantize/quantize_ilp.*`) exist because one global seamless solve fights
  the field's curl everywhere at once. Per patch, a harmonic map onto a convex
  (rectangular) domain is theoretically bijective (Rado–Kneser–Choquet), so
  fold management shrinks to cotan-weight degeneracy handling.
- **Quantization shrinks** from per-cut-edge integer translations to
  per-dual-loop subdivision counts — a far smaller, better-conditioned integer
  problem (LCK21 T-mesh family; see also Coudert-Osmont et al. 2023 "Quad Mesh
  Quantization Without a T-Mesh" for a no-T-mesh alternative, analyzed against
  our quantizer in
  [quad-quantization-without-tmesh.md](quad-quantization-without-tmesh.md)).
- **Our field stack stays load-bearing.** Embedding optimization fixes
  geometry, *not* combinatorics: field singularities attract layout corners,
  and a misplaced field singularity yields a misplaced (asymmetric) layout.
  Filtering Tiers 2/4/5 (curvature filtering, smoothness knob, singularity
  pair cancellation) remain prerequisites in the layout future, not legacy.
- **The intrinsic representation extends our solve/reproject split.** §7.3:
  layouts transfer across mesh resolutions by transporting
  `(face, barycentric)` and re-tracing geodesics. That is `decimateForSolve`'s
  philosophy lifted to layouts — optimize the embedding on the coarse solve
  mesh, transport to the full-res input, re-trace, parametrize per patch at
  full res. Any future layout stage should use `(face, barycentric)` node
  coordinates from day one.
- **Initialization comes free for us.** The method requires a *valid* initial
  embedding (they punt to Born et al. 2021 branch-and-bound when invalid). Our
  separatrices traced from the existing field/parametrization provide it. Note
  their noise experiment: the *optimizer* tolerates heavy noise; the *initial
  construction* is what fails first — a repair path is still needed.

## Design lessons (energy selection, recorded so we don't relitigate)

- **Distortion-driven, curvature-regularized — not the reverse.**
  Campen-Kobbelt 2014 (curvature-driven singularity placement) fails on noisy
  input; this method survives heavy normal-displacement noise because the
  primary signal is 0th-order (areas/lengths via the harmonic map) and the
  2nd-order curvature field is demoted to a `ω ≈ 0.1` regularizer. Matches the
  filtering plan's derivative-order rationale; right bias for our Meshy/scan
  corpus.
- **Symmetric Dirichlet is wrong for boundary/cut placement** when
  connectivity must survive: part of its integration runs over the parameter
  domain, creating a patch-*shrinkage* incentive (collapse). Use the
  non-symmetric iso term + separate area term instead.
- **Hencky/Yamabe conformal-factor energy (Sharp-Crane 2018) is ill-posed**
  for this: distortion can always be reduced with longer cuts; patches go
  non-rectangular and useless downstream.
- **Prescribe domain shape, float its dimensions** — the core trick. Fixing
  the rectangle shape keeps patches extraction-friendly; floating dimensions
  (from differentiable geodesic lengths) lets the rest shape adapt. This is
  the inverse of classical parametrization: find the patch for a prescribed
  domain.
- **Moving singularities beats smoothing them.** On identical layout
  connectivity, free singularity movement measurably improves min scaled
  Jacobian / max inner angle over the Laplacian-smoothing post-process of
  Lyon 2021. Our `reproject` stage's Laplacian relax is precisely the baseline
  they beat.
- **T-junctions are cheap** (T-node = corner in the two patches at the T-base,
  sample node in the patch over the T-bar; preserved through resampling) —
  matters because a T-mesh-quantized layout has them. **Boundaries**: boundary
  nodes embed in boundary edges and slide along the boundary — relevant to the
  Tier 6 open-character-mesh policy. Non-quad patches: any convex prescribed
  boundary works (triangle-inequality fallback: equilateral).

## Engineering gotchas they report

- **Geodesics through mesh vertices** break differentiability. Fix (Appendix
  A): half-sector unfolding + greedy fan embedding + nudging intersections to
  keep `α ∈ (0,1)`. *Rarely*, the cyclic arc order around a node is violated
  afterward → invalid embedding, no recovery implemented (acknowledged open
  problem).
- **Overlay slivers** near embedded arcs destabilize the cotan systems —
  budget for intersection snapping / epsilon merging or intrinsic-Delaunay
  cleanup.
- **No backtracking**; robustness relies on small steps. A production version
  should add a per-iteration validity check + rollback.
- They **isotropically retriangulate all inputs first** — quietly confirming
  Tier 1 triage / Tier 9 pre-remesh as prerequisites, not nice-to-haves.

## Cost / feasibility

Table 1 (12-core Ryzen 5900X): 78–206-patch layouts on 1.5k–17k-face meshes
take **31–173 minutes** (4.9–58.8 s/iteration). Breakdown: backprop ≈ 67%,
overlay construction ≈ 22%, objective evaluation ≈ 5%. The LibTorch autodiff
is the bottleneck and a non-starter for WASM.

Path to practicality (all standard, none done in the paper):

- **Analytic adjoint gradients.** The chain is overlay → two SPD linear solves
  → closed-form 2×2 SVD energy. The adjoint of a linear solve is one extra
  solve against the *same* (symmetric) cotan factorization — reusable, and we
  already carry CHOLMOD. 2×2 SVD derivatives are closed-form.
- **Patches are independent** → parallel per-patch solves.
- **Better initialization** (our field-traced layout vs their manual initial
  embeddings) → far fewer iterations.
- **Coarse-solve-mesh transport** (above) → small overlay during optimization.

## Immediately actionable on the current pipeline

1. **Add min scaled Jacobian per quad** to the Tier 0 metrics
   (`mesh/utils/mesh_validate.h` → STATS → manifest). It is their headline
   quality metric (with max inner angle, which we already track via
   `min_interior_angle`'s family) and enables comparison against published
   numbers.
2. **Consider upgrading the `reproject` relax** from plain Laplacian to a
   tangential relaxation driven by the pointwise `e_iso + e_area` energy on
   output quads — captures some of "distortion-driven beats smoothing" with no
   layout machinery.
3. When the layout milestone is planned, start from the §7.5 blueprint above
   and the representation/energy decisions in these notes.

## Plan integration (decided 2026-06)

Decisions folded into
[../plans/quad-remeshing-filtering.md](../plans/quad-remeshing-filtering.md)
after reviewing these notes (quotes verified against the PDF):

- **The separate `--solve` decimation stage is removed; the Tier 9 pre-remesher
  is the only input-geometry stage.** Matches the paper's setup verbatim — "For
  all experiments, we isotropically retriangulate the input surfaces" — one
  isotropic remesh, no decimator, ever. The conditioning gotchas above (overlay
  slivers, geodesics through vertices) are the deep reason: every downstream
  cotan solve, theirs and ours, wants near-equilateral triangles, which BK
  produces and geometric-error decimation does not. Follow-ons (param removal,
  coarsen-bootstrap replacement, 20% auto edge-budget composition) live in the
  Tier 9 removal blockquote.
- **The multi-resolution workflow survives via anchors, not snapping** (Tier 9g):
  the pre-remesher maintains per-vertex `(input face, barycentric)` source
  anchors, so reproject becomes a local walk that cannot sheet-jump —
  `sheet_min_dot` demotes to a safety net, and the §7.3 intrinsic representation
  the layout milestone needs exists from day one.
- **Tier 0 gains min scaled Jacobian + max inner angle** (actionable item 1 →
  planned).
- **Tier 6 boundary policy: slide, don't pin** — boundary verts constrained to
  the boundary polyline (collapse along it, smooth along it), as boundary nodes
  slide here.
- **`pre_remesh_align` gets an intermediate A/B point (≈0.5) at gate 9** — the
  paper keeps alignment a small bias (`ω_align ≈ 0.1`) over the distortion
  driver; full field-alignment against a noisy rough field risks regularizing
  toward the field's own noise.
- **Quantize-parallelism deep work is deprioritized**: §7.5 dissolves the global
  integer problem into per-dual-loop counts plus independent per-patch harmonic
  solves (embarrassingly parallel), so heavy engineering on parallelizing the
  global greedy CHOLMOD rounding is likely throwaway. Cheap wins only, if
  quantize pain persists.
- **Reproject's Laplacian relax is confirmed as the weak link** — Fig. 14 beats
  LCK21's Laplacian singularity-smoothing on *identical connectivity* with
  distortion-driven movement; actionable item 2 stays on the books, unscheduled.
- **The 20% auto edge budget composes cleanly**: while the global-MIQ pipeline
  solves on the pre-remeshed mesh, solve resolution ≈ fidelity, so auto targets
  stay budgeted (`L ≤ mean_in/√0.8`); explicit `pre_remesh_target` is the
  deep-coarsening route. The layout milestone later adds a cost-driven auto
  solve resolution + anchor transport — deep coarsening becomes safe there
  because fidelity is restored by transport rather than lost.
