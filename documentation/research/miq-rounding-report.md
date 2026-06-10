I've now read both our quantizer and the full CoMISo mixed-integer stack (the greedy MISolver, the exact
  ConstrainedSolver elimination, the integer-preserving Gauss elimination in ConstraintTools, and the local Gauss-Seidel
  updater). Here's what I found.

  What's actually comparable

  Our computeQuantization (quantize/quantize_ilp.cc) and CoMISo's MISolver::solve_multiple_rounding (MISolver.cc:476)
  are solving the same problem — Bommes-style MIQ integer rounding of a seamless map — but with one deep architectural
  difference that cascades into everything else:

  CoMISo eliminates its constraints exactly before rounding; we enforce them with penalties.

  That one choice is, I think, the headline lesson. Let me lay out the specifics.

  ---
  Lesson 1 — Exact constraint elimination vs. our 1e6 penalties (the big one)

  Our seamless-transition and integer-lock constraints are soft penalties:

  - lam_seam = 1.0e6 ties the two endpoints of every cut to one shared translation (quantize_ilp.cc:128, applied at
  :206).
  - lam_fix = 1e6 pins a locked side onto its chosen integer (:207-214).

  CoMISo never pays the penalty tradeoff. ConstrainedSolver::make_constraints_independent →
  ConstraintTools::gauss_elimination (ConstrainedSolver.cc:346) does Gaussian elimination on the constraint rows, then
  eliminate_constraints (:443) substitutes the dependent variables out of A and rhs and shrinks the system. The energy
  is then minimized unconstrained over the genuinely-free variables, so the result is the true constrained minimizer —
  no penalty distortion, exact to machine precision.

  This matters enormously because your own folding problem is largely self-inflicted by the penalty. Your code comment
  at quantize_ilp.cc:651 says it outright: "folds scale directly with lam_seam: ~0% at 0.1, ~34% at 1e6." The three
  untangle passes you maintain afterward —

  - Tier-1b seam-integer relaxation (:815-880),
  - global injectivity stiffening (:882-955),
  - Tier-3 local fold-patch reparametrization (:957-1221),

  — are, to a large degree, machinery for fighting the distortion your own lam_seam=1e6 introduces. CoMISo doesn't need
  any of them at the seam level because the seam constraint is structurally exact, not a 1e6 spring fighting the field.
  If you eliminated the transition constraints exactly, my expectation is most folds disappear at the source and a big
  chunk of that ~400 lines of untangle code becomes unnecessary.

  The good news: your class space is already half-way there. Corner-classes already identify corners across non-cut
  edges — that's exactly the kind of variable elimination CoMISo does. You're only keeping the cut transitions as
  penalties. So the surgical version of this lesson is: identify the per-cut transition equations B x_b − A x_a = k,
  eliminate them exactly (one variable per equation), and let greedy rounding run on the reduced free system.

  Lesson 2 — Exact integer lock makes "no spirals" a theorem, not a tolerance

  Today you soft-lock integers and then report whether it worked: feasible = all_solved && (residual < integer_tol) with
  max_integer_residual measured after the fact (:1237-1239). The "provably free of spiraling iso-lines" guarantee in
  the docs is really "no spirals if the residual happened to land under 1e-4."

  CoMISo's fix_var_csc_symmetric (MISolver.cc:552) eliminates each rounded variable exactly — substitutes the value,
  folds it into the rhs, deletes the row/column. Residual is identically zero by construction; the integer cocycle holds
  exactly. Even if you keep the penalty seam formulation, switching the integer lock from lam_fix to exact elimination
  removes the feasible=false failure mode entirely and turns your spiral-freedom claim into something structural.

  Lesson 3 — If you eliminate, you must preserve integrality (GCD trick)

  This is the subtle one that's easy to get wrong, and CoMISo has a careful answer. When you eliminate an integer
  variable in terms of others, fractional coefficients would mean "round to integer" no longer lands on the lattice.
  ConstraintTools.cc:276-377 picks pivots in a specific order — real-valued variables first, then integer variables with
  coefficient closest to ±1 — and update_constraint_gcd/find_gcd (:507-589) divides each constraint row by the gcd of
  its integer coefficients so integer combinations stay integer. If you go the elimination route, port this logic
  faithfully; it's the difference between a real lattice and a lattice that's integer "to 1e-4."

  Lesson 4 — Local Gauss-Seidel instead of a global back-solve every round (pure perf)

  This one is independent of penalty-vs-exact and is probably your highest-value cheap win at the 5M-tri target.

  Every rounding round you do a full CHOLMOD cholmod_solve back-solve (solveCurrent, :363). You were clever about the
  factor (incremental cholmod_updown, :329), but you still pay an O(nnz(L)) back-solve per round even though locking one
  side perturbs only a local neighborhood.

  CoMISo's update_solution_is_local (MISolver.cc:412) escalates in three tiers:
  1. Local Gauss-Seidel (gauss_seidel_local, IterativeSolverT_impl.hh:26) — a work-queue that starts at the rounded
  variable's neighbors, relaxes each, and pushes a neighbor back onto the queue only when its residual actually moved.
  The perturbation propagates outward exactly as far as it matters and stops (queue empties) when absorbed — usually a
  few dozen variables, not the whole mesh.
  2. Conjugate gradient if GS didn't converge.
  3. Full direct re-factor only as a last resort.

  Most rounds never touch the direct solver. For you the analog is: try local GS first, fall back to your existing
  updown-CHOLMOD path only when it doesn't converge. Caveat worth A/B-testing: your 2M block system with lam_seam
  springs is more strongly coupled than CoMISo's eliminated system, so GS may propagate further — but
  bench_dyntopo-style measurement on the corpus will tell you quickly, and even a partial hit rate cuts the dominant
  cost.

  Lesson 5 — Error-budget batching is more principled than a fixed confidence radius

  You batch by a fixed confidence_radius = tau = 0.3 plus vertex-independence (:707-750). CoMISo batches by cumulative
  rounding error: keep adding most-confident-first until the summed round-residue exceeds multiple_rounding_threshold_ =
  0.5, always rounding at least one (MISolver.cc:533-553). That adapts to batch confidence instead of a hand-tuned
  radius.

  More interesting: your vertex-independence requirement is itself a symptom of the penalty formulation. You need it
  because rounding two spokes of a one-ring together under a soft re-solve can break loop closure (your comment at
  :639). With exact elimination the rounded variable's effect is folded into the system, the cocycle is preserved
  automatically, and the independence constraint — plus all the usedV bookkeeping — can go away.

  Lesson 6 — Incremental priority re-sort

  You recompute sideAvg for all unfixed sides and full-sort every round (:710-722) — O(S² log S) over a run. CoMISo
  keeps a std::set sorted by residue and refreshes only the variables the local update actually touched
  (MISolver.cc:556-564, via updated_variable_indices()). If you adopt local GS (Lesson 4), the touched set is the GS
  work-queue's output, so the incremental re-sort falls out for free.

  Lesson 7 & 8 — Strategy enum: a direct fast-path and an exact fallback

  CoMISo exposes rounding as a strategy (RoundingType::{NONE,DIRECT,MULTIPLE,GUROBI,CPLEX}, MISolver.hh:71):
  - DIRECT (MISolver.cc:306) rounds all integers in one pass + one final solve — "fast but far from optimal." Worth
  having as a fast path for clean inputs (grids, cylinders, few singularities) where you currently still grind the full
  MIS loop, and as a correctness oracle in tests (greedy should never beat... I mean never be worse than direct).
  - Gurobi/CPLEX (:597, :186) are a true branch-and-bound fallback for hard cases. You won't ship a commercial solver,
  but the architecture lesson stands: keep the rounding behind a strategy enum so the corpus runner can pick per-asset,
  and a slower-but-exact path (even a small exhaustive ±1 search — which your seam_relax already is, in embryo) can be
  swapped in for the tangled organic blobs.

  ---
  What I'd actually try first

  If you want one experiment with the best payoff-to-risk: eliminate the integer lock exactly (Lesson 2) — replace the
  lam_fix penalty path with CoMISo-style fix_var_csc_symmetric substitution. It's contained (doesn't require rebuilding
  the whole constraint system), it kills the feasible=false mode, and it makes the spiral-freedom guarantee real. Then
  measure how much of the fold problem remains before deciding whether the larger seam-elimination rewrite (Lessons
  1/3/5) is worth it.

  The local Gauss-Seidel update (Lesson 4) is the orthogonal perf win and can land independently — it's the thing
  that'll matter most when you push toward millions of triangles.

  One caveat on scope: CoMISo is the MIQ-era (Bommes 2009/2013) approach, which is exactly what your pipeline is. The
  newer line — Campen et al. Quantized Global Parametrization / the layout-embedding work in your untracked
  documentation/research/layout-embedding-optimization.md — reframes quantization as integer flow on a T-mesh rather
  than rounding a seamless map, and sidesteps the fold-fighting differently. Worth knowing these lessons are "make your
  current MIQ rounding exact," not necessarily "this is the endgame architecture."

  Want me to draft the exact-integer-lock change against quantize_ilp.cc, or prototype the local-GS update path so you
  can bench it on the corpus?