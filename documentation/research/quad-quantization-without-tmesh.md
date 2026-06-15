# Paper notes: Quad Mesh Quantization Without a T-Mesh

Coudert-Osmont, Desobry, Heistermann, Bommes, Ray, Sokolov — Computer Graphics
Forum 2023, [10.1111/cgf.14928](https://doi.org/10.1111/cgf.14928), HAL
[hal-04395861](https://inria.hal.science/hal-04395861v1). CC BY-SA 4.0.
PDF: [`quad-remeshing-without-tmeshes.pdf`](quad-remeshing-without-tmeshes.pdf)
(same directory).

Why these notes exist: this paper sits on the *other side* of the strategic
fork that [`../plans/miq.md`](../plans/miq.md) names — it improves the robust
**QGP (T-mesh) quantization family**, whereas our shipped quantizer
(`quantize/quantize_ilp.cc`) is a member of the **MIQ rounding family** that
family contrasts against. It is already cataloged once, in
[`layout-embedding-optimization.md`](layout-embedding-optimization.md) (line ~91,
"the no-T-mesh alternative"); this note records the full comparison against our
implementation so the fork analysis lives next to the PDF. Read alongside the
shipped pipeline ([../quad-remeshing.md](../quad-remeshing.md)) and the MIQ
plan ([../plans/miq.md](../plans/miq.md)).

---

## Scope of the paper (narrow on purpose)

Despite the broad title, the contribution is **one pipeline stage**: the
*quantization step* of the standard integer-grid-map (IGM) pipeline. It does
**not** touch frame/cross-field design, seamless-map solving, or quad
extraction — given a seamless map, it emits the linear constraints `[A, ω]` that
pin the integer DoF (Algorithm 1, line 2). Quantization is the spiral-elimination
step: snap the seamless map's real cut-edge transitions to the integer lattice so
every iso-line closes.

The reference it improves on is **QGP** (Campen et al. 2015), the robust
quantization standard:

- QGP builds a **T-mesh proxy** by tracing a motorcycle graph (iso-curves from
  singularities → rectangular charts), optimizes integer **edge lengths** `ℓ_h`
  by validity-preserving atomic operations (±1 the lengths along a loop in the
  chart-adjacency graph; Dijkstra weights favor lengths near the seamless ones),
  then converts the chosen lengths back to constraints on the original map.

The paper swaps the T-mesh for a **decimated triangle mesh**:

1. **§4.1 Decimate** (Bommes 2013): collapse every collapsible edge (never
   removing a singularity, manifold-preserving, CCW-in-map), flip edges that
   violate the in-map Delaunay criterion, repeat to convergence. Cuts are moved
   out of the affected region before each op (Fig 7). Every op is a linear
   expression in the old map coords, chained into a matrix `D` with
   `Uproxy = D·Useamless`.
2. **§4.2 Optimize integer edge geometry** `ω_h = U⁻_next(h) − U⁻_h ∈ ℤ[i]` —
   Gaussian-integer **vectors**, not just lengths. The atomic operation is a
   cycle in the dual edge graph `G` of the quad covering with a per-edge
   `α_h ∈ {1,i,i²,i³}`; richer than QGP (one cycle can insert *and* delete
   quads — the Bommes 2011 q-helix removal). Path search is Dijkstra on the
   *dual-of-dual* graph (validity is path-dependent: it depends on the current
   *and* previous edge), with lexicographic weights `w = x + y/ε + z/ε²`
   (energy-decreasing < energy-increasing < validity-breaking).
3. **§4.2.3 Init**: the seamless map scaled by `2ⁿ` is grid-preserving for large
   enough `n` (a certified-valid start); round to the nearest nonzero Gaussian
   integer, repair closeness by pairing triangles with paths in `G`, bump `n`
   and retry if the result is still invalid.
4. **§4.3 Move back**: `ω_h = (D_next(h) − D_h)·U_grid` constrains the final
   grid-preserving solve; pin one singularity at 0.

**The authors' own honesty (§5.2, Conclusion):** *"we do not improve the
state-of-the-art robustness nor quality."* The win is **simplicity and
flexibility** — off-the-shelf triangle tooling instead of T-mesh machinery, and
edges that need not be axis-aligned in the map. The flexibility payoffs in §6
(free boundaries with no explicit alignment; enforcing features *at* the
quantization step; representing aligned singularities without degenerate facets;
coarse-to-fine transfer and direct extraction on the decimated mesh) are mostly
**future-work sketches on CAD models**, not a shipped system.

## Where our quantizer sits

`computeQuantization` (`quantize/quantize_ilp.cc`, ~2400 lines) is **not** in
this paper's family. It is a **penalty-formulation MIQ rounder** — the Bommes
2009/2012/2013 *round + re-solve* lineage:

- Rebuild the seamless system in `2M` class space; seam consistency + integer
  locks are `1e6` soft quadratics; CHOLMOD LDL′ + incremental `cholmod_updown`
  (native) / Eigen refactor (WASM).
- **Greedy most-confident-first batch rounding** (`confidence_radius = 0.3`),
  lazy heap, local-GS tier, GREEDY/DIRECT strategy.
- Folds (the linear rounded map is not injectivity-guaranteed) are **repaired
  after the fact**: field-clamped ARAP untangle continuation, Tier-1b
  seam-integer ±1 relaxation, Bommes-2013 local-injectivity stiffening, Tier-3
  local fold-patch reparam, plus a non-crashing `feasible=false` fallback.

The paper's §3 puts exactly this lineage under "Rounding" and notes it "[does]
not provide robustness in the general case" — i.e. we deliberately chose the
formulation the paper contrasts *against*, shipped it, and gate it on a corpus.

Two naming red herrings in our tree, since they look related but are not:

- **`quantize/t_mesh.{h,cc}` is not a T-mesh.** Its header says so: it is the
  `QuantGraph` realizing the IGM structure *directly on the M4 cut graph* (cut
  edges = "sides," cut cycles = "loops"). No motorcycle graph, no chart layout.
- **`fast_decimate` / `decimateForSolve` (`remesh.cc:501`) is a preview speed
  hack**, not the paper's quantization proxy: it decimates the *whole working
  copy* to a coarser solve resolution. The team removed the standalone
  quality-blind `--solve` decimation at Tier 9 in favor of the field-aligned
  pre-remesh.

## Head-to-head on the quantization step

| Axis | Paper (Coudert-Osmont) | Ours (`computeQuantization`) |
|---|---|---|
| Family | QGP / robust quantization (combinatorial) | **MIQ rounding** (Bommes 09/12/13) — the family §3 calls "not robust in the general case" |
| Proxy | Decimated triangle mesh; map back via `D` | **No proxy** — quantizes the full-res cut/class system directly |
| Integer DoF | Edge geometry vectors `ω_h ∈ ℤ[i]`, optimized by graph cycles | Translations/class positions, optimized by greedy rounding + linear re-solves |
| Validity | **Maintained** by validity-preserving atomic ops; certified-valid init | **Repaired afterward** (ARAP / stiffening / Tier-1b / Tier-3) |
| Robustness | Provable (QGP-class) | Best-effort + `feasible=false` fallback + the Tier 0–9 empirical ladder |
| Resolution scaling | Integer solve is resolution-independent (Fig 15: ~49 ms regardless); only decimation scales | Scales with full-res class count |
| Target domain | CAD (ABC/MAMBO, feature-curve-heavy) | Sculpting/organic + 5M-tri dyntopo; cad/hard-surface presets exist but aren't the center of gravity |

## What adopting it would (and would not) buy us

Adopting it means **replacing our entire quantization formulation**, retiring the
Q1–Q4 MIQ investment (lazy heap, local-GS, GREEDY/DIRECT, the `cholmod_updown`
machinery). It is an architectural rewrite, not a bug fix.

- **Would NOT buy:** field, seamless, or extraction (we have all three, with more
  scaffolding). And by the authors' own admission, **no robustness or quality
  gain over QGP** — hence none over a well-tuned MIQ either.
- **Could genuinely help:**
  - *Resolution-independent quantization cost* — we quantize full-res; their
    integer solve does not grow with input size (Fig 15). Relevant only if
    quantize wall-clock ever dominates at high res (today ARAP dominates, not
    rounding — see miq.md Q4).
  - *Free boundaries* without explicit boundary alignment (§6.1).
  - *Feature-enforcement at the quantization step* (§6.2) — matters precisely
    where our "hard-pin features early, in cross-field + seamless" approach makes
    the seamless map distort or fail on over-constrained feature networks. This
    is our weakest link for hard-surface/CAD input.
- **Caveats the paper itself raises:** its quantization scales *worse* than QGP
  w.r.t. proxy size (Fig 14-bottom), and it has a pathological 50×-slower outlier
  (Fig 16: a singularity snapping onto a feature edge needing scale 64). The
  atomic-operation machinery (Dijkstra on the dual-of-dual graph with the
  `x + y/ε + z/ε²` lexicographic weights) is genuinely fiddly.

## Bottom line / recommendation

We are on the opposite side of this paper's premise. This is **not** "we
implemented an older version of the paper" — it is "we chose a different
quantization philosophy, shipped it, gated it on a corpus, and the team has
already deferred the QGP/no-T-mesh fork."

Per miq.md's own gate, moving to this family is justified only if the corpus
starts showing `feasible=false` / residual failures *at a rate that matters* —
which the Tier 0–9 ladder currently prevents on organic input. The strongest
*specific* reason to revisit it is **not** robustness or quality; it is
**feature-enforcement-at-quantization for hard-surface/CAD inputs**. If
hard-surface quad output becomes a priority, this paper (plus the Lyon
2019/2021 free-boundary line it builds on) is the right reference for *that*
sub-problem — adopted as a CAD-path quantizer, not a wholesale replacement of
the organic pipeline. Otherwise it stays a read-not-build reference.
