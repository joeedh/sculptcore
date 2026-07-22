# Fast GPU-compatible cloth simulation — survey

> **Status (2026-07-21).** Literature survey / orientation report — no method
> adopted yet. This is a map of the design space for a real-time cloth solver
> that would live alongside the sculpt engine, with a focus on (a) solvers that
> parallelize cleanly on the GPU and (b) continuous collision against the *simple
> analytic shapes* a sculpt brush drags around (Section 6). Claims are tagged
> **[literature]** (published result), **[folklore]** (widely-held practitioner
> knowledge, weaker provenance), or **[hypothesis]** (my inference for this
> codebase). No measured numbers from our own engine appear here — nothing is
> built yet.

## Motivating context

The sculpt engine already owns the pieces a cloth mode would need: a triangle
`mesh::Mesh` with per-vertex attributes, a spatial BVH (`source/spatial/`) with
GPU-aggregated node VBOs, a compiled-kernel brush executor
(`source/brush/`), and a WGSL/SPIR-V/CUDA/HIP compute path. A "cloth brush"
in the ZBrush/Blender sense is: the artist drags a **collider primitive**
(sphere, cylinder, capsule, plane) through a sheet or a surface region and the
mesh drapes, wrinkles, and slides against it in real time. That framing sets
two hard requirements this survey is organized around:

1. **The solver must be GPU-parallel and unconditionally stable at large step
   sizes** — an artist yanks the brush fast; an explicit spring solver would
   explode. This points squarely at the position-based / projective-dynamics
   family (Sections 3–4).
2. **Collision is against moving analytic shapes, not (only) a triangle
   soup.** The collider is a swept cylinder/capsule with a known closed-form
   distance function. That is *much* cheaper and more robust than mesh–mesh
   CCD, and it is the interesting special case Section 6 develops in full.

Self-collision of the cloth with itself is the harder, separate problem and is
surveyed only briefly (Section 5) — for a first cloth-brush it can be
approximated or deferred.

---

## 1. Problem setup

Cloth is a thin elastic sheet discretized as `n` particles with positions
`x ∈ R^{3n}`, velocities `v`, a (usually lumped-diagonal) mass matrix `M`, and
internal + external forces `f(x, v)`. The equation of motion is
`M x'' = f(x, v)`. Two discretization philosophies:

- **Mass–spring.** Edges (and bending "cross" springs / dihedral pairs) are
  Hookean springs. Simple, cheap, mesh-topology-driven; anisotropic and
  resolution-dependent, but by far the most common real-time choice and the one
  that maps most directly onto a per-edge GPU constraint. Bending is either a
  spring across the two opposite vertices of adjacent triangles or a proper
  dihedral-angle constraint.
- **Continuum / FEM.** Per-triangle in-plane strain (Baraff–Witkin's
  stretch/shear energies **[literature]**, or St. Venant–Kirchhoff / co-rotational
  membrane energies) plus a discrete bending energy. More physically faithful,
  isotropic, resolution-consistent; heavier per-element math but still very
  GPU-friendly because every triangle is independent within a color.

For a sculpt-tool cloth, mass–spring or the Baraff–Witkin triangle energy are
both reasonable. The **solver** matters far more than the constitutive model for
GPU throughput, so the rest of the survey is organized by solver.

---

## 2. Time integration and the implicit-Euler baseline

Explicit integrators (forward/symplectic Euler, RK) are trivially parallel but
their stable step size shrinks as spring stiffness rises (`Δt ∝ sqrt(m/k)`);
stiff cloth forces either tiny steps or explodes. Real-time cloth therefore uses
**implicit / variational integration**, which is unconditionally stable and lets
you take one big step per frame.

The canonical baseline is **backward (implicit) Euler**, Baraff & Witkin,
*"Large Steps in Cloth Simulation"* (SIGGRAPH 1998) **[literature]**. One step
solves the nonlinear system

```
(M − Δt² ∂f/∂x − Δt ∂f/∂v) Δv = Δt ( f + Δt (∂f/∂x) v )
```

i.e. a large **sparse symmetric linear solve** per step (they linearize once and
solve with modified preconditioned conjugate gradient, folding collision
constraints in as filtered directions). This is the reference every fast method
is trying to approximate more cheaply.

The modern restatement is the **variational / optimization view** (Martin et al.
2011; Gast et al. 2015) **[literature]**: implicit Euler is the minimizer of

```
E(x) = (1 / 2Δt²) (x − y)ᵀ M (x − y) + Σ_i W_i(x),   y = xⁿ + Δt vⁿ + Δt² M⁻¹ f_ext
```

