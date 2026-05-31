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
int generateUVFromSeams(MeshBase *m, const char *uvName, float margin = 0.01f);

} // namespace sculptcore::mesh
