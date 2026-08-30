#pragma once

/* Singularity adjustment / curl reduction for the quad remesher (M3).
 *
 * The M2 cross field is a relaxed complex least-squares solve: its per-face
 * phase θ = arg(c)/4 is not the true minimizer of the angular smoothness energy
 * (the unbounded magnitude |c| soaks up energy near singularities), so the field
 * carries avoidable curl. M3 re-solves for the smoothest *phase* field with the
 * period jumps (hence the singularities) held fixed — a single real SPD Poisson
 * solve on the face dual graph — which provably lowers the per-edge curl. See
 * documentation/plans/quad-remeshing.md (Liu et al. 2024).
 *
 * XXX: singularity *relocation* and same-sign *merge* (general coordinated
 * period moves) are still deferred; opposite-pair *cancellation* exists below
 * (cancelSingularityPairs) and is built on the same period-flip-along-a-path
 * primitive a future relocation pass needs. Pins are honored by both.
 *
 * Honors the optional .remesh.v.pole_pinned (bool) user-pin layer. Eigen is kept
 * out of the header; the sparse solve lives in singularity_adjust.cc. */

#include "litestl/util/vector.h"

#include <cstdint>

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::remesh {

struct SingularityAdjustParams {
  float gauge_eps = 1e-6f; // Tikhonov gauge fix for the Laplacian null space
  uint32_t seed = 1u;      // reserved for future stochastic relocation
};

struct SingularityAdjustStats {
  int num_faces = 0;
  int num_singularities = 0; // interior verts with nonzero index
  int index_sum = 0;         // Σ quarter-index (== 4χ)
  double curl_before = 0.0;  // per-edge curl L2 of the input field
  double curl_after = 0.0;   // after the fixed-period Poisson re-solve
};

/* L2 norm of the per-edge cross-field curl (the period-reduced smoothness
 * residual reduce(θ_b − θ_a − ρ) over interior manifold edges) for the current
 * .remesh.f.theta. */
double crossFieldCurl(mesh::Mesh &m);

struct SingularityPairStats {
  int num_singularities = 0; // verts with nonzero pole index
  int close_pairs = 0;       // opposite-index pairs within max_hops (each once)
  int clutter_verts = 0;     // poles participating in at least one such pair
};

/* Tier-5 gate diagnostic: opposite-index singularity pairs within @p max_hops
 * vertex hops — the spurious clutter a pair-cancellation pass could annihilate.
 * Reads .remesh.v.pole_index (computeCrossField must have run). @p pair_verts,
 * when given, receives the participating pole verts (deterministic order). */
SingularityPairStats findSingularityPairs(
    mesh::Mesh &m, int max_hops, litestl::util::Vector<int> *pair_verts = nullptr);

/* Re-solve the smoothest phase field for the M2 period jumps and rewrite
 * .remesh.f.theta / .remesh.e.period / .remesh.v.pole_index. Runs
 * computeCrossField first if no field is present. Thaws topology + recomputes
 * normals. */
SingularityAdjustStats adjustSingularities(mesh::Mesh &m,
                                           const SingularityAdjustParams &params);

struct SingularityCancelParams {
  float target_edge_length = 0.0f; // quad-grid edge length; <= 0 disables the pass
  float max_sep = 1.5f;            // pair-separation gate, in target_edge_length units
  int max_rounds = 3;              // bounded find → flip → re-solve rounds
  float gauge_eps = 1e-6f;         // Tikhonov gauge fix for the re-solves
  uint32_t seed = 1u;              // reserved (selection is deterministic)
};

struct SingularityCancelStats {
  int rounds = 0;            // rounds that ran a re-solve
  int attempted_pairs = 0;   // pairs selected for period flips
  int cancelled_pairs = 0;   // pole pairs actually annihilated
  int reverted_rounds = 0;   // rounds rolled back by the acceptance check
  int num_singularities = 0; // final count (pinned poles included)
  int index_sum = 0;         // final Σ quarter-index (conserved, == 4χ)
  double curl_after = 0.0;
};

/* Tier-5 noise-pair cancellation: annihilate opposite-index (±1) pole pairs
 * within max_sep·target_edge_length geodesic distance — sub-resolution pairs
 * the output lattice cannot represent as distinct irregular vertices. Flips
 * the implied periods along the shortest vertex path between the poles, then
 * re-runs the fixed-period Poisson; a round is reverted unless the index sum
 * is conserved and the singularity count strictly drops. Skips pinned poles.
 * Runs adjustSingularities first when the field/pole attributes are absent. */
SingularityCancelStats cancelSingularityPairs(mesh::Mesh &m,
                                              const SingularityCancelParams &params);

} // namespace sculptcore::remesh
