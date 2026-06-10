# MIQ Rounding — Applying the CoMISo Lessons

## Status

**Plan complete** (2026-06-10): **Q0 + Q1 + Q2 + Q4 + Q6 landed; Q3 measured
and rejected; Q5 remains gated** (its entry conditions are unmet — revisit only
if the corpus ever shows `feasible=false` at a rate that matters *and* the
T-mesh/QGP strategic fork resolves in favor of staying MIQ). Q0 baseline
archived at `tests/remesher-results/miq-q0-baseline/`; Q1 A/B at
`miq-q1-run1` (GS on) / `miq-q1-gsoff` (same-session direct path); Q2 runs at
`miq-q2-run{1,2}`; Q3 budget A/B at `miq-q3-budget-run{1,2}` + `miq-q3-control`;
Q4 at `miq-q4-run1` (default, byte-identical to Q2) + `miq-q4-direct`.
Q1's measurement: GS propagates globally under the `1e6` coupling and
escalates 43–61% of rounds — see the Q1 result block; Q5 stays gated. Q2's
lazy heap + touched-set re-key cut sideAvg evaluations ~4× with byte-identical
metrics — see the Q2 result block. Q3's cumulative error budget lost to the
confidence radius on rounds, wall-clock, and quality — measured, rejected,
removed; see the Q3 result block. Q4's GREEDY/DIRECT strategy enum landed with
the oracle ctest; DIRECT confirmed fast-but-infeasible on the corpus — see the
Q4 result block. Q6 docs cleanup done. Source analysis:
[`research/miq-rounding-report.md`](../research/miq-rounding-report.md)
(a read of the CoMISo mixed-integer stack at `c:/dev/CoMISo` — `MISolver`,
`ConstrainedSolver`, `ConstraintTools`, `IterativeSolverT`) against our
`quantize/quantize_ilp.cc`. This plan adopts the report's lessons **with one
correction and a reordering** (below).

## Context

`computeQuantization` is a penalty-formulation MIQ rounder: seam consistency
and integer locks are `1e6` soft quadratics on the `2M` corner-class system,
solved with CHOLMOD LDL' + incremental `cholmod_updown` rank updates (native)
or per-round `SimplicialLLT` refactorization (WASM), with greedy
vertex-independent batch rounding. CoMISo instead eliminates constraints
exactly up front and re-solves locally per round.

The report's per-lesson claims were verified against the code; the mechanics
all check out (line refs in the report are current). One causal claim does
not:

**Correction to Lesson 1.** The report expects exact seam elimination to make
"most folds disappear at the source." Our own measurement says otherwise: the
ARAP-fallback comment (quantize_ilp.cc, `lam_seam` walk) records folds scaling
*with* the seam penalty — ~0% at `lam_seam=0.1`, ~34% at `1e6` on a rounded
blob. A penalty at `1e6` approximates the exact constraint, and exact
elimination is the stiff limit, so the folds are **constraint-induced, not
penalty-induced** — exact elimination keeps (or worsens) them. This matches
the literature: Bommes 2013 local stiffening exists precisely because
exactly-constrained MIQ folds too. Consequences for this plan:

- Fold reduction is **not** a goal or expected benefit of any milestone here;
  the untangle machinery (ARAP continuation, Tier-1b, stiffening, Tier-3)
  stays.
- The ARAP continuation *requires* soft seams (`lam_seam` ramp 0.1 → 1e6) and
  cannot be expressed in an eliminated formulation — the exact route must keep
  a penalty phase for it.
- The exact route is therefore gated on measurement (Q5), not assumed.

## Lesson → verdict

| # | Lesson (report) | Verdict | Milestone |
|---|-----------------|---------|-----------|
| 4 | Local Gauss-Seidel re-solve instead of full back-solve per round | adopt — **do first**; it is the dominant-cost fix and its measurement decides the exact route | Q1 |
| 6 | Incremental confidence re-sort | adopt — falls out of Q1's touched-set | Q2 |
| 5 | Error-budget batching (cumulative 0.5) instead of fixed `confidence_radius` | adopt modified — keep vertex-independence (required under penalties) | Q3 |
| 7/8 | Rounding strategy enum (direct fast path / exact fallback) | adopt the enum + DIRECT path; no commercial solver | Q4 |
| 2 | Exact integer lock (kills `feasible=false`, structural no-spiral) | gated — done honestly it pulls in Lessons 1+3 (see Q5 design note) | Q5 |
| 1 | Exact seam-constraint elimination | gated — perf enabler only; fold claim rejected per the correction above | Q5 |
| 3 | GCD/pivot discipline for integrality-preserving elimination | gated — prerequisite of Q5; near-trivial for our ±1 rotation blocks except at singularity junctions | Q5 |

