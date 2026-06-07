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
 * topology + recomputes normals first. */
void computeFeatureTags(mesh::Mesh &m, float sharp_angle);

} // namespace sculptcore::remesh
