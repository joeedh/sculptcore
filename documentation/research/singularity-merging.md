# Singularity merging / noise-pair management

> **Status (2026-06-11).** Brainstorm/survey report — no decisions adopted yet.
> Companion to [`quantize-acceleration-survey.md`](quantize-acceleration-survey.md)
> (the quantize-pass cost analysis that motivates this). Claims tagged
> **[measured]** (our data), **[code]** (read off the implementation),
> **[literature]**, or **[hypothesis]**.

## Motivating question

Spurious (noise) singularity pairs bloat the integer problem, co-locate with
parametrization folds, and feed the untangle/Tier-1b churn that dominates the
quantize tail. Can we detect them inside (or downstream of) the quantization
phase and merge / delete / re-optimize them — and is pair cancellation the
only mechanism?

## 1. How singularities travel through quantization today

The pipeline treats singularities as **boundary conditions, not variables**
**[code]**:

- Born in M2 as vertices whose one-ring period jumps don't cancel
  (`.remesh.v.pole_index`, Σ = 4χ). M3 today is *only* the fixed-period
  Poisson re-smooth — the relocation/merge/split pass is explicitly deferred
  (`field/singularity_adjust.h:13`, the XXX note).
- M4's cut graph must pass through every singularity (`param/cut_graph.h:9`),
  giving the seamless map its holonomy: transitions composed around a cone
  yield a net 90°-multiple rotation, not the identity.
- **M5 never rounds cone positions.** The integer variables are per-cut-side
  translations only (`quantize/t_mesh.h:29`); the only penalties are
  `lam_seam`/`lam_fix` (`quantize_ilp.cc:148`). The seam constraints at a
  cone force its UV to the *fixed point* of the composed transition
  `x ↦ R·x + t_total`; for `R = ±90°`, `det(I−R) = 2`, so with integer
  translations the cone lands on the **half-integer lattice** in general —
  integer only by parity luck. (Classic Bommes MIQ rounds singularity UVs
  first, as explicit integer variables, precisely to avoid this
  **[literature]**.)
- Consequences accepted downstream **[code]**: `loopClosureResidual` skips
  singular vertices (`t_mesh.cc:148` — the no-spiral guarantee is a statement
  about regular vertices); cone-1-ring folds are classified cosmetic
  (`quantize_ilp.h:64`); extraction skips rasterizing cone 1-rings entirely
  and **caps** the resulting even-gon holes with a quad fan centered on the
  cone (`extract/quad_extract.cc:296`, `:532`) — that fan center is where the
  cone finally becomes a valence-3/5 vertex. Odd rims are the documented
  capped-cylinder caveat.