Ordering rationale: the report recommends the exact integer lock first. We
invert — Q1 (local GS) is cheap, formulation-preserving, helps both backends,
and its convergence measurement is exactly the evidence that tells us whether
the Q5 rewrite is worth its cost. If GS converges locally despite the `1e6`
coupling, most of the performance win is banked and Q5 becomes optional.

## Milestones

### Q0 — Instrumentation + corpus baseline (do first, cheap)

The rounding loop's costs are currently invisible (only `stats.iters`
survives). Add throwaway-resistant counters:

- Per-phase wall-clock in `QuantizeStats` (or a side struct): initial
  factorization, ARAP continuation, rounding rounds (split into
  assemble/updown/refactor/back-solve), Tier-1b, stiffening, Tier-3.
- Counts: rounds, full refactors vs updowns, back-solves, Tier-1b probes.
- Surface through the run report / manifest `run` block. **Not** into the
  corpus `metrics.csv` — that file is deterministic-columns-only; timing
  belongs in `results.json`.
- Housekeeping while there: delete the dead `QuantizeParams::max_iters`
  (declared, never read; the real cap is `max_rounds = S + 8`).

Baseline: `node tools/remesh_corpus.mjs` on the current branch, archived for
diffing. Record per-asset: quantize-stage ms, rounds, `feasible` rate, folds,
residual.

Gate: baseline `metrics.csv` byte-stable across two runs (determinism intact);
timing recorded per asset.

### Q1 — Local Gauss-Seidel re-solve tier (Lesson 4)

Per-round cost today is a full `cholmod_solve` over all `2M` unknowns (native)
or a full numeric refactorization + solve (WASM) even when one side was
locked. Add a CoMISo-style escalation: local GS → existing direct path.

- Keep the assembled matrix in CSR alongside the factor (today `assemble`'s
  matrix is dropped after factorization; new locks append their fix-penalty
  triplets to it incrementally — same data the updown columns are built from).
- After locking a batch: seed a work-queue with the classes touched by the new
  locks (the ≤4 class slots × 2 components per side), relax, push neighbors
  whose residual moved past tolerance. Queue empties → done; iteration cap →
  fall back to the existing updown + full back-solve (native) / refactor
  (WASM) path.
- **Backend-agnostic** plain-CSR implementation (no CHOLMOD dependency) so the
  WASM path — which currently refactorizes every round — gets the larger win.
- **Determinism:** process the queue in deterministic order (index-sorted, no
  hash-set iteration order). Fixed input + seed → byte-identical output is a
  corpus prerequisite (`RemeshParams::seed`).
- Bonus once it works: Tier-1b's ±1 probes (currently one full RHS back-solve
  each) can run on the local tier too.

Measure (the decision data for Q5): per-lock touched-variable count and its
distribution, GS convergence rate vs the `1e6` seam coupling, escalation rate,
per-round wall-clock vs baseline, tail-round cost.

Gate: existing ctest gates pass (`test_remesh_quantize`,
`test_remesh_extract`); corpus quality metrics unchanged within noise
(`feasible` rate identical, `spiral_isolines == 0` wherever it was 0, folds
within ±few); quantize wall-clock improves on the dense organic assets.

Decision output: **GS converges locally** → Q5 demoted to optional/strategic.
**GS propagates globally or escalates constantly** → that measurement is the
case for Q5.

**Q1 result (landed 2026-06-10).** Implemented as `QuantizeParams::use_local_gs`
(default on): the assembled system is kept (`Acur`/`bcur`) alongside the factor,
new locks fold in value-only (the fix-penalty pattern is a subset of the
always-present seam pattern), sweep GS over sorted work queues, visit cap
`min(256·nseed, 4N)`, and the exact direct solve always re-runs before anything
downstream reads `x` (fold tests, Tier-1b, the reported residual). Gates: ctest
green incl. a new GS-vs-direct parity test; corpus `metrics.csv` **byte-identical
to the Q0 baseline** (zero decision changes) and byte-stable across two runs;
WASM links. Measurement:

