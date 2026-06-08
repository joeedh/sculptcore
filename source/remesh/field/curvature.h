#pragma once

/* Discrete principal-curvature estimator for the quad remesher (M1).
 *
 * computeCurvature(m) writes per-vertex principal directions + signed
 * magnitudes into TEMP attribute layers — the cross-field's curvature guidance
 * (M2):
 *   .remesh.v.kmin_dir, .remesh.v.kmax_dir  (float3, unit, tangent)
 *   .remesh.v.k                              (float2 = (kmin, kmax))
 * See documentation/plans/quad-remeshing.md. Kept Eigen-free in the header; the
 * eigensolver is confined to curvature.cc. */

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::remesh {

/* Tier 2: optional tensor-field denoising of the shape operator before its
 * eigendecomposition. Both default to today's behaviour (no smoothing). */
struct CurvatureParams {
  int smooth_iters = 0;       // Jacobi sweeps over the tensor field (0 = raw 1-ring)
  float smooth_lambda = 0.5f; // per-sweep blend 0..1
};

/* Estimate principal curvatures + directions at every vertex (Cohen-Steiner &
 * Morvan normal-cycle shape operator over the 1-ring) into the .remesh.v.*
 * TEMP layers. Thaws topology + recomputes normals first. With
 * params.smooth_iters > 0 the per-vertex shape operator is Jacobi-diffused over
 * the one-ring before eigendecomposition (Tier 2a), denoising the field without
 * touching geometry. */
void computeCurvature(mesh::Mesh &m, const CurvatureParams &params = {});

} // namespace sculptcore::remesh
