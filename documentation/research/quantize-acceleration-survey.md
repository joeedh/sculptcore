# Quantize-pass acceleration survey

> **Status (2026-06-11).** Brainstorm/survey report — no decisions adopted yet.
> Grounded in the measured Q0–Q4 / parallelization-pass (A–E) data in
> [`plans/miq.md`](../plans/miq.md); claims below are tagged **[measured]**
> (our own A/B data), **[literature]**, or **[hypothesis]**. Companion to
> [`miq-rounding-report.md`](miq-rounding-report.md) (the CoMISo comparison)
> and [`singularity-merging.md`](singularity-merging.md) (noise-pair
> detection/merging, a complementary cost lever).

## Motivating question

Our MIQ-style quantization is the wall-clock bottleneck of the quad remesher:
the worst corpus-scale run is **953 s of a 962 s quantize at 73k corner
classes — almost entirely Tier-1b probes** (75,752 probes × ~50 ms back-solve)
**[measured]**, and on typical assets ARAP dominates (~74% of quantize on
simple-closed) **[measured]**. ZRemesher remeshes an 800k-triangle mesh in
10–20 s. The gap is 2–3 orders of magnitude — too large for constant-factor
engineering, so this report asks what ZRemesher is plausibly doing instead,
what in our pass is *inherently* serial, and which acceleration routes keep
our guarantees.

## 1. What ZRemesher is probably doing (and not doing)

**It almost certainly does not solve an integer-grid map [hypothesis, strong
behavioral evidence].** ZRemesher is famous for producing *spiraling edge
loops* (endemic in v1/2, only mitigated in v3, with guide curves offered as
the manual workaround). Spiral iso-lines are precisely the failure mode
integer quantization eliminates structurally — a method holding a valid IGM
cannot produce them. Corroborating tells: approximate-only target polycount
(±50% routine), irregular-vertex clusters not confined to field
singularities, and guide curves / density painting integrating the way
local/greedy methods allow.

Two architecture families fit the speed and the artifacts:

- **Instant-Meshes-style local fields** (Jakob et al. 2015) **[literature]**:
  orientation *and* position fields solved by multiresolution Gauss–Seidel on
  a decimation hierarchy; the integer (lattice-translation) DOFs are rounded
  *locally per edge during smoothing* — there is no global integer problem at
  all. O(n), tiny constants, embarrassingly parallel; defects (T-junctions,
  spiral-like drift) are patched heuristically at extraction.
- **Field-guided advancing front / streamline paving** **[hypothesis]**:
  trace integral curves of a cheap cross field and pave quads between them.
  Cost scales with *output* quad count, not input triangle count.

Either way: local decisions + relaxation + cleanup, not global optimization
with proofs. ZRemesher's speed comes from **not solving our problem** — its
output contract is weaker (no spiral guarantee, no exact-quad-count lattice).
It also almost certainly never touches the full-res mesh in any solve
(ZBrush owns excellent decimation; an internal ~50–100k proxy is the obvious
pipeline) **[hypothesis]** — which we have plumbed (`solve_edge_length`) but
default-off.

## 2. Why ARAP is in our pass (context, all previously documented)

The ARAP continuation is an **injectivity rescue, not a quality polish**: the
seamless map at `lam_seam = 1e6` folds ~34% of faces on rounded organic
inputs, the folds are constraint-induced (they scale *with* the seam weight;
exact elimination is the stiff limit and keeps them — the Lesson-1 correction
in `plans/miq.md`) **[measured]**, and the `0.1 → 1e6` ramp (16 steps × 3
local-global inner solves) keeps the map injective as seams tighten. ARAP
specifically because (a) the local-global split keeps the matrix fixed within
a step — inner solves are RHS-only back-solves against the kept factor, (b)
the continuation *requires* soft seams (`lam_seam` as a dial), and (c)
rotation-fitting is simultaneously the untangle energy and the quality energy
for a field-aligned isometric grid map. Its cost is the ~16 refactorizations
the ramp forces. **Any acceleration plan must keep an untangle path** —
geometry prefiltering was tried and reverted (folds live in the
parametrization, not the input tessellation) **[measured]**.

## 3. QGP / T-mesh quantization keeps the no-spiral guarantee

Spirals come from *fractional* translational DOFs across cuts; any method
ending with all translations integer (a valid IGM) is spiral-free by
construction. QGP (Campen, Bommes, Kobbelt 2015) **[literature]** ends there
too — it changes only *where* the integer problem lives:

- **Ours (MIQ-style):** integers per cut-edge translation on the full mesh —
  the 2M corner-class system, ~73k classes on bad inputs, greedy rounding
  interleaved with global re-solves.
