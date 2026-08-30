#pragma once

/* Feature-edge tagging for the quad remesher (M1).
 *
 * computeFeatureTags(m, sharp_angle) writes per-edge boolean TEMP layers the
 * cross-field pins its axes to (M2):
 *   .remesh.e.is_sharp     dihedral angle exceeds `sharp_angle` (radians)
 *   .remesh.e.is_boundary  open mesh boundary (1 face) or non-manifold/wire
 * See documentation/plans/quad-remeshing.md. */

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::remesh {

/* Tag sharp + boundary edges into the .remesh.e.* TEMP bool layers. Thaws
 * topology + recomputes normals first. Tier 7a: `feature_hysteresis` (radians,
 * clamped to [0, sharp_angle]) also tags weaker edges (dihedral above
 * sharp_angle - hysteresis) when vertex-connected to a strong edge; 0 = off.
 * Tier 7b: `feature_min_chain` drops sharp chains shorter than this many edges
 * unless both ends anchor at a junction/boundary (0 = off). */
void computeFeatureTags(mesh::Mesh &m,
                        float sharp_angle,
                        float feature_hysteresis = 0.0f,
                        int feature_min_chain = 0);

} // namespace sculptcore::remesh
