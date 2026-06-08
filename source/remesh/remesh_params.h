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
  /* Target quad edge length (world units). Drives the seamless-parametrization
   * scale (M4) and therefore the output face count. */
  float target_edge_length = 0.1f;

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

  /* M4: scale the parametrization metric by 1/density so quad spacing follows
   * the per-vertex `.remesh.v.density` map. Off = uniform spacing. */
  bool use_density = false;

  /* M6: snap each output vertex back onto the input surface via the BVH
   * closest-point query. Off = leave extracted positions as-is (debugging). */
  bool reproject = true;

  /* M6: also close odd-length cap rims (one cap triangle each). An odd rim is
   * unquadable, so this trades the all-quad guarantee for a watertight result.
   * Off = leave odd holes open (strict all-quad). Useful on organic inputs whose
   * singularity tangles leave a handful of residual odd holes. */
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

  /* Tier 2a: Jacobi-diffuse the per-vertex shape operator over the one-ring
   * before eigendecomposition, denoising the principal-curvature field without
   * touching geometry. 0 = today's raw 1-ring estimate (no smoothing). */
  int curvature_smooth_iters = 0;
  /* Tier 2a: per-sweep blend in [0,1] for the curvature tensor diffusion. */
  float curvature_smooth_lambda = 0.5f;

  /* Bound out-of-line in remesh/bindings.cc (keeps the binding headers out of
   * this header). Crosses the seam by value → registers a copy constructor. */
  static litestl::binding::types::Struct<RemeshParams> *defineBindings();
};

} // namespace sculptcore::remesh