- **QGP:** compute the seamless map (continuous), trace its **motorcycle
  complex** (separatrices from every singularity through the param) into a
  coarse T-mesh of rectangular patches, and quantize the **arc lengths** of
  that T-mesh under hard consistency constraints (opposite patch sides equal,
  non-negativity). Integer count scales with *singularity count* (typically
  hundreds), not edges. One final continuous solve with integers fixed yields
  the IGM; QEx extraction is unchanged. The 2023 min-deviation-flow
  reformulation (bi-directed graph flow) solves the quantization to
  optimality fast **[literature]**.

The guarantee situation is arguably *stronger* than ours: validity is
combinatorial (no `feasible=false` failure mode, which our penalty rounding
has on messy inputs). Caveats:

1. **Zero-length arcs** collapse T-mesh patches — graceful coarsening, but
   bad collapses can merge singularities into higher-index irregular
   vertices; constraint sets exist to forbid them.
2. **It needs a traceable (injective-enough) seamless map** — so the ARAP
   continuation stays, *before* tracing. QGP replaces the rounding loop (the
   953 s tail), not the untangle. The final fitting solve is one matrix, one
   factorization — the per-round refactor/updown machinery evaporates.
3. **Engineering risk concentrates in robust motorcycle tracing**
   (degenerate edges, near-singular regions, boundaries); the integer solve
   itself becomes almost trivial. `quantize/t_mesh.cc` is a foothold; the
   motorcycle complex is a genuinely new component.

## 4. The inherently serial parts of the quantize pass

Phase map (counters in `quantize_ilp.cc`): `setup → initial_factor → arap →
rounding → tier1b → stiffen → tier3`. The A–E parallelization pass already
widened everything wide (per-face sweeps, assembly scatter-add, the 4-RHS
probe batch); what remains is structural, in two flavors — **algorithmic
chains** (step N+1 needs step N's solution) and the **serial kernel** (the
sparse triangular solve).

