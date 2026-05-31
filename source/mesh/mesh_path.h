#pragma once

#include "litestl/util/vector.h"

namespace sculptcore::mesh {
struct MeshBase;

// Dijkstra shortest path over mesh edges, weighted by 3D edge length, from
// vStart to vEnd. Fills outVerts with the vertex sequence [vStart .. vEnd] and
// returns true; returns false (outVerts cleared) if the inputs are invalid or
// vEnd is unreachable. The compute core of the seam/boundary marking tool
// (Wave 5): the tool calls this between the click-anchored start vertex and the
// vertex under the cursor, then flags the path edges. Requires live disk links.
bool shortestEdgePath(MeshBase *m, int vStart, int vEnd,
                      litestl::util::Vector<int> &outVerts);

} // namespace sculptcore::mesh
