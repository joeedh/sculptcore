#pragma once

/* Quad-remesh parameters — the user-facing knobs for QuadRemesh().
 *
 * Kept binding-header-free (forward-declared Struct, out-of-line
 * defineBindings() in remesh/bindings.cc) so the algorithmic headers that
 * include this stay light, mirroring dyntopo/dyntopo.h's DynTopoParams. The
 * struct crosses the WASM/N-API seam by value, so bindings.cc registers a copy
 * constructor for it. */

#include <cstdint>

// Forward-declared so the param struct can carry a defineBindings() hook
// without pulling the binding system into this header (model: dyntopo.h).
namespace litestl::binding::types {
template <typename CLS> struct Struct;
}

namespace sculptcore::remesh {

struct RemeshParams {
  /* Target output quad count. The pipeline derives the quad edge length from
   * it: L = sqrt(integral of density dA / N). Ignored when target_edge_length
   * is set explicitly. */
  int target_quad_count = 15000;

  /* Explicit quad edge length (world units), overriding target_quad_count.
   * 0 = derive from target_quad_count (count mode, with corrective
   * re-quantize); > 0 = legacy fixed-length mode, no count correction. */
  float target_edge_length = 0.0f;

  /* Optional solve-mesh edge length (world units). 0 = off (solve on the raw
   * input). When > 0, a dyntopo uniform-remesh pre-pass coarsens the working
   * copy to roughly this edge length before the heavy global solve, so dense
   * inputs (100s of k of triangles) stay tractable. The final reprojection
   * always targets the full-res original, so detail is preserved. Pick it a
   * touch finer than target_edge_length (a few solve-triangles per output quad). */
  float solve_edge_length = 0.0f;

  /* M1/M2: align the cross field to principal-curvature directions, weighted by
   * local anisotropy. Off = a pure-smoothness field (strokes/features only). */
  bool use_curvature = true;

  /* M1/M2: pin the cross field to sharp edges + open boundaries (axis parallel
   * to the feature tangent), so creases and borders stay on quad edge loops. */
  bool use_sharp_features = true;

  /* Dihedral threshold (radians) above which an edge is tagged sharp (M1).
   * Default ~45deg. */
  float sharp_angle = 0.7853982f;

  /* Tier 7a: hysteresis band (radians, clamped to [0, sharp_angle]) below
   * sharp_angle; a weaker edge is tagged sharp only when vertex-connected to
   * a strong one, so a crease oscillating around the threshold stays whole. */
  float feature_hysteresis = 0.0f;

  /* M4: scale the parametrization metric by 1/density so quad spacing follows
   * the per-vertex `.remesh.v.density` map. Off = uniform spacing. */
  bool use_density = false;

  /* M5: round every cut translation at once off the seamless solve (one
   * re-solve) instead of greedy most-confident-first batches. Fast path for
   * clean inputs and the quality oracle greedy must beat (plans/miq.md Q4). */
  bool quantize_direct_rounding = false;

  /* M6: snap each output vertex back onto the input surface via the BVH
   * closest-point query. Off = leave extracted positions as-is (debugging). */
  bool reproject = true;

  /* M6: also close odd-length cap rims. Odd rims pair up per component; each
   * pair is made even by a lengthwise quad-strip ladder split, keeping the cap
   * all-quad. Unpairable rims fall back to a fan with one cap triangle.
   * Off = leave odd holes open (strict all-quad). */
  bool cap_odd_holes = false;

  /* M6: Laplacian-smoothing passes interleaved with reprojection to relax kinks
   * without inverting quads. */
  int smooth_iterations = 2;
  /* M6: per-iteration smoothing step (0..1). */
  float smooth_strength = 0.5f;

  /* Determinism seed for the M3 iteration / M5 greedy-rounding tie-breaks, so a
   * fixed input+seed yields a byte-identical output (the cross-backend parity
   * prerequisite). */
  uint32_t seed = 1u;

  /* Tier 1: run input triage on the working copy before any field math — weld
   * near-coincident verts, drop degenerate faces / tiny components, detect
   * non-manifold. Defaults ON (review gate 1): a no-op on clean input
   * (byte-identical), and unblocks messy/Meshy/scanned input. */
  bool triage = true;
  /* Tier 1: weld tolerance as a fraction of the mesh bbox diagonal. */
  float triage_weld_rel = 1e-5f;
  /* Tier 1: drop disconnected components below this fraction of total verts
   * (0 = keep all). */
  float triage_min_component_frac = 0.0f;

  /* Tier 6: pre-solve input hole policy — triangulate-fill input boundary
   * loops whose rim length is under this fraction of the total boundary
   * length, so tiny punctures don't seed spurious boundary constraints; larger
   * boundaries stay open. 0 = fill nothing. */
  float input_hole_fill_max_frac = 0.0f;

  /* Tier 6: remesh disconnected components independently so one component's
   * field/singularities can't perturb another's solve. All components share
   * the globally resolved quad edge length; a component whose sub-run fails
   * is dropped from the merged output (counted in the run report). */
  bool per_component = false;