- *Convergence:* 16/28 attempts drained within cap (simple-closed), 41/105
  (anime-girl) — escalation 43% / 61%.
- *Locality:* none. `touched_max` ≈ the whole system (7201 of N=7204, 2322 of
  N=2380); mean touched per attempt 16% / 8.7% of N; revisit factor 5.4 / 8.3.
  The `1e6` seam coupling propagates every lock globally, as the risk note
  anticipated.
- *Native wall-clock (same-session A/B):* rounding phase −4% on simple-closed
  (16 ms of GS replaced ~27 ms of direct work), +64% (+23 ms) on anime-girl,
  whose N=2380 makes the replaced per-round direct work nearly free. Quantize
  total stays ARAP-dominated either way — no corpus-scale native win.
- *Where the tier actually pays:* the WASM round structure (each converged round
  removes a full refactorize+solve — 39–57% of rounds) and large-N native (the
  cap is O(batch) while the replaced back-solve is O(nnz(L))). At corpus N the
  caps admit several full-system sweeps, which is where the anime-girl loss
  comes from.

Decision: by the rule above the measurement is nominally *the case for Q5* —
but it equally shows that at corpus scale the per-round direct cost Q5 would
eliminate is marginal (rounding is 7–16% of quantize; ARAP dominates), and the
corpus has zero `feasible=false`. Q5's entry gate is **not** satisfied (condition
1's second clause fails; condition 2 unresolved); it stays gated pending
multi-100k-class assets or the strategic fork. Q2 proceeds on the touched-set.

### Q2 — Incremental confidence re-sort (Lesson 6)

Every round currently recomputes `sideAvg` for all unfixed sides and
re-sorts (O(S log S) per round). With Q1's touched-set, only sides whose
endpoint classes the GS queue actually relaxed can have changed `sideAvg`:

- Maintain the priority structure (sorted container or heap keyed by
  `(frac, side)` — explicit side-index tie-break, today's `std::sort` ties are
  unspecified) and update only touched sides.
- Full-recompute fallback on refactor rounds (where everything moved).

