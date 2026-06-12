#pragma once

/* Integer quantization (M5) — the spiral-elimination step.
 *
 * M4 gives a seamless (u, v) whose cut-edge transitions are 90-degree rotations
 * plus *real* translations. Spirals appear precisely when those translations are
 * non-integer: an iso-line drifts each time it crosses a cut and never closes. We
 * snap every cut translation to an integer, producing an integer-grid map (IGM)
 * in which every iso-line closes.
 *
 * Method: rebuild the M4 coupled system in the 2M class space x = (U_0, V_0, U_1,
 * V_1, ...). The base cotangent Dirichlet term keeps the map close to M4; for
 * each cut edge a penalty lambda * || B x_b - A x_a - k_e ||^2 pulls its realized
 * translation toward the current integer target k_e (A, B are the gauge/period
 * 90-degree rotations). We solve, lock a vertex-independent batch of confident
 * sides onto integers, and re-solve until every translation is integral. The
 * factor is maintained incrementally across rounds (each lock is a low-rank
 * update, not a fresh factorization). When integrality is reached the IGM is
 * valid (no spirals); if it cannot be reached
 * (over-constrained), we keep the best real-valued result and report feasible =
 * false — a usable, non-crashing fallback. Writes .remesh.c.uv (updated float2)
 * and .remesh.e.translation_q (int2), both TEMP. */

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::remesh {

/* Rounding strategy (plans/miq.md Q4). GREEDY is Bommes most-confident-first
 * batching with re-solves — the quality default. DIRECT rounds every side off
 * the initial seamless solve in one shot ("fast but far from optimal"): a fast
 * path for clean inputs and the oracle GREEDY must never lose to. A third,
 * exact tier (CoMISo's Gurobi/CPLEX slot) is deliberately unbuilt — Tier-1b's
 * ±1 search is its embryonic local form; add it here if it ever pays. */
enum class RoundingStrategy { GREEDY = 0, DIRECT = 1 };

struct QuantizeParams {
  float target_edge_length = 0.1f; // world-space target quad spacing
  bool use_density = false;        // scale spacing by 1/sqrt(.remesh.v.density)
  float gauge_eps = 1e-9f;         // Tikhonov shift for solver conditioning
  double integer_tol = 1e-4;       // max ||t - round(t)|| accepted as integral
  double max_lambda = 1e8;         // penalty ceiling; small values force fallback
  // Greedy-rounding confidence radius: lock a side only once its realized
  // translation is within this of an integer. Larger = fewer rounds/solves (the
  // dominant cost); past ~0.3 it adds a few irregular vertices. 0.3 is the
  // quality-neutral knee (bit-identical to the old per-side 0.1). Measured
  // better than CoMISo's cumulative error budget here (plans/miq.md Q3).
  double confidence_radius = 0.3;
  // Local-injectivity stiffening rounds, run after the integers lock. The linear
  // MIQ map can fold where curvature concentrates (cap/crease cones); each round
  // ramps a Dirichlet+RHS weight on folded faces + their 1-ring (capped below the
  // locked-grid penalties) to pull them flat, keeping the lowest-fold result.
  // Stops early once folds plateau, so a generous count is cheap. 0 disables.
  int inj_iters = 25;
  // Tier-1b seam-integer relaxation rounds, run right after rounding (before the
  // injectivity stiffening). A fold wedged at a cut often clears if one incident
  // cut-edge translation is nudged by +/-1; we try each fold-adjacent side's four
  // unit moves with a cheap RHS-only re-solve and keep a move only when it strictly
  // drops the fold count *and* the integer residual stays feasible (so loop closure
  // / no-spiral is preserved). 0 disables. Bounded work: fold-adjacent sides only.
  int seam_relax_iters = 4;
  // Engage Tier-1b only once residual folds are numerous (the dense-organic
  // regime). A handful of isolated folds are cosmetic singularity-cone artifacts
  // that already extract cleanly; perturbing their integers to chase the proxy
  // fold count can shuffle the grid into a *different* valid map that extracts
  // slightly worse. Many clustered folds are the genuine tangle Tier-1b clears.
  int seam_relax_min_folds = 100;
  // Tier-3 local fold-patch re-parametrization, run after the global injectivity
  // pass. Folds wedged between locked seams survive the global stiffening; this
  // instead *moves* each fold patch's interior (non-seam, non-boundary) classes to
  // remove the inversion, minimizing the convex fold-removal energy
  // sum_t max(0, delta - signed_area_t)^2 (area linear in each class -> convex, so
  // it reaches the global optimum for the pinned patch boundary). Accept only when
  // total folds drop. local_untangle_iters = gradient-descent steps (0 disables);
  // local_untangle_grow = neighbor rings added around each folded face (boundary
  // pinned), giving the interior slack to flatten.
  int local_untangle_iters = 80;
  int local_untangle_grow = 2;
  // ARAP untangle fallback. The exactly-seamless map folds wherever the cross
  // field curls (the seam-consistency penalty fights the field there). On clean
  // inputs that is a few percent; on rounded organic blobs it can reach a third
  // of the faces and break extraction. When the post-seamless fold fraction
  // exceeds this, walk the seam penalty up from a low (injective) weight,
  // retargeting every face to the nearest rotation of its realized Jacobian each
  // step, so the map starts injective and stays injective as the seams tighten.
  // 0 disables (field-aligned only). 0.10 matches the extraction fold gate.
  double untangle_fold_threshold = 0.10;
  // Max angular deviation (radians) the ARAP untangle retarget may keep from
  // the nearest field-aligned rotation. Scheduled from free (45 deg) at the low
  // seam weight down to this by the final step, so the map untangles first and
  // realigns as the seams tighten. >= pi/4 = legacy unclamped retarget.
  double untangle_field_max_dev = 0.17453293; // 10 degrees
  // Local Gauss-Seidel re-solve tier (plans/miq.md Q1). After each rounding
  // batch, try a work-queue GS relaxation seeded at the locked sides' classes
  // before paying for the direct path; escalate on a visit cap. Backend-agnostic
  // (the bigger win is WASM, which otherwise refactorizes every round).
  bool use_local_gs = true;
  // See RoundingStrategy. DIRECT skips the greedy round loop entirely: all
  // sides lock at once off the seamless/ARAP-settled solve, one re-solve.
  RoundingStrategy rounding = RoundingStrategy::GREEDY;
  // Native rounding: a rank-k cholmod_updown costs ~O(k * etree-path); above
  // this many pending update columns (4 per locked side) a full numeric
  // re-factorization is cheaper than the incremental update.
  int updown_max_cols = 256;
  // Native: let CHOLMOD_AUTO pick supernodal (BLAS-3) factorization, keeping a
  // lazy simplicial clone for cholmod_updown. Off by default: even with the
  // threaded OpenBLAS (+ the blas_threads cap) it measured ~4x slower on
  // corpus-scale systems (refactor-heavy ARAP/stiffening, fronts too small to
  // amortize BLAS-3); it wins ~2.3x end-to-end only from ~18k classes up
  // (synthetic sphere sweep). Flip per-run for large systems.
  bool use_supernodal = false;
};

struct QuantizeStats {
  int num_faces = 0;
  int num_corners = 0;
  int num_classes = 0;
  int num_cut_edges = 0;
  double max_integer_residual = 0.0; // max ||t_e - round(t_e)|| over cut edges
  double max_loop_closure = 0.0;     // max one-ring closure residual (no-spiral)
  double min_jacobian = 0.0;         // min per-face det(grad u, grad v)
  int parametrization_folds = 0;     // faces with det(grad u, grad v) <= 0 (pre-extract)
  // Field alignment of the final map: |angle(grad u) - theta| mod 90 deg,
  // area-weighted over faces with a usable gradient. frac = area fraction
  // beyond 22.5 deg (closer to the diagonal than to the field).
  double field_dev_mean_deg = 0.0;
  double field_dev_max_deg = 0.0;
  double field_dev_frac = 0.0;
  int iters = 0;         // greedy rounding rounds run
  bool solved = false;   // linear solves succeeded
  bool feasible = false; // integrality reached within integer_tol (no spirals)

  // Tier-5 gate diagnostic, measured on the raw seamless solve: pairs =
  // opposite-index poles within 2 vertex hops; near_pairs = folded faces
  // touching a vert within 3 hops of a paired pole (fold/pair co-location).
  int num_singularities = 0;
  int spurious_pairs = 0;
  int seamless_folds = 0;
  int seamless_folds_near_pairs = 0;

  // Rounding-loop profile (plans/miq.md Q0). Counts are deterministic; the _ms
  // wall-clocks are volatile and must stay out of corpus metrics.csv (they are
  // surfaced via the manifest "run" block / results.json only).
  int full_refactors = 0; // full numeric factorizations (analyze excluded)
  int updowns = 0;        // incremental rank-update applications (native only)
  int simp_refreshes = 0; // supernodal->simplicial factor clones (native only)
  int back_solves = 0;    // RHS solves against the factor (a probe batch = 1)
  int tier1b_probes = 0;  // Tier-1b +/-1 trial solves (settles excluded)
  // Local-GS tier (Q1): the Q5 decision data. touched = distinct components a
  // single attempt visited (how far the lock's influence spread); visits/touched
  // is the mean revisit factor (GS convergence rate vs the 1e6 seam coupling).
  int gs_rounds = 0;        // lock batches attempted on the local tier
  int gs_converged = 0;     // attempts that drained within the visit cap
  int gs_visits = 0;        // component relaxations summed over attempts
  int gs_touched_total = 0; // distinct touched components summed over attempts
  int gs_touched_max = 0;   // largest single-attempt touched set
  // Confidence re-sort (Q2): rounds that re-keyed everything (direct solves)
  // vs incrementally from the GS touched-set, and total side re-keys.
  int resort_full = 0;
  int resort_incr = 0;
  int resort_keys = 0;
  double total_ms = 0.0;    // whole computeQuantization call
  double setup_ms = 0.0;  // seamless system + quant graph build
  double initial_factor_ms = 0.0; // first seamless solve (analyze+factor+solve)
  double arap_ms = 0.0;           // ARAP untangle continuation
  double rounding_ms = 0.0;       // greedy rounding loop incl. leftover locks
  double round_assemble_ms = 0.0; //   matrix assembly within rounding
  double round_refactor_ms = 0.0; //   full refactors within rounding
  double round_updown_ms = 0.0;   //   rank updates within rounding
  double round_backsolve_ms = 0.0; //  RHS solves within rounding
  double convert_ms = 0.0; // supernodal->simplicial refreshes (whole call)
  double gs_ms = 0.0;              //   local-GS attempts within rounding
  double tier1b_ms = 0.0;          // seam-integer relaxation
  double stiffen_ms = 0.0;         // injectivity stiffening rounds
  double tier3_ms = 0.0;           // local fold-patch re-parametrization
};

/* Quantize the seamless parametrization to an integer-grid map. Builds the
 * cross field / cut graph / seamless system internally (does not require M4 to
 * have run). Thaws topology + recomputes normals. */
QuantizeStats computeQuantization(mesh::Mesh &m, const QuantizeParams &params);

} // namespace sculptcore::remesh
