#pragma once

/* Feature-aligned cross field — a pseudo 4-RoSy direction field maintained
 * incrementally under the feature-align smoothing brush (topology rake).
 *
 * The field is a single per-vertex tangent direction stored as a *persistent*
 * (saved) float3 vertex attribute, `crossfield`. It is a cross field: a
 * direction d and its three 90°-about-normal rotations are treated as
 * equivalent, so all diffusion must rotate a source direction to the nearest
 * 90° image of the destination before accumulating (otherwise opposed/quarter-
 * turned neighbours cancel).
 *
 * `updateCrossFieldRegion` is the incremental update run once per dab over the
 * dab's vertex region: it (1) hard-seeds boundary-feature verts (sharp / seam /
 * face-set boundary / mesh border) to the tangent along their feature curve,
 * (2) soft-seeds still-unset interior verts from local principal curvature, then
 * (3) diffuses the field across the region with the 4-fold-aware step so feature
 * directions propagate inward. Only region verts are written, so the field grows
 * outward as the stroke covers more of the mesh — and because it is saved, it
 * persists across sessions.
 *
 * Marked NOINTERP so dyntopo splits don't linearly average two cross directions
 * (which can cancel); freshly-created verts start cleared and are filled by the
 * next dab's diffuse. See documentation/plans/feature-align-brush.md. */

#include "litestl/util/vector.h"

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::brush {

// Persistent (saved) per-vertex float3 cross-field attribute.
inline constexpr const char *CROSS_FIELD_ATTR = "crossfield";

struct FeatureFieldParams {
  int diffuse_iters = 6;     // 4-fold-aware Jacobi/Gauss-Seidel sweeps per dab
  bool use_features = true;  // hard-seed sharp / seam / face-set / border verts
  bool use_curvature = true; // soft-seed unset interior verts from curvature
};

/* Ensure the persistent `crossfield` float3 vertex layer exists (NOINTERP, not
 * TEMP). Cheap if already present; does not materialize every page. */
void ensureCrossField(mesh::Mesh &m);

/* Seed + diffuse the cross field over `verts` (the dab region). Requires live
 * topology (walks the vertex disk). `verts` order must be deterministic for
 * cross-backend parity — pass the spatial nodes' unique_verts in node order. */
void updateCrossFieldRegion(mesh::Mesh &m,
                            const litestl::util::Vector<int> &verts,
                            const FeatureFieldParams &params);

} // namespace sculptcore::brush