  /* Tier 2a: Jacobi-diffuse the per-vertex shape operator over the one-ring
   * before eigendecomposition, denoising the principal-curvature field without
   * touching geometry. 0 = today's raw 1-ring estimate (no smoothing). */
  int curvature_smooth_iters = 0;
  /* Tier 2a: per-sweep blend in [0,1] for the curvature tensor diffusion. */
  float curvature_smooth_lambda = 0.5f;

  /* Tier 4: per-edge smoothness weight of the cross-field solve. Higher =
   * globally smoother field with fewer noise-born singularities, at the cost
   * of curvature tracking (regularization ~ field_smoothness/curvature_weight). */
  float field_smoothness = 1.0f;
  /* Tier 4: soft curvature-alignment scale (x local anisotropy) — the other
   * half of the smoothness/alignment tradeoff. */
  float curvature_weight = 1.0f;

  /* Tier 5: after the M3 re-solve, annihilate opposite-index singularity pairs
   * closer than singularity_cancel_max_sep quad-edge-lengths (geodesic) — noise
   * pairs the output lattice can't represent anyway. Index-sum-checked. */
  bool singularity_cancel = true;
  float singularity_cancel_max_sep = 1.5f;

  /* Tier 3a: generate the per-vertex .remesh.v.density sizing field from the
   * (Tier-2-smoothed) principal curvature — small quads at high curvature, large
   * on flat regions. Implies density consumption (the pipeline ORs this into
   * QuantizeParams.use_density). Off = no auto field (today). */
  bool auto_density = false;
  /* Tier 3a: clamp on the generated density (size range). density_max also acts
   * as the minimum-feature-size floor. */
  float density_min = 0.25f;
  float density_max = 4.0f;

  /* Tier 3b: bound the spatial growth rate of the size field (Alauzet) so quad
   * size never shears across a steep density step. Max world-space size growth
   * per unit distance; 0 = off. Typical 0.3–1.0. Applies to auto OR painted
   * density. */
  float density_gradation = 0.5f;
  /* Tier 3b: gradation-limiter relaxation sweep cap. */
  int density_gradation_iters = 10;

  /* Tier 9: field-aligned input pre-remesh — clean the working triangulation's
   * flow (Botsch-Kobbelt to a curvature size field + field-aligned smooth)
   * before the field solve. Geometry only; the final reproject snaps the output
   * back onto the full-res original. Off = pipeline unchanged. */
  bool pre_remesh = false;
  /* Pre-pass edge length. 0 = auto: solve_edge_length when set (field-align at
   * the solve resolution); else 0.7x the resolved quad edge length, floored at
   * half the input's median edge so the pre-pass never over-refines. */
  float pre_remesh_target = 0.0f;
  /* Outer convergence iterations. 0 = auto from the measured input (resolution
   * ratio + fold/irregularity level, clamped to [3,6]); converge_eps usually
   * stops earlier on clean input. */
  int pre_remesh_iters = 0;
  /* Drive the BK band from the per-vertex curvature size field (Tier 3 chain,
   * regenerated on the evolving mesh each outer iter); false = uniform target.
   * Regeneration overwrites a painted .remesh.v.density on the WORKING copy, so
   * painted-density users who also quantize with it should set this false. */
  bool pre_remesh_density = true;
  /* Per-edge-hop growth cap on the pre-pass size field (Tier 3b); 0 disables
   * (A/B only — a size cliff makes the BK band churn at the boundary). */
  float pre_remesh_gradation = 0.5f;
  int pre_remesh_gradation_iters = 10;
  /* Smooth blend: 0 isotropic ↔ 1 field-aligned. */
  float pre_remesh_align = 1.0f;
  /* Recompute the rough cross field every N outer iters. */
  int pre_remesh_field_cadence = 2;
  /* Isotropic denoise sweeps before the field is trusted. -1 = auto from input
   * noise (1 clean / 2 default / 4 noisy); 0 = none (keep crisp features). */
  int pre_remesh_bootstrap_iters = -1;
  /* Inner field-aligned smooth sweeps per outer iter + relaxation factor. */
  int pre_remesh_smooth_iters = 5;
  float pre_remesh_smooth_lambda = 0.5f;
  /* Early-out: stop once an outer iter's smooth moves every vertex less than
   * eps·target. On by default — the real per-input adaptation (clean meshes
   * settle in 2-3 iters). 0 = always run all iters. */
  float pre_remesh_converge_eps = 0.05f;
  /* Pin boundary loops + dihedral-sharp creases (Tier 9c) through collapse and
   * smooth so the iterated flow follows features instead of eroding them. */
  bool pre_remesh_preserve_features = true;
  float pre_remesh_sharp_angle = 0.7853982f;
  /* Print the pre-pass per-iter convergence summary + oscillation verdict
   * (dyntopo::printTraceSummary) to stderr. */
  bool pre_remesh_trace = false;

  /* Bound out-of-line in remesh/bindings.cc (keeps the binding headers out of
   * this header). Crosses the seam by value → registers a copy constructor. */
  static litestl::binding::types::Struct<RemeshParams> *defineBindings();
};

} // namespace sculptcore::remesh
