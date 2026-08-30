#pragma once

/* Tier 3: automatic curvature-driven sizing field + gradation limiting.
 *
 * Both stages write the per-vertex .remesh.v.density (FLOAT, TEMP) layer that the
 * seamless parametrization (M4) and quantizer (M5) consume: local quad size scales
 * as 1/sqrt(density), so density > 1 → smaller quads, density < 1 → larger.
 *
 *   generateAutoDensity  — map (Tier-2-smoothed) principal curvature to density.
 *   limitDensityGradation — bounded-gradation prepass: cap the per-edge growth
 *                           ratio of the size field so neighbouring quads never
 *                           shear across a steep size step.
 *
 * Kept Eigen-free; curvature.cc owns the eigensolver. See
 * documentation/plans/quad-remeshing-filtering.md (Tier 3). */

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::remesh {

struct DensityParams {
  /* Target quad edge length: sets the dimensionless curvature scale s = k·L. */
  float target_edge_length = 0.1f;
  /* Clamp on the generated density (size range). density_max doubles as the
   * minimum-feature-size floor — it caps how small curvature can drive quads, so
   * sub-quad scan noise / tiny folds can't run the field to the ceiling. */
  float density_min = 0.25f;
  float density_max = 4.0f;
  /* Curvature smoothing used if curvature must be (re)computed for the field;
   * mirror the cross-field's so the density tracks the same denoised k. */
  int curvature_smooth_iters = 0;
  float curvature_smooth_lambda = 0.5f;
};

/* Generate .remesh.v.density from the per-vertex principal curvature. Reuses the
 * existing .remesh.v.k (the Tier-2-smoothed estimate the cross field already
 * computed) and only recomputes curvature if that layer is absent (e.g.
 * use_curvature was off). Creates the density layer; never throws. */
void generateAutoDensity(mesh::Mesh &m, const DensityParams &params);

/* Bounded-gradation limiter: cap the goal-length field h = L/sqrt(density) to
 * grow by at most a factor (1 + gradation) across any single edge, by expanding
 * worklist rings outward from fine regions with geometrically relaxed goals
 * (h[n] ← min(h[n], h[v]·(1+gradation)) to a fixed point). Hop/ratio-based, not
 * geometric-distance-based, so a per-edge size cliff is always widened even on a
 * coarse input mesh. Only ever refines (raises density); `iters` caps total work
 * (pops per vertex). No-op when gradation ≤ 0 or no density layer exists. */
void limitDensityGradation(mesh::Mesh &m,
                           float target_edge_length,
                           float gradation,
                           int iters,
                           float density_min,
                           float density_max);

} // namespace sculptcore::remesh
