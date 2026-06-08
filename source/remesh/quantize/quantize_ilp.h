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

struct QuantizeParams {
  float target_edge_length = 0.1f; // world-space target quad spacing
  bool use_density = false;        // scale spacing by 1/sqrt(.remesh.v.density)
  float gauge_eps = 1e-9f;         // Tikhonov shift for solver conditioning
  double integer_tol = 1e-4;       // max ||t - round(t)|| accepted as integral
  double max_lambda = 1e8;         // penalty ceiling; small values force fallback
  int max_iters = 28;              // lambda-ramp / re-round rounds
  // Greedy-rounding confidence radius: lock a side only once its realized
  // translation is within this of an integer. Larger = fewer rounds/solves (the
  // dominant cost); past ~0.3 it adds a few irregular vertices. 0.3 is the
  // quality-neutral knee (bit-identical to the old per-side 0.1).
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
};

struct QuantizeStats {
  int num_faces = 0;
  int num_corners = 0;
  int num_classes = 0;
  int num_cut_edges = 0;
  double max_integer_residual = 0.0; // max ||t_e - round(t_e)|| over cut edges
  double max_loop_closure = 0.0;     // max one-ring closure residual (no-spiral)
  double min_jacobian = 0.0;         // min per-face det(grad u, grad v)
  int iters = 0;
  bool solved = false;   // linear solves succeeded
  bool feasible = false; // integrality reached within integer_tol (no spirals)
};

/* Quantize the seamless parametrization to an integer-grid map. Builds the
 * cross field / cut graph / seamless system internally (does not require M4 to
 * have run). Thaws topology + recomputes normals. */
QuantizeStats computeQuantization(mesh::Mesh &m, const QuantizeParams &params);

} // namespace sculptcore::remesh
