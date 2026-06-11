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
 * XXX: full iterative singularity *relocation* / merge / split (coordinated
 * multi-edge period moves) is deferred — after the fixed-period Poisson re-solve
 * the field already sits at the integer-optimum for its current singularities,
 * so no single-edge move improves it, and the no-spiral guarantee comes from
 * M5's quantization regardless. Pins are read so the relocation pass can honor
 * them once added.
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
SingularityPairStats findSingularityPairs(mesh::Mesh &m, int max_hops,
                                          litestl::util::Vector<int> *pair_verts = nullptr);

/* Re-solve the smoothest phase field for the M2 period jumps and rewrite
 * .remesh.f.theta / .remesh.e.period / .remesh.v.pole_index. Runs
 * computeCrossField first if no field is present. Thaws topology + recomputes
 * normals. */
SingularityAdjustStats adjustSingularities(mesh::Mesh &m,
                                           const SingularityAdjustParams &params);

} // namespace sculptcore::remesh