where `W_i` are the elastic potentials and `y` is the inertial ("predicted")
position. **Every fast solver below is a different way of approximately
minimizing this same `E`.** That is the single most useful unifying idea in the
survey: PBD, Projective Dynamics, ADMM, descent methods, and Newton are all
minimizers of `E`, trading convergence quality for parallelism and cost.

---

## 3. Solver families and their GPU affinity

Summary table (n = particle count, C = constraint count):

| Method | Stability at large Δt | Per-iter cost | GPU parallelism | Convergence / quality | Notes |
|---|---|---|---|---|---|
| Explicit mass-spring | poor | O(C) | trivial | — | needs tiny Δt; avoid |
| **PBD** (Müller 2007) | excellent | O(C) | good (coloring/Jacobi) | stiffness = f(iters, Δt) | fast, biased, stiffness not physical |
| **XPBD** (Macklin 2016) | excellent | O(C) | good | compliance-correct | PBD done right; substep-heavy |
| **Projective Dynamics** (Bouaziz 2014) | excellent | O(n) local + global solve | very good (local step) | good; fixed system matrix | prefactor once; global solve is the bottleneck |
| **ADMM / Liu et al. 2017** | excellent | O(C) local + global | very good | good; flexible energies | generalizes PD; per-constraint splitting |
| **Descent + accel** (Wang 2016, Wang&Yang 2016) | excellent | O(C) | excellent (matrix-free) | Chebyshev/Anderson to speed | no global solve; pure GPU |
| **Vivace** (Fratarcangeli 2016) | excellent | O(C) | excellent (colored GS) | fast GS convergence | graph-colored Gauss–Seidel PD |
| **Implicit + matrix-free PCG** | excellent | O(C)/iter | good | most accurate cheap option | classic; PCG has serial-ish reductions |
| **Newton / Projective Newton** | excellent | O(n^{1.x}) solve | moderate | best per-step | expensive; offline-ish |
| **IPC** (Li et al. 2020) | excellent + guaranteed non-penetration | high | moderate | reference-quality | too heavy for a brush; great as ground truth |

### 3.1 Position Based Dynamics (PBD)

Müller et al., *"Position Based Dynamics"* (2007) **[literature]**. Skip forces
entirely: predict `x̃ = xⁿ + Δt vⁿ + Δt² M⁻¹ f_ext`, then **iteratively project
positions onto constraints** (each distance/bending/collision constraint is
satisfied by moving its particles along the constraint gradient, mass-weighted),
then set `vⁿ⁺¹ = (x − xⁿ)/Δt`. It is a nonlinear Gauss–Seidel on the constraint
set.

- **Why it wins for real-time:** unconditionally stable, dead simple, handles
  collisions as just more constraints, trivially bounded cost (fixed iteration
  count). It is the backbone of virtually every game cloth system.
- **The catch:** effective stiffness depends on iteration count *and* step size
  and mesh resolution — it is not a physical material parameter. Softens with
  fewer iterations, stiffens with more; changing frame rate changes the cloth.

### 3.2 XPBD (Extended PBD)

Macklin, Müller & Chentanez, *"XPBD: Position-Based Simulation of Compliant
Constrained Dynamics"* (2016) **[literature]** fixes PBD's iteration-dependent
stiffness by introducing a **compliance** `α = 1/(k Δt²)` and a per-constraint
Lagrange-multiplier accumulator. The projection becomes

```
Δλ = ( −C(x) − α̃ λ ) / ( ∇C M⁻¹ ∇Cᵀ + α̃ ),   α̃ = α/Δt²
Δx = M⁻¹ ∇Cᵀ Δλ
```

Now stiffness is a real material constant independent of iteration count, and as
`α → 0` you recover a hard constraint. XPBD is the recommended default for a
new real-time cloth solver **[hypothesis]**: same cost and GPU shape as PBD,
physically meaningful parameters. It pairs with **small-step substepping**
(Macklin et al. 2019, *"Small Steps in Physics Simulation"* **[literature]**):
many tiny substeps with **one** constraint iteration each converges better and
cheaper than one big step with many iterations, and it improves collision
robustness because per-substep motion is small (directly relevant to Section 6).

### 3.3 Projective Dynamics (PD)

Bouaziz et al., *"Projective Dynamics: Fusing Constraint Projections for Fast
Simulation"* (2014) **[literature]**, building on Liu et al. 2013's mass–spring
version. PD restricts the elastic energies to the special form
`W_i(x) = (w_i/2) min_{p ∈ C_i} ‖ A_i x − p ‖²` (distance to a constraint
manifold), which makes minimizing `E` a **local/global alternation**:

- **Local step** (massively parallel): for each constraint independently, find
  the closest point `p_i` on its manifold (project the edge to rest length,
  project the triangle to a rigid/rest configuration, etc.). Embarrassingly
  parallel, one GPU thread per constraint, no data sharing.
- **Global step:** solve a *single* sparse SPD linear system
  `(M/Δt² + Σ w_i A_iᵀ A_i) x = b` whose matrix is **constant** (the geometry of
  `A_i` doesn't change), so you **prefactor once** (Cholesky) and only re-solve
  each iteration. The matrix constancy is PD's superpower.

On the GPU the local step is free; the **global solve is the bottleneck** — a
back-substitution of a Cholesky factor is inherently serial. The usual GPU
answer is to *not* do a direct solve: run a few Jacobi/Chebyshev/CG iterations on
the global system instead (see 3.5), or color-and-Gauss–Seidel it (3.6). PD
gives noticeably better draping than raw PBD for similar cost and is a strong
choice if you can afford a modest global solve.

### 3.4 ADMM / operator splitting

Overby et al., *"ADMM ⊇ Projective Dynamics"* (2017) **[literature]** generalizes
PD via the alternating direction method of multipliers: it keeps PD's parallel
local projections and constant global matrix but admits **arbitrary nonlinear
energies** (real St. Venant–Kirchhoff, strain limiting, hyperelasticity) rather
than PD's quadratic-distance restriction, by carrying a dual variable per
constraint. Same GPU shape as PD (parallel local + one global solve), more
material generality, slightly more per-constraint state. A good upgrade path if
PD's material model proves too limiting.

### 3.5 Descent methods (no global solve) — the pure-GPU sweet spot

These attack `E(x)` directly with first-order optimization and **no linear
system at all**, which is exactly what a GPU wants.

- **Wang & Yang 2016**, *"Descent Methods for Elastic Body Simulation on the
  GPU"* **[literature]**: gradient/Jacobi descent on `E` accelerated by
  **Chebyshev semi-iteration** (a cheap polynomial over-relaxation with a
  spectral-radius estimate) plus Nesterov-style momentum. Fully matrix-free, one
  thread per vertex/constraint, tiny memory, scales to hundreds of thousands of
  vertices at real-time rates on commodity GPUs. This is arguably the most
  GPU-native option in the survey.
- **Wang 2015**, *"A Chebyshev Semi-Iterative Approach for Accelerating
  Projective and Position-Based Dynamics"* **[literature]**: wraps Chebyshev
  acceleration around PD *or* PBD iterations — a drop-in ~2–5× convergence
  speedup with almost no extra cost or code. If you build PD or XPBD, add this
  essentially for free.
- **L-BFGS** (Liu et al. 2017, *"Quasi-Newton Methods for Real-Time Simulation
  of Hyperelastic Materials"* **[literature]**): a few-vector quasi-Newton loop
  with the PD/global matrix as the initial Hessian approximation; better
  convergence than plain descent at slightly higher per-iter cost and memory.
- **Anderson acceleration** (Peng et al. 2018 **[literature]**): a more general
  fixed-point accelerator that also speeds up PD/local-global.

For a sculpt-brush cloth, a **Chebyshev-accelerated Jacobi/XPBD** loop is likely
the best cost/quality/parallelism balance **[hypothesis]** — matrix-free, no
prefactor to maintain when topology changes (important if the cloth is
dyntopo-remeshed), and it degrades gracefully under a fixed frame budget.

### 3.6 Vivace — graph-colored parallel Gauss–Seidel

Fratarcangeli, Tibaldo & Pellacini, *"Vivace: a Practical Gauss–Seidel Method
for Stable Soft Body Dynamics"* (SIGGRAPH Asia 2016) **[literature]** observes
that Gauss–Seidel converges roughly 2× faster than Jacobi but is serial — so it
**graph-colors** the constraint graph (adjacent constraints get different
colors) and processes one color at a time: within a color all constraints are
independent and run fully parallel, across colors you get Gauss–Seidel's fast
convergence. This is the standard recipe for turning any local-projection solver
(PBD/XPBD/PD-local) into a fast GPU Gauss–Seidel and is discussed further in
Section 4.

### 3.7 Implicit Euler with matrix-free PCG on the GPU

The direct descendant of Baraff–Witkin: build the system-matrix–vector product
`(M − Δt² K) p` as a matrix-free per-element gather (never assemble the matrix)
and run **preconditioned CG** on the GPU. Most physically accurate of the cheap
options; the wrinkle is that CG's dot-products are **global reductions** (a mild
serialization / sync point each iteration) and preconditioners good enough to
cut iteration count (incomplete Cholesky) are themselves serial. Block-diagonal
/ Jacobi preconditioners parallelize but converge slowly. Viable, but PD/descent
methods usually beat it on pure GPU throughput for interactive cloth.

### 3.8 Newton, Projective Newton, and IPC (the accuracy ceiling)

Full Newton on `E` (with SPD Hessian projection — Teran et al.'s "Projective
Newton" **[literature]**) gives the best per-step convergence but needs a fresh
factorization each iteration; too heavy for a brush. **IPC** (Li et al. 2020,
*"Incremental Potential Contact"* **[literature]**) adds a barrier energy that
**guarantees** interpenetration-free, inversion-free results with a line search —
the gold standard for robustness, and the right tool if you ever need
*guaranteed* non-penetration, but far too expensive for a real-time brush.
Useful here as a **ground-truth oracle** to validate a fast solver against,
mirroring how `sbrush-verify` A/B-checks the brush backends.

### 3.9 Hierarchical / multigrid

Multigrid and multiresolution schemes (e.g. Wang et al.; Tamstorf et al. 2015's
smoothed-aggregation multigrid for cloth **[literature]**) accelerate the *global*
convergence that all the flat iterative methods struggle with — low-frequency
draping modes propagate across the sheet in O(levels) rather than O(diameter)
iterations. More complex to build and to keep robust under changing topology,
but the right lever if large, low-frequency drape (a whole cape settling) is too
slow with a flat solver. Note the sculpt engine already has a decimation
hierarchy concept in the remesher — a coarse cloth proxy could seed a fine solve
**[hypothesis]**.

---

## 4. GPU parallelization strategy (cross-cutting)

Independent of solver family, these are the levers that decide GPU throughput:

- **Jacobi vs Gauss–Seidel.** Jacobi (update all constraints from the *old*
  positions, then average/accumulate) is embarrassingly parallel but converges
  slowly and can oscillate without under-relaxation. Gauss–Seidel (use freshest
  positions) converges ~2× faster but is serial unless colored. The practical
  GPU answer is **graph-colored Gauss–Seidel** (Section 3.6): a small number of
  independent-set colors, one dispatch per color.
- **Graph coloring.** Precompute a coloring of the constraint graph (greedy /
  Welsh–Powell, or a parallel Luby-style coloring on the GPU). Redo it only when
  topology changes. Aim for few colors (≈ max constraint valence + 1) so there
  are few dispatches. For a triangle mesh's edge constraints this is typically
  6–8 colors.
- **Atomics vs coloring for accumulation.** Jacobi-style methods that scatter
  position deltas to shared vertices can use atomic adds instead of coloring
  (simpler, topology-agnostic) at the cost of atomic contention and
  nondeterminism. Coloring avoids conflicts entirely and is deterministic — a
  real advantage for the engine's bit-reproducibility ethos (cf. `sbrush-verify`).
- **Memory layout.** Structure-of-arrays for positions/velocities; keep the
  constraint's vertex indices packed so a warp reads coalesced. This is exactly
  the discipline `source/spatial/` already applies to node VBOs.
- **Substepping over inner iterations.** Per Macklin 2019 **[literature]**,
  spend the frame budget on more *substeps* (each: integrate + one constraint
  pass + one collision pass) rather than many inner iterations of one big step.
  Better energy behavior, and — critically for Section 6 — the per-substep
  displacement is small, so even *discrete* collision rarely tunnels and CCD is
  cheaper and more reliable.
- **Fixed budget, graceful degradation.** A brush must hold frame rate. All the
  position-based/descent methods let you cap iterations/substeps and simply
  accept a slightly softer result — a property Newton/IPC lack.

**Fit with sculptcore [hypothesis].** The brush executor already dispatches a
compiled kernel over `SpatialNode`s with a vertex-iterator factory; a colored
XPBD or Chebyshev-Jacobi cloth pass is the same shape — a per-color (or
per-substep) compute dispatch over the affected leaves, authored in the same
sbrush/WGSL pipeline, reusing the spatial tree for the brush-region restriction
(only simulate leaves near the collider, like dyntopo's `seedVerts`).

---

## 5. Collision handling (overview)

Two sub-problems, very different difficulty:

**Cloth vs external objects.** Cheap and robust, *especially* when the object is
an analytic primitive — this is Section 6, the case the sculpt brush actually
needs.

**Cloth self-collision.** The hard part. Requires:

- **Broad phase** on the GPU: spatial hashing (Teschner et al. 2003
  **[literature]**) or a linear BVH (LBVH, Karras 2012 **[literature]**) built
  each frame over triangle AABBs (dilated by thickness + motion). Both build and
  query well on the GPU. The spatial tree we already have is a natural fit for
  the broad phase.
- **Narrow phase:** vertex–triangle and edge–edge tests, discrete or continuous.
- **Response:** repulsion forces / impulses (Bridson et al. 2002 **[literature]**),
  or constraint projection (PBD collision constraints), or the IPC barrier for
  guaranteed non-penetration.

For a first cloth-brush, self-collision can be handled with simple per-vertex
thickness repulsions or skipped; robust cloth-on-cloth is a large follow-on
project. The rest of this survey concentrates on the tractable, high-value case:
continuous collision against the moving primitive.

---

## 6. Continuous collision detection with simple (non-mesh) shapes

This is the section the sculpt-brush use case lives in: **a brush drags a
cylinder (or sphere / capsule / plane) along the cloth, and the cloth must not
tunnel through it even when the artist moves fast.** Because the collider is an
*analytic primitive with a closed-form signed distance function*, we can do CCD
far more cheaply and robustly than mesh–mesh CCD — no BVH over the collider, no
edge–edge combinatorics, just per-vertex closed-form math that is a perfect fit
for one-GPU-thread-per-vertex.

### 6.1 Why continuous (not discrete)

Discrete collision samples only the *end* of the step: if a vertex was on one
side of the cylinder at `xⁿ` and the other side at `xⁿ⁺¹`, a discrete test sees
no penetration and the cloth **tunnels**. The faster the brush (or the thinner
the cylinder relative to the step), the worse it is. CCD instead asks: *along the
straight-line motion of the vertex over the substep, what is the earliest time it
first touches the collider?* — the **time of impact (TOI)**, `t* ∈ [0, 1]`.

Two things make the brush case tractable where general CCD is painful:

1. **The collider moves too.** The brush translates (and maybe rotates) the
   primitive from `Pⁿ` to `Pⁿ⁺¹` over the substep. We work in the collider's
   frame so only *relative* motion matters: subtract the primitive's motion from
   the vertex's, and the primitive is momentarily static.
2. **The primitive has an SDF.** Sphere, capsule, infinite/finite cylinder, box,
   plane, and their swept versions all have cheap closed-form (or few-iteration)
   distance functions. CCD reduces to *finding the first time a moving point
   enters an offset surface* — often a scalar root of a low-degree polynomial.

### 6.2 Vertex-vs-primitive as a moving-point / SDF root find

Let a cloth vertex move linearly over the substep,
`x(t) = x₀ + t (x₁ − x₀)`, `t ∈ [0,1]`, and let the primitive move rigidly with
pose `T(t)` (translation `c(t)`, rotation `R(t)`). Transform the vertex into the
primitive's *local* frame:

```
p(t) = R(t)ᵀ ( x(t) − c(t) )
```

If the brush motion over one substep is a pure translation with constant
velocity (the common case, and exactly true if you substep finely), `R` is
constant and `p(t)` is **linear** in `t`:
`p(t) = p₀ + t·u`, with `u = (x₁ − x₀) − (c₁ − c₀)` the *relative* displacement.
The collider is a static SDF `Φ(p)` in local space, inflated by the cloth
thickness `h`. **TOI is the smallest `t ∈ [0,1]` with `Φ(p(t)) = h`.** For the
primitives below this is a closed-form scalar root; for a rotating collider you
either substep finely (recommended) or do a few Newton/bisection steps on
`Φ(p(t)) − h`, which converges fast because `Φ` is smooth and nearly monotone
along the short segment.

Below, all shapes are placed in their local frame (axis along +Z where relevant,
centered at the origin); `h` is the cloth thickness / collision offset; the
moving point is `p(t) = p₀ + t u`.

### 6.3 Sphere (and the moving-sphere base case)

Local SDF `Φ(p) = |p| − r`. Solve `|p₀ + t u| = r + h`, a **quadratic** in `t`:

```
a = u·u
b = 2 (p₀·u)
c = p₀·p₀ − (r + h)²
disc = b² − 4ac
```

If `disc < 0`, no hit. Else `t* = (−b − sqrt(disc)) / (2a)` (the smaller root);
accept if `t* ∈ [0,1]` (and `a > ε`; if `a ≈ 0` there's no relative motion, fall
back to a static inside/outside test). This is the classic ray-vs-sphere against
the *relative* motion — the cheapest CCD there is, a handful of FLOPs per vertex.

### 6.4 Capsule (line-segment-swept sphere) — the recommended collider

A capsule of radius `r` with segment endpoints `A,B` (local frame) has
`Φ(p) = dist(p, segment AB) − r`, where the segment distance uses the clamped
projection parameter

```
s = clamp( (p − A)·(B − A) / (B − A)·(B − A), 0, 1 )
q(p) = A + s (B − A)        // closest point on the segment
dist(p, seg) = |p − q(p)|
```

For CCD, `Φ(p(t)) = r + h` is *not* a simple quadratic because `s` depends on
`t`. The robust, GPU-friendly approach is a **short conservative-advancement /
bisection** on `g(t) = Φ(p(t)) − (r + h)`:

- `g` is smooth and, over one small substep, effectively monotone as the point
  approaches the capsule. Start at `t = 0`; if `g(0) ≤ 0` the vertex starts in
  contact (handle as static). Otherwise march/bisect on `[0,1]` for the first
  sign change; 5–10 iterations give ample precision and vectorize perfectly (no
  branch divergence beyond the clamp).
- **Conservative advancement** (Mirtich; Zhang et al. **[literature]**) is the
  principled version: repeatedly advance `t` by `g(t) / v_max` where `v_max` is
  an upper bound on the relative closing speed (`|u|` here), guaranteeing you
  never step past the first contact. Converges in a few iterations for a convex
  primitive.

**Why the capsule is the right default collider [hypothesis]:** it *is* a
segment-swept sphere, so a "cylinder dragged along the cloth" — a segment moving
through space — is naturally a capsule (rounded ends avoid the sharp-rim
special-casing a true finite cylinder needs), and its distance function is the
cheapest non-trivial SDF. Use it unless the flat end caps of a true cylinder are
artistically required.

### 6.5 Finite cylinder

A true finite cylinder (radius `r`, half-height `H`, axis = local Z) is the
union of three regions and its SDF is
`Φ(p) = max( |p.xy| − r,  |p.z| − H )` for the outside, with the classic
2D "rounded-box in (radial, axial) space" form for exactness:

```
d_radial = |p.xy| − r        // radial distance to the side
d_axial  = |p.z|  − H        // axial distance to the caps
Φ(p) = min(max(d_radial, d_axial), 0)
     + length( max(vec2(d_radial, d_axial), 0) )
```

For CCD you can either root-find `Φ(p(t)) = h` directly (bisection as in 6.4), or
— cheaper and often good enough — test the **three sub-cases analytically**
against the relative ray `p(t) = p₀ + t u` and take the earliest valid TOI:

1. **Infinite side wall:** ignore Z, solve the 2D quadratic
   `|p.xy(t)| = r + h` (same form as the sphere but in the XY-plane), accept the
   TOI only if `|p.z(t*)| ≤ H`.
2. **End-cap planes** `z = ±(H + h)`: linear solve `p.z(t) = ±(H+h)`, accept
   only if the hit point lies within the cap disc `|p.xy(t*)| ≤ r`.
3. **Rim circles** (the sharp edge at `|p.xy| = r, p.z = ±H`): a moving point vs a
   circle is the messiest case (a quartic in general). In practice **round the
   rim** — i.e. use a capsule or a rounded cylinder — and skip the quartic
   entirely. Sharp-rim exact CCD is rarely worth it for a brush.

The infinite-cylinder side-wall quadratic is:

```
// axis = local Z; drop the z-component, treat as a 2D circle of radius r+h
w  = (p₀.xy)             // 2D
d  = (u.xy)              // 2D relative velocity in the cross-section plane
a = d·d
b = 2 (w·d)
c = w·w − (r + h)²
// smaller root in [0,1], then range-check |p.z(t)| ≤ H
```

### 6.6 Plane / half-space (backboard, floor)

`Φ(p) = n·(p − p_plane)`. TOI is a single linear solve
`n·(p(t) − p_plane) = h ⇒ t* = (h − n·(p₀ − p_plane)) / (n·u)` (guard `n·u ≈ 0`).
Trivial and exact; useful as a backing surface the brush presses cloth against.

### 6.7 The "dragging" case explicitly

"Drag a cylinder along the cloth" means the collider's own motion over the
substep is significant — that motion is precisely the `−(c₁ − c₀)` term folded
into `u` in §6.2. Two robustness notes:

- **Substep the brush path.** The brush moves along a stroke; split each frame's
  collider displacement into the same substeps as the solver (Section 4). Small
  per-substep `|u|` keeps `p(t)` well-approximated as linear even if the collider
  also rotates, keeps the conservative-advancement bound tight, and makes the
  whole thing more accurate than one big swept test.
- **Swept-volume view (equivalent framing).** A cylinder translating over a
  substep sweeps a capsule-ish/prism volume; testing a *static* vertex against
  that swept SDF is mathematically the same as testing a *moving* vertex against
  the static primitive (what §6.2 does). The moving-point form is preferred on
  the GPU — no swept-SDF to construct, just the relative velocity `u`.

### 6.8 Cloth edges vs the primitive (optional, for thin sheets)

Per-vertex CCD can still miss a case where an *edge* of the cloth straddles the
thin collider between its two vertices (both endpoints pass on the same side, but
the edge crosses). For a fat collider (cylinder radius ≫ edge length) this never
happens and vertex-only CCD is sufficient. If it matters, add an
**edge-vs-capsule / edge-vs-cylinder** closest-approach test (segment–segment
distance over time), still closed-form-ish and parallel per edge. For a
sculpt-brush collider that is large relative to the mesh resolution, **vertex-only
CCD is the pragmatic default [hypothesis]**; keeping cloth edges short relative to
the collider (the mesh is already dyntopo-remeshable) sidesteps the problem.

### 6.9 Collision response after TOI

Finding `t*` is half the job; the response must keep the solver stable:

- **Position projection (PBD/XPBD-style).** Advance the vertex to `t*`, then push
  it out to the offset surface along the SDF gradient `∇Φ` (which for these
  primitives is the closed-form outward normal — radial direction for a
  cylinder/capsule, `p/|p|` for a sphere, `n` for a plane). Add it as a
  (possibly one-sided, unilateral) collision constraint in the same constraint
  loop as the elastic constraints, so the solver reconciles cloth tension and
  contact together. This is the cleanest fit for a position-based solver.
- **Velocity filtering.** Remove the *inbound normal* component of the relative
  velocity (`v ← v − min(0, v·n̂) n̂`) so the vertex doesn't re-penetrate next
  substep; keep the tangential component for sliding.
- **Friction.** Coulomb friction on the tangential relative velocity — clamp the
  tangential change by `μ` times the normal impulse. Cheap to add per contact and
  makes the cloth grip vs slide on the dragged collider, which is most of the
  *feel* of a cloth brush.
- **Move with the collider.** Because the constraint is expressed against the
  primitive's current pose, a vertex in contact is naturally dragged along as the
  collider moves — the brush "pushes" the cloth. Give the contact the collider's
  surface velocity when computing friction so a rotating/translating cylinder
  imparts the right tangential drag.

### 6.10 GPU mapping of the collision pass

The whole primitive-CCD pass is close to ideal for the GPU:

- **One thread per (active) vertex.** Each vertex independently: transform to
  local frame, run the closed-form TOI / few-iteration bisection, apply the
  response. No inter-thread communication, no atomics, no divergence beyond a
  couple of clamps/branches.
- **Collider in uniform/constant memory.** The primitive's pose, radius, height,
  and thickness are a tiny uniform block (`DrawCommand`/`set=2` cadence in the
  rendering model, or a brush-command uniform) — read by every thread, cached.
- **Restrict to the brush region.** Only test vertices in spatial-tree leaves
  whose bounds overlap the swept collider AABB (dilated by `r + h + |u|`). This
  is the same broad-phase-by-spatial-node restriction dyntopo uses to stay
  O(brush region) rather than O(mesh), and it means the collision pass touches
  only the handful of leaves under the brush.
- **Fold into the substep loop.** Order per substep: integrate → elastic
  constraint pass(es) → **primitive-CCD + response** → velocity update. With fine
  substeps the per-step motion is small, so even a single collision pass per
  substep is robust.

### 6.11 Summary of the collision recipe

For the sculpt-brush cloth: **substep the solver; represent the dragged collider
as a capsule (or rounded cylinder) with a closed-form SDF; per substep, per
vertex in the brush-region leaves, solve the moving-point-vs-static-SDF TOI in
the collider's frame (quadratic for sphere/side-wall, short bisection /
conservative advancement for capsule/cylinder); respond with an XPBD collision
constraint plus normal-velocity filtering and Coulomb friction against the
collider's surface velocity.** It is cheap, robust to fast strokes, and maps onto
the existing spatial-tree + compute-brush machinery with no mesh–mesh CCD.

---

## 7. Recommendations for a sculptcore cloth mode

Ranked, and all **[hypothesis]** — starting points, not decisions:

1. **Solver: XPBD with small-step substepping, optionally Chebyshev-accelerated.**
   Unconditionally stable, physical stiffness parameters, fixed frame budget with
   graceful softening, no global prefactor to rebuild when the cloth is
   dyntopo-remeshed, and it maps directly onto the compiled-kernel brush executor
   (a per-color / per-substep compute dispatch over affected spatial leaves).
   Chebyshev (Wang 2015) is a near-free convergence boost on top.
2. **Parallelism: graph-colored Gauss–Seidel (Vivace-style)** for deterministic,
   atomic-free constraint solves — aligns with the engine's bit-reproducibility
   discipline (`sbrush-verify`). Recolor only on topology change.
3. **Collision: analytic-primitive CCD as in Section 6** — capsule/rounded-cylinder
   collider, moving-point-vs-SDF TOI, XPBD contact constraints + friction,
   restricted to brush-region spatial leaves. Defer robust cloth self-collision
   to a later phase (start with per-vertex thickness repulsion or none).
4. **Region restriction via the spatial tree**, exactly like dyntopo's `seedVerts`
   — only simulate and collide the leaves near the collider, keeping cost
   O(brush region).
5. **Validation harness:** A/B a fast-solver stroke against an offline
   **IPC / Newton** reference the way `sbrush-verify` A/Bs brush backends, and
   reuse the debug-app `save_pos`/`assert_pos` idea for drape-stability
   regression scripts.
6. **If large low-frequency drape is too slow later**, add a **multigrid** global
   step seeded by a coarse cloth proxy off the existing decimation hierarchy
   (Section 3.9) — but only if a flat Chebyshev-XPBD proves insufficient.

Progression: start at (1)+(3) restricted to a brush region for a usable cloth
brush; add (2) for determinism/speed; add (6) only if draping scale demands it.

---

## References (representative, by topic)

**Integration / implicit Euler**
- Baraff & Witkin, *Large Steps in Cloth Simulation*, SIGGRAPH 1998.
- Martin, Thomaszewski, Grinspun & Gross, *Example-Based Elastic Materials*, 2011 (variational view); Gast et al., *Optimization Integrator for Large Time Steps*, 2015.

**Position-based / projective / ADMM**
- Müller, Heidelberger, Hennix & Ratcliff, *Position Based Dynamics*, 2007.
- Macklin, Müller & Chentanez, *XPBD*, MIG 2016; Macklin et al., *Small Steps in Physics Simulation*, SCA 2019.
- Bouaziz, Martin, Liu, Kavan & Pauly, *Projective Dynamics*, SIGGRAPH 2014; Liu, Hao, Zhang & Sifakis, *Mass–Spring... Fast Simulation*, 2013.
- Overby, Brown, Li & Narain, *ADMM ⊇ Projective Dynamics*, IEEE TVCG 2017.

**Descent / acceleration on GPU**
- Wang, *A Chebyshev Semi-Iterative Approach...*, SIGGRAPH 2015.
- Wang & Yang, *Descent Methods for Elastic Body Simulation on the GPU*, SIGGRAPH Asia 2016.
- Liu, Bouaziz & Kavan, *Quasi-Newton Methods for Real-Time Simulation of Hyperelastic Materials*, ACM TOG 2017.
- Peng, Deng et al., *Anderson Acceleration for Geometry Optimization and Physics Simulation*, SIGGRAPH 2018.

**Parallel Gauss–Seidel / coloring**
- Fratarcangeli, Tibaldo & Pellacini, *Vivace: a Practical Gauss–Seidel Method for Stable Soft Body Dynamics*, SIGGRAPH Asia 2016.

**Collision / CCD / contact**
- Bridson, Fedkiw & Anderson, *Robust Treatment of Collisions, Contact and Friction for Cloth Animation*, SIGGRAPH 2002.
- Teschner et al., *Optimized Spatial Hashing for Collision Detection of Deformable Objects*, VMV 2003.
- Karras, *Maximizing Parallelism in the Construction of BVHs (LBVH)*, HPG 2012.
- Mirtich, *Conservative Advancement*; Zhang, Redon, Lee & Kim, *Continuous Collision Detection for Articulated Models Using Taylor Models and Temporal Culling*, 2007.
- Li, Ferguson, Schneider, Langlois, Zorin, Panozzo, Jiang & Kaufman, *Incremental Potential Contact (IPC)*, SIGGRAPH 2020.
- Inigo Quilez, *Distance Functions* (closed-form primitive SDFs: sphere, capsule, cylinder, box, plane).
