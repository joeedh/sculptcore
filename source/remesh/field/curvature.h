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

/* Estimate principal curvatures + directions at every vertex (Cohen-Steiner &
 * Morvan normal-cycle shape operator over the 1-ring) into the .remesh.v.*
 * TEMP layers. Thaws topology + recomputes normals first. */
void computeCurvature(mesh::Mesh &m);

} // namespace sculptcore::remesh