Two structural facts follow: **singularity count is the true size of the
integer problem** (singularity-free inputs quantize near-trivially —
`t_mesh.h:6`; this is QGP's whole insight), and every noise pair the field
stage leaks is paid for again in M5 (folds, untangle, Tier-1b) and M6 (caps).

## 2. Detection: the good signal only exists downstream

Three tiers of "is this pair noise?", in increasing quality:

1. **Combinatorial (exists today):** `findSingularityPairs` — opposite-index
   pairs within k vertex hops (`singularity_adjust.h:57`, the Tier-5 gate
   diagnostic, plumbed into `QuantizeStats` at `quantize_ilp.cc:1395`). A
   proxy: hop distance knows nothing about target resolution.
2. **Metric (the criterion this report proposes):** the pair's **UV
   separation measured against the lattice**, computable as soon as
   `buildSeamlessSystem` has run (`quantize_ilp.cc:129`) — i.e. inside the
   quantize phase but before anything expensive. A pair under ~1–1.5 grid
   units apart is **sub-resolution: the output lattice cannot represent the
   two cones as distinct irregular vertices regardless of what the rounder
   does**. Merging such a pair is free by definition — nothing real is lost
   at the current `target_edge_length` (it correctly reappears at a finer
   target). **[hypothesis — criterion; the representability argument is
   exact]**
3. **Symptomatic (exists as diagnostics):** fold/pair co-location BFS
   (`quantize_ilp.cc:1412`), Tier-1b churn concentrated near a pair, the
   untangle threshold tripping. Right role: a *rescue trigger* in the house
   pattern of the ARAP continuation — inert on clean inputs.

## 3. Where the edit must live: feedback restart, not in-place

By quantize time the singularity structure is baked into corner classes,
gauges, the cut graph, and the sparse pattern; removing a pair changes class
*topology*, so there is no `cholmod_updown`-style in-place system edit
**[code]**. But at the **period-jump level** the edit is cheap: annihilating
an opposite-index pair = flipping `.remesh.e.period` along a dual path
connecting the poles (index sum conserved automatically), then re-running
`adjustSingularities` — whose fixed-period Poisson is exactly "smoothest
phase for a given period set." The shape that falls out:

1. Run the pipeline through `buildSeamlessSystem` (cheap: `setup_ms` ≪
   ARAP/rounding/Tier-1b **[measured]**, see the acceleration survey).
2. Compute UV separations for the Tier-5 pair list. No sub-resolution pairs →
   proceed; the path is inert on clean inputs.
3. Otherwise: bail before the initial factorization, flip periods along the
   shortest dual path per pair (deterministic tie-break off `seed`), re-run
   the M3 Poisson, rebuild cut graph + seamless system, retry. Bounded (2–3
   attempts; each annihilation removes two poles, so termination is
   monotone).
4. Never annihilate a pole carrying `.remesh.v.pole_pinned`.
5. Restrict to opposite-sign pairs initially; same-sign merges (two +¼ → one
   +½, a valence-2 cone) are occasionally right but belong to a solver-level
   mechanism (§4, QGP collapse constraints), not a hand rule.

Risks: the period-flip path chooses where the field's rotation discontinuity
goes (shortest path keeps it local); genuine micro-features are protected by
the criterion itself (sub-resolution pairs were unrepresentable anyway).

## 4. Is pair cancellation the only mechanism? No — four altitudes

Unifying observation: at the field level, *every* singularity edit — cancel,
relocate, merge — is the same primitive, **period flips along a dual path**
(relocation = transport along the path; cancellation = transport onto an
opposite pole; merge = transport onto a same-sign pole, indices adding). The
path-flip + Poisson-re-smooth machinery buys the whole family.

**Altitude 1 — prevention (don't give birth to them).**
- *Confidence-weight curvature constraints by anisotropy*: where
  `|kmin − kmax|` is small the principal directions are numerically random,
  and the field manufactures pole pairs to follow them. Down-weight or drop.
  Cheap; attacks the root cause. **[hypothesis, standard practice]**
- *Solve the field on the decimated mesh*: a coarse solve mesh physically
  cannot host sub-resolution pole pairs — the noise wavelength doesn't exist
  there. The decimate-by-default recommendation (acceleration survey §6.1)
  paying a second dividend; likely the highest payoff-per-effort item here.
- *Global smoothness-vs-alignment weight*: blunter — trades feature fidelity
  everywhere; less attractive.

**Altitude 2 — field-level editing (this report's §3).** Beyond incremental
cancellation, the principled generalization is **prescription**: choose the
final pole set (filter by spacing/confidence, or adopt the pole set of a
coarser solve) and derive a consistent period assignment in one shot — M3's
fixed-period Poisson is a discrete trivial-connection solve (Crane et al.) in
everything but name **[literature]**. Optimal cone placement as sparse/L0
optimization exists in the literature (Soliman et al. 2018 for conformal
cones; integer-holonomy variants for quad fields) but a spacing+confidence
filter is likely sufficient. Incremental cancellation is the right v1;
prescription the v2 if greediness shows.

**Altitude 3 — quantization-level (the solver decides).** QGP's zero-length
T-mesh arcs *are* the merge operation, chosen jointly with the lattice under
validity constraints that forbid illegal collapses **[literature]**. This is
the only altitude where merging is decided with knowledge of what the lattice
needs — altitude 2 has to guess what the quantizer will want. Subsumes
opposite-sign cancellation and does same-sign merges where genuinely right.
Long-term home of the decision; also fixes cone *placement* structurally
(integer arc lengths put every cone on the lattice — the half-integer
fixed-point issue and the cap-rescue of §1 dissolve).

**Altitude 4 — output-level cleanup.** Bommes et al. 2011 *Global Structure
Optimization of Quad Meshes*: cancel/relocate irregular-vertex pairs in the
extracted quad mesh via grid-preserving local ops; Tarini-style base-complex
simplification is the coarser cousin **[literature]**. Fixes appearance and
downstream usability; does nothing for quantize cost (the money is already
spent). A polish pass, orthogonal to the performance question.

## 5. Recommendations

1. **Measure first (one corpus run, zero code):** correlate the existing
   Tier-5 manifest counters (`close_pairs`, `clutter_verts`) with quantize
   wall-clock, untangle triggers, and `tier1b_probes` across the corpus —
   establishes whether noise pairs are a performance problem or only a
   quality problem.
2. **Turn on decimate-by-default and re-measure** — it may delete most noise
   pairs as a side effect, shrinking the altitude-2 work to a residual
   rescue path.
3. **Build the altitude-2 pass** (period-flip pair cancellation gated on the
   sub-resolution UV-separation criterion, structured as the §3 feedback
   restart). Low-regret: the primitive is also the relocation tool, and the
   detection criterion + flip machinery are exactly what a future QGP
   T-mesh construction and collapse-constraint set need — nothing is thrown
   away on migration.
4. **Leave same-sign merging and joint merge/lattice decisions to QGP**
   (acceleration survey §6.4); leave output-level GSO-style cleanup as an
   eventual quality pass, out of scope for quantize cost.