Gate: corpus metrics identical to Q1 (this is bookkeeping, not behavior;
untouched sides' keys are unchanged by construction). Wall-clock neutral-to-
better on assets with large S.

**Q2 result (landed 2026-06-10).** Implemented as a generation-validated lazy
min-heap (`std::push_heap`/`pop_heap` over a `util::Vector<HeapEnt{frac, side,
gen}>`; an entry is current iff `gen == sgen[side]`, stale pops drop silently)
plus a class→sides CSR (`clsOfs`/`clsSides`) so a converged GS round re-keys
only sides incident to `gsSeenList` classes; any direct-solve round (back-solve
rewrites all of x) and round 1 do a full `rebuildHeap()`. Entries popped but not
locked (vertex conflict / past `tau`) are stashed and reinserted with their gen
unchanged. Counters: `resort_full` / `resort_incr` / `resort_keys` (stats +
manifest + `[remesh_quantize:gs]` debug-verb print); `testLocalGsParity` now
asserts the GS run takes the incremental path and the direct control never does.

- **Gates**: native build, both targeted ctests, full suite (the 3 known
  pre-existing failures only), WASM canary all pass. Corpus ×2: metrics.csv
  byte-identical run1 == run2 == `miq-q1-run1` == Q0 baseline — the explicit
  `(frac, side)` tie-break flipped no decisions on the corpus, and round
  structure is unchanged (`gs_rounds`/`gs_converged` identical to Q1).
- **Bookkeeping won**: re-keys (`sideAvg` evals) dropped ~4.6× on anime-girl
  (6 693 vs ~31 k old-scheme estimate; S=592, 105 rounds) and ~3.6× on
  simple-closed (6 994 vs ~25 k; S=1801, 28 rounds), and the per-round
  O(S log S) sort is gone. `resort_incr = gs_converged − 1` on both assets
  (the final round empties `remaining` and skips re-key by construction).
- **Wall-clock**: neutral-to-better — rounding_ms 58.2 → 48.8/57.6
  (anime-girl), 204.9 → 182.6/148.4 (simple-closed) vs `miq-q1-run1`; within
  cross-run noise, no regression. The deterministic counters are the real
  evidence; runs at `tests/remesher-results/miq-q2-run{1,2}/`.

### Q3 — Error-budget batching (Lesson 5, modified)

Replace the fixed `confidence_radius = 0.3` lock criterion with CoMISo's
cumulative budget: take sides most-confident-first while the summed round
residue stays under ~0.5, always taking at least one.

- **Keep the vertex-independence test.** The report claims it can go away —
  true only under exact elimination; under penalties, two spokes of a one-ring
  rounded together can still break loop closure. It is ~15 lines; it stays
  until/unless Q5 lands.
- A/B on the corpus: rounds count, irregular-vertex count, folds, `feasible`
  rate. Keep `confidence_radius` as a param until the budget demonstrably
  dominates, then remove it (no compat shims).

Gate: corpus quality non-regression; rounds (and therefore solves) reduced on
assets where the baseline grinds the tail.

**Q3 result (measured and REJECTED, 2026-06-10).** Implemented exactly per
spec (`round_error_budget = 0.5`, summed-residue cap over the Q2 heap scan,
always ≥ 1, vertex-independence kept; conflicted pops consume no budget) and
A/B'd on the corpus same-session against the Q2 tree; deterministic
(budget run1 == run2, archived `miq-q3-budget-run{1,2}`). Both gate clauses
failed:

- **Rounds/solves up where it matters.** anime-girl — the tail-grinder (105
  rounds, S=592) — went to 113 rounds, back-solves 122 → 147, GS convergence
  41/105 → 22/113, rounding_ms 48.8 → 94.4 (+93 %). simple-closed: rounds
  28 → 24 but back-solves 70 → 74 and rounding_ms 182.6 → 231.8 (+27 %).
  ctest torus: 4 → 9 rounds.
- **Quality regressed on anime-girl**: param folds 47 → 57, inverted 3 → 4, a
  degenerate sliver quad (max_area_ratio 22.6 → 4.2e5, min_angle → 0),
  components 6 → 8. simple-closed moved mixed-better (holes 8 → 6, inverted
  5 → 3, open isolines 120 → 97) with worse shape extremes.
- **Why, structurally**: a 0.5 cumulative budget is strictly ⊆ the tau = 0.3
  batch whenever fracs ≤ 0.3 (no two sides > 0.25 can share a round), so it
  only shrinks confident batches; the single ambiguous (frac > tau) side it
  pulls forward per round locks low-confidence decisions early — which is
  exactly what collapsed GS convergence and produced the sliver. CoMISo's
  multiple-rounding threshold wins against their *one-variable-per-round*
  baseline, not against a confidence-radius batch.

Decision (the mirror of the keep-until-dominates clause): the budget code is
removed, `confidence_radius = 0.3` stays the criterion, no dead param left
behind. Final tree re-gated: quantize/extract ctests reproduce Q2 outputs,
full suite (3 known pre-existing failures only), WASM canary, and a control
corpus run byte-identical to `miq-q2-run1` (and Q0). Revisit only if a future
asset class shows tail rounds with many *sub-tau* sides left unbatched — the
one regime where a budget could admit more than tau does.

### Q4 — Rounding strategy enum (Lessons 7/8)

`QuantizeParams::rounding ∈ {GREEDY (default), DIRECT}`:

- DIRECT: after the initial seamless solve, round every side at once, one
  re-solve, done. "Fast but far from optimal" — a fast path for clean inputs
  (grid / cylinder / few singularities) and a **test oracle**: greedy must
  never be worse than direct on the corpus (folds, residual, irregular
  verts).
- The exact-fallback slot (CoMISo's Gurobi/CPLEX tier) stays empty; Tier-1b's
  ±1 search is already the embryonic local version. Document the slot, don't
  build it.

Gate: corpus runner can select per-asset; a new ctest case asserts
greedy ≥ direct on the standard fixtures; DIRECT on grid/cylinder/torus is
feasible with `max_loop_closure < 1e-6`.

**Q4 result (landed 2026-06-10).** `RoundingStrategy {GREEDY, DIRECT}` on
`QuantizeParams`; DIRECT is realized as *zero greedy rounds* (`max_rounds = 0`,
no heap build) — the existing leftover block already locks every unfixed side
off the seamless/ARAP-settled solve and re-solves once, so the strategy is a
two-line guard, not a second code path. `stats.iters == 0` self-describes a
DIRECT run. Plumbed end-to-end: `RemeshParams::quantize_direct_rounding`
(bound, manifest-recorded), `remesh_cli --quant-direct`, corpus `--quant-direct`
global (per-asset `params` override last-wins), debug verb
`remesh_quantize direct=1`. The exact-fallback tier (CoMISo's Gurobi/CPLEX
slot) is documented at the enum and deliberately unbuilt.

- **Gates, all pass**: `testDirectRounding` — DIRECT on grid/cylinder/torus
  feasible, `max_loop_closure = 0 < 1e-6`, `iters = 0`, and greedy folds ≤
  direct folds on every fixture (0 ≤ 0). Existing quantize/extract cases
  byte-identical; full suite (3 known pre-existing failures only); WASM
  canary (TS bindings regenerate with the new member). Default-path corpus
  run `miq-q4-run1` **byte-identical** to `miq-q2-run1` (→ Q1 → Q0): the
  enum is invisible until selected.
- **DIRECT corpus A/B** (`miq-q4-direct`, same-session): greedy dominates the
  messy asset on every axis — anime-girl: feasible true vs **false** (residual
  2.7e-6 vs 0.43), folds 47 vs 70, inverted 3 vs 8, irregular interior 42
  vs 56 (regular 72.4 % vs 62.7 %), min_jacobian −494 vs −4533, max area
  ratio 22.6 vs 57.3. simple-closed is a statistical tie of two different
  quantizations: greedy a hair worse on folds (54 vs 53) and irregular (66
  vs 63), better on inverted (5 vs 10), components (1 vs 2), and feasibility
  (true, 2.4e-5 vs **false**, 0.37). Treat sub-5 % mixed deltas on a clean
  asset as a tie; the regression the oracle exists to catch is greedy losing
  *decisively* anywhere.
- **Role confirmed**: DIRECT's single 1e6-penalty re-solve cannot reach
  integrality on either corpus asset (both `feasible=false`) — it is the
  clean-input fast path and test oracle, never the quality default. Rounding
  wall-clock 10×/7.8× faster (6.0 vs 60.6 ms; 28.1 vs 220.6 ms) but total
  quantize moves only 2–18 % — ARAP dominates. Greedy machinery verifiably
  skipped: rounds/updowns/gs_*/resort_* all 0 under DIRECT.

### Q5 — Exact constraint route (Lessons 2 + 3 + 1) — **gated**

Entry gate (all three):
1. Q1 measurement shows local GS insufficient (constant escalation / global
   propagation), so the eliminated formulation's conditioning is needed; **or**
   corpus shows `feasible=false` / residual failures actually occurring at a
   rate that matters.
2. The strategic fork is resolved in favor of staying MIQ: the T-mesh / QGP
   reframing (`research/layout-embedding-optimization.md`, `quantize/t_mesh.*`)
   would replace this rounding formulation entirely — don't build the exact
   route if that direction is chosen.
3. Accepted cost: the ARAP continuation keeps a penalty phase (dual
   formulation, one extra analyze+factor per quantize call) — see the
   correction in Context.

Design honesty note (where the report oversimplifies): in our formulation the
translation is *derived* (`t = B·x_b − A·x_a`), not a variable. An "exact
integer lock" is therefore the exact elimination of one variable per locked
constraint row — and since every side eventually locks, Lesson 2 done exactly
**is** Lesson 1 applied incrementally. Plan for that, don't pretend it's
contained.

Sub-steps:

- **Q5.a — Elimination machinery (Lesson 3).** Constraint-row Gaussian
  elimination with CoMISo's discipline: pivot real variables first, then
  ±1-coefficient integer pivots; divide rows by the gcd of integer
  coefficients (`ConstraintTools` / `update_constraint_gcd` are the
  reference). For our 90°-rotation blocks coefficients start in {0, ±1} and
  stay there along simple seam curves; the gcd path genuinely activates only
  at singularity junctions — unit-test exactly those (junction rows with ±2
  combinations).
- **Q5.b — Exact lock.** Replace the `lam_fix` penalty with elimination of the
  locked rows. `feasible` becomes structural (`residual ≡ 0` by
  construction); the over-constrained fallback is redefined — infeasibility
  now surfaces as a rank/conflict during elimination and must still be a
  clean, non-crashing fallback (`testOverConstrainedFallback` rewritten to
  assert that).
- **Q5.c — Factor maintenance.** Elimination shrinks/re-patterns the system
  per round, which breaks the pattern-invariant updown scheme. Options:
  CHOLMOD `rowdel`-based maintenance, refactor cadence, or — preferred — rely
  on Q1's local GS (CoMISo's own answer; on the eliminated system it converges
  properly). Decide by measurement.
- **Q5.d — ARAP reconciliation.** Continuation phase runs on the penalty
  formulation as today; switch to the eliminated formulation at rounding
  entry.
- **Q5.e — Drop vertex-independence batching** (Lesson 5 full form) — valid
  only now, since rounded values hold exactly.

Gate: `test_remesh_quantize` residual asserts tightened from `< 1e-3` to
machine-zero; corpus quality non-regression (explicitly **no** fold-count
expectation); wall-clock not worse than the Q1-Q3 system; WASM build still
links and passes (no CHOLMOD there — Q5.c's answer must be GS or Eigen-only).

### Q6 — Cleanup

Strip `CLAUDENOTE:`s, remove superseded params (`confidence_radius` if Q3
removed it), update `documentation/quad-remeshing.md` (quantize stage
description + stats), and annotate the research report with a pointer to this
plan and the Lesson-1 correction.

**Q6 result (done 2026-06-10).** No `CLAUDENOTE:`s existed in the tree;
`confidence_radius` **stays** (the removal clause was conditional on Q3
winning — Q3 was rejected and tau = 0.3 remains the criterion, with no
`round_error_budget` residue). `documentation/quad-remeshing.md` gained a
*Quantization rounding (M5)* subsection (penalty formulation, greedy + lazy
heap, local-GS tier, GREEDY/DIRECT strategy, stats counters), the
`quantize_direct_rounding` param row, and an accurate `run.quantize` manifest
description; `research/miq-rounding-report.md` now opens with the annotation
block (plan pointer + the Lesson-1 correction + the Q1/Q3 measured verdicts).
Docs-only — the Q4 gate results (full suite, byte-identical corpus) stand as
the current-tree evidence.

## Measurement protocol

- **Corpus:** `node tools/remesh_corpus.mjs` (full) / `--only <asset>`;
  diff `tests/remesher-results/corpus/metrics.csv` against the Q0 baseline —
  quality columns must not regress; timing read from `results.json`.
- **ctest:** `node make.mjs test test_remesh_quantize` /
  `test_remesh_extract` per milestone; full `node make.mjs test` before
  landing each.
- **Determinism:** two corpus runs per milestone must produce byte-identical
  `metrics.csv` (fixed seed).
- **WASM:** `node make.mjs build` (wasm) per milestone — Q1's CSR tier is the
  only planned WASM-visible change before Q5, but the build is the cheap
  canary.
- **Interactive spot-check:** `remesh_debug_app` + `tools/remesh_dbg.mjs` on
  the dense organic assets (the regime where the tail-round cost and the ARAP
  fallback both fire).

## Risks / open questions

- **GS convergence under 1e6 springs** is the whole Q1 question. If the seam
  coupling makes every local perturbation propagate along the full cut
  network, the tier buys little — that result is itself the Q5 entry
  evidence, not a failure of the plan.
- **Determinism of iterative solves.** GS results differ from the direct
  solve in floating point; run-to-run identity requires strict deterministic
  ordering. Any unordered container in the hot loop is a corpus-breaking bug.
- **Tier-1b interplay.** Tier-1b assumes the factor holds every lock and does
  RHS-only re-solves; with the GS tier active, probe/settle solves must leave
  the same state the direct path would (settle with the same escalation
  rules).
- **Updown machinery is load-bearing.** The pattern-invariant rank-update
  scheme (`updown_max_cols`, refactor heuristic) is tuned and correct; Q1-Q3
  must not regress it — GS is a tier *in front of* it, not a replacement.
- **The strategic fork.** If the T-mesh / QGP direction is taken, Q5 is dead
  weight; Q1-Q4 survive regardless (any formulation needs fast local
  re-solves, batching, and a strategy switch). That asymmetry is why the plan
  front-loads them.
