#pragma once

namespace sculptcore::mesh {
struct MeshBase;

// Generate a per-corner UV map (FLOAT2 corner attribute `uvName`) from marked
// seam edges (boundary::EDGE_SEAM). The simple unwrapper of the
// boundary-conditions plan (Wave 7):
//   1. flood-fill faces into charts bounded by seam edges,
//   2. project each chart's corners onto its area-weighted ("group") normal
//      plane,
//   3. shelf box-pack the chart bounding boxes into [0,1].
// Per-corner (not per-vertex) storage so a vertex on a seam carries distinct
// UVs per chart. Returns the chart count. Requires live topology.
//
// LIMITATIONS: each chart gets a single *planar* projection, so a chart that
// curves/closes around its group normal will fold and overlap itself in UV
// space — charts must be reasonably developable (cut them with more seams
// otherwise). An empty mesh (no faces) returns 0 and creates NO layer. `margin`
// is the pre-pack padding in UV space added around each chart *before* the whole
// set is rescaled to fit [0,1], so the final inter-chart gap shrinks as the
// chart count grows (it is not the literal gap in the [0,1] result).
int generateUVFromSeams(MeshBase *m, const char *uvName, float margin = 0.01f);

} // namespace sculptcore::mesh
