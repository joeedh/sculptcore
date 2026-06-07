#pragma once

/* Seamless parametrization (M4).
 *
 * Computes a real-valued (u, v) per corner whose gradient aligns with the M2/M3
 * cross field, with edge transitions in the seamless group: a 90°·period_e
 * rotation plus a real translation (zero on non-cut edges, free on cut edges).
 *
 * Method (constraint-eliminated MIQ / QuadCover, integers free): the cut graph
 * makes the non-cut face adjacency a tree, so a per-face integer gauge rotation
 * R_f (BFS-accumulated period jumps) can rotate every face's cross axes into one
 * consistent "u" direction. In that gauged frame the seam rotations across
 * non-cut edges vanish, the two corners on either side of a non-cut edge share a
 * single (u, v) variable (union-find over corners), and u and v decouple into
 * two scalar Poisson solves on the same cotangent-style Laplacian over the cut
 * mesh (Eigen SimplicialLDLT, SPD). The per-corner output is then un-gauged
 * (rotated back by −R_f·90°), which makes the non-cut transition exactly
 * R(−90°·period) with zero translation. See documentation/plans/quad-remeshing.md.
 *
 * Writes .remesh.c.uv (float2), .remesh.e.translation (float2),
 * .remesh.f.gauge_rot (short), all TEMP. Reads optional .remesh.v.density
 * (use_density) and the cut graph's .remesh.e.is_cut. Eigen stays out of this
 * header. */

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::remesh {

struct SeamlessParamParams {
  float target_edge_length = 0.1f; // world-space target quad spacing
  bool use_density = false;        // scale spacing by 1/sqrt(.remesh.v.density)
  float gauge_eps = 1e-9f;         // Tikhonov shift for solver conditioning
};

struct SeamlessParamStats {
  int num_faces = 0;
  int num_corners = 0;
  int num_classes = 0;    // independent (u,v) variables on the cut mesh
  int num_cut_edges = 0;
  double grad_angle_err = 0.0;      // max |angle(∇u) − θ| over faces (rad, mod π/2)
  double max_seam_translation = 0.0; // max ‖t‖ on non-cut edges (should be ≈ 0)
  double min_jacobian = 0.0;        // min per-face det(∇u, ∇v) (informational)
  bool solved = false;
};

/* Build the cut graph, gauge, and Poisson-solve the seamless (u, v). Runs
 * computeCrossField first if no .remesh.f.theta is present. Thaws topology +
 * recomputes normals. */
SeamlessParamStats computeSeamlessParam(mesh::Mesh &m,
                                        const SeamlessParamParams &params);

} // namespace sculptcore::remesh