| part | serial structure | lever |
|------|------------------|-------|
| ARAP continuation | chain by definition (continuation: each ramp step retargets off the previous step's Jacobians); 16 × 3 links | shorten: adaptive ramp / early-out once folds < gate |
| greedy rounding rounds | sequentially adaptive on purpose (confidences exist only after the previous solve); round count = chain length | delete: QGP/T-mesh removes the loop; DIRECT is the 0-round limit but goes `feasible=false` on messy inputs **[measured]** |
| Tier-1b | greedy walk with state mutation on *accept* (updates `x`/`t_int`/`curFold`); rejected probes mutate nothing | speculative multi-candidate batching (see §5); acceptance-aware early-out |
| simplicial back-solve | the kernel under everything: critical path = etree depth; threads make it *slower* at 73k (1T 962 s vs 8T 1074 s) **[measured]**; supernodal loses 4× at corpus scale (fronts too small for BLAS-3) **[measured]** | multi-RHS blocking; shrink N |
| `cholmod_updown` | serial along the modified column's etree path | removed with the rounding loop |
| confidence heap / local-GS tier / stiffen+tier3 | serial but minor (heap re-keys incremental post-Q2; GS exists to dodge refactors, chiefly WASM; stiffening rounds are short) | — |

Net: **wall-clock ≈ (chain length) × (back-solve cost)**, and thread-level
parallelism is measured to be tapped out at corpus scale. Wins must come from
shortening chains, blocking the kernel, or shrinking N.

## 5. Alternatives to the simplicial triangular solve

**Direct-land (exact, determinism-safe):**

- *Supernodal / multifrontal* — measured and shelved: 2.3× win ≥ ~18k
  classes, 4× loss at corpus scale; opt-in (`use_supernodal`) **[measured]**.
  Explicit domain-decomposition/Schur variants are the same idea with more
  machinery.
- *Level-scheduled parallel back-solve* — thin etree levels near the root are
  the critical path; our 1T-beats-8T data is this effect. Marginal.
- *Multi-RHS blocking* — **the one direct-land lever that works**: amortizes
  the serial factor traversal across columns. The Tier-1b speculative batch
  is the prime application: probe K candidates as one 4K-column `solveBatch`
  against the current (unchanging) factor, walk results in the existing
  deterministic order, accept by the existing rule, discard speculated
  results invalidated by an acceptance, re-batch. Identical output; pays off
  in proportion to the rejection rate. **Pre-req measurement: add an
  `accepts` counter next to `tier1b_probes` and re-run the 73k asset.**
- *Sparse-RHS / subset solves* (`cholmod_solve2` with `Bset`/`Xset`) — a
  Tier-1b probe changes only a handful of RHS entries; the forward solve then
  touches only their etree reach (~O(√N)). Exact and available — but we
  currently consume the full dense solution (global fold scan / residual
  max), and Q1 measured that perturbations spread globally under the 1e6
  coupling, so truncated-support *evaluation* would be an approximation
  needing its own A/B.

**Iterative-land (the throughput bet):**

- *PCG* — every iteration is a parallel SpMV; no serial etree path. Threats:
  the 1e6 penalties put ~1e6 contrast in the spectrum and PCG's rate goes as
  √κ (warm starts shrink the initial residual, **not** the rate); byte-
  determinism needs fixed iteration order + chunk-ordered reductions (pattern
  already owned). Pairs naturally with the gated Q5 exact elimination, which
  removes the stiff penalty from the spectrum. Cost arithmetic: one back-solve
  ≈ 2·nnz(L); one PCG iteration ≈ nnz(A) + preconditioner; with mesh-like
  fill, PCG wins iff it converges in ~3–8 warm-started iterations — a
  measurable, not guessable, number.
- *Geometric multigrid* — great for the field stages (the Instant-Meshes
  trick), awkward for quantize (seam penalties don't coarsen geometrically);
  AMG sidesteps that algebraically but is a heavyweight dependency.
- *GPU* — triangular solve is the same serial dependency, worse on GPU; GPU
  PCG is the sensible version and is out of scope for a WASM-paritied
  pipeline.

**Hybrid PCG-then-direct (proposed):** the pass already contains the exact
control structure — the local-GS tier is "iterate until it stops improving,
then escalate to the direct solve" (visit cap → escalation; final solve
always direct). The proposal is swapping GS → PCG in that slot. Q1's negative
finding does **not** transfer: GS failed because the 1e6 coupling spreads
perturbations globally and GS moves information one neighborhood per sweep;
CG moves information globally every iteration. Design points:

- Stagnation trigger: residual-ratio bail (<~10–25% reduction per iteration
  for 2–3 iterations) or hard cap ~8–10; monitor in a *diagonally-scaled*
  norm or the 1e6 rows dominate it.
- Tolerance must tie to downstream decision thresholds (`tau`,
  `integer_tol`) — approximate solutions feed lock/accept decisions; the GS
  tier already set the precedent (deterministic algorithm, quality A/B'd, not
  bit-identical to the direct path).
- Targets, in order: Tier-1b probes (warm start ideal, competes with the
  50 ms kernel — but do speculative batching first), per-round settles
  (drop-in for the GS slot), ARAP inner solves (loose tolerance needs).
- Experiment: Jacobi-preconditioned warm-started PCG, 8-iteration cap, wired
  into the GS escalation slot, A/B on the 73k asset + corpus à la Q1. Decision
  number: warm-started iterations to decision-grade tolerance. >10 with
  Jacobi → "needs IC(0) or Q5 first"; >10 with IC(0) → shelve with a measured
  rejection next to Q3.

## 6. Recommendations (effort-to-payoff order)

1. **Decimate-by-default** — auto-derive `solve_edge_length` from
   `target_edge_length` instead of default-off. ~10× on dense inputs, zero
   new algorithm risk, already shipped behind a flag. Shrinking N is the only
   thing that helps a serial kernel directly.
2. **Tier-1b speculative multi-candidate batching** — attacks the measured
   953 s tail with byte-identical output. Instrument `accepts` first (one
   counter, one corpus run) to confirm the rejection-dominated regime.
3. **`cholmod_solve2` sparse-RHS forward solves** for probe deltas — exact,
   cheap to try, compounds with (2).
4. **QGP / T-mesh quantization** — the strategic answer: integer problem
   shrinks from mesh scale to singularity scale, the rounding loop (and its
   refactor/updown machinery) disappears, the no-spiral guarantee is *kept*
   (and the `feasible=false` mode eliminated). Risk lives in motorcycle
   tracing; ARAP untangle stays, reordered before tracing. At T-mesh scale
   the simplicial back-solve stops being worth replacing at all.
5. **ARAP ramp diet** — adaptive step count / early-out under the fold gate;
   inherits the decimation win automatically.
6. **PCG hybrid** — tactical bridge only if the tail survives (2)–(4);
   conditioned on the experiment in §5, ideally after Q5 elimination fixes
   the spectrum.

Explicitly *not* recommended: treating ZRemesher as a better-engineered
version of this pipeline. The evidence says different algorithm class,
weaker output contract. If (1)–(4) land we are in ZRemesher's time class
*with* guarantees ZRemesher doesn't have.
