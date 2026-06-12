#pragma once

/* Per-face direction constraints for the cross-field solve (M2).
 *
 * Gathers, for each face, a 4-RoSy target direction expressed as a unit complex
 * c = exp(i·4·φ) in the face's own tangent frame (so the four cross arms are
 * φ, φ+π/2, φ+π, φ+3π/2). Hard constraints (user stroke / sharp / boundary
 * edges) pin the face; soft constraints (principal curvature, weighted by
 * anisotropy) pull on it. See documentation/plans/quad-remeshing.md. */

#include "litestl/util/vector.h"

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::remesh {

struct CrossFieldParams;

/* One face's constraint. (cx, cy) is exp(i·4·φ) in the face frame; (0,0) means
 * unconstrained. is_hard faces are pinned (solver fixes c_f to the target);
 * soft faces add a quadratic data term scaled by weight. */
struct FaceConstraint {
  float cx = 0.0f, cy = 0.0f;
  float weight = 0.0f;
  bool is_hard = false;
};

/* Fill `out` (indexed by face id, sized to m.f.capacity()) with per-face
 * constraints. Reads .remesh.f.stroke_dir (optional) and computes curvature +
 * feature tags (M1) as needed. Faces are visited via the shared faceFrame(). */
void gatherConstraints(mesh::Mesh &m, const CrossFieldParams &params,
                       litestl::util::Vector<FaceConstraint> &out);

} // namespace sculptcore::remesh
