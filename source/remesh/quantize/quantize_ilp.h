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
 * 90-degree rotations). We solve, re-round k_e = round(realized), and ramp lambda
 * until the translations lock onto integers (SimplicialLDLT each round). When
 * integrality is reached the IGM is valid (no spirals); if it cannot be reached
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
  // Local-injectivity stiffening rounds, run after the integers lock. The linear
  // MIQ map can fold where curvature concentrates (cap/crease cones); each round
  // ramps a Dirichlet+RHS weight on folded faces + their 1-ring (capped below the
  // locked-grid penalties) to pull them flat, keeping the lowest-fold result.
  // Stops early once folds plateau, so a generous count is cheap. 0 disables.
  int inj_iters = 25;
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
