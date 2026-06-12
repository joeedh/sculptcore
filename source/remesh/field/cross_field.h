#pragma once

/* Cross-field (4-RoSy) solver for the quad remesher (M2).
 *
 * Solves a smooth per-face cross field θ (mod π/2) aligned to curvature /
 * sharp features / user strokes (Diamanti 2014, complex-polynomial 4-PolyVector:
 * each face carries a unit complex c = exp(i·4·θ) in its tangent frame; the
 * edge smoothness term is |c_a·exp(i·4·ρ) − c_b|² with ρ the parallel-transport
 * angle between adjacent frames). Outputs:
 *   .remesh.f.theta      (float)  cross angle in the face frame, in (−π/4, π/4]
 *   .remesh.e.period     (short)  period jump 0..3, oriented face(e.c) → radial
 *   .remesh.v.pole_index (short)  computed quarter-index (Σ == 4χ) per interior
 *                                 vertex; user pins (M6) would pre-seed this
 * All TEMP. See documentation/plans/quad-remeshing.md.
 *
 * Kept Eigen-free in the header; the sparse solve lives in cross_field.cc. */

#include "litestl/math/vector.h"

#include <cstdint>

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::remesh {

struct CrossFieldParams {
  bool use_curvature = true;       // soft-align to principal curvature
  bool use_sharp_features = true;  // hard-align to sharp/boundary edges
  float sharp_angle = 0.7853982f;  // dihedral threshold for "sharp" (radians)
  float feature_hysteresis = 0.0f; // Tier 7a: weak-tag band below sharp_angle
  int feature_min_chain = 0;       // Tier 7b: drop unanchored sharp chains shorter than this
  float curvature_weight = 1.0f;   // soft constraint scale (× anisotropy)
  float field_smoothness = 1.0f;   // per-edge smoothness weight (wsmooth)
  uint32_t seed = 1u;              // determinism for the eigen-fallback seed
  int curvature_smooth_iters = 0;       // Tier 2a: tensor-field Jacobi sweeps (0 = today)
  float curvature_smooth_lambda = 0.5f; // Tier 2a: per-sweep blend 0..1
};

struct CrossFieldStats {
  int num_faces = 0;
  int num_singularities = 0; // interior verts with nonzero index
  int index_sum = 0;         // Σ quarter-index over interior verts (== 4χ)
  bool solved_eigen = false; // true = fell back to smoothest-eigenvector
};

/* Solve the cross field into the .remesh.* TEMP attrs. Computes curvature /
 * feature tags first as needed (M1). Thaws topology + recomputes normals. */
CrossFieldStats computeCrossField(mesh::Mesh &m, const CrossFieldParams &params);

/* Per-face orthonormal tangent frame (X, Y, N): N = unit face normal, X = the
 * first face edge projected into the tangent plane, Y = N × X. Deterministic
 * from geometry alone so M4's seamless param recomputes the identical frame the
 * field's θ is expressed in. */
void faceFrame(mesh::Mesh &m, int f, litestl::math::float3 &X,
               litestl::math::float3 &Y, litestl::math::float3 &N);

} // namespace sculptcore::remesh
