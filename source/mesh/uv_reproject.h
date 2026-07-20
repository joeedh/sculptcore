#pragma once

// UV slide-reprojection (the bsmooth.sbrush follow-up). A smoothing pass that
// slides vertices tangentially without changing topology leaves their corner
// UVs anchored to the old positions, so textures swim. This module re-anchors
// them: project each moved vertex's new position onto its old 1-ring fan and
// re-interpolate the UVs there, per UV wedge.

#include "litestl/math/vector.h"

#include <span>

namespace sculptcore::mesh {
struct MeshBase;
struct MeshCallbacks;
}

namespace sculptcore::mesh::uvproj {

/**
 * Re-interpolate the UVs of @p verts after a tangential move.
 *
 * @p oldCo holds the pre-move position of each vertex in @p verts (parallel
 * spans); ring vertices not listed are treated as unmoved (their current
 * position doubles as the old one), so a simultaneous (Jacobi) smooth pass is
 * handled by passing every moved vertex in one call.
 *
 * For every FLOAT2 corner layer tagged AttrUse::UV, a moved vertex's corners
 * are grouped into wedges by old-UV equality at the vertex — the actual UV
 * discontinuity, so stale derived chart flags cannot corrupt the result. The
 * new position is projected onto each wedge face's old corner triangle
 * (corner, next, prev) and the closest one's barycentric weights re-interpolate
 * that face's old corner UVs; every corner of the wedge receives the value.
 * All writes are computed first and applied afterwards, so the result is
 * independent of vertex order.
 *
 * Requires live topology (walks disk/radial cycles). Returns the number of
 * corners whose UV changed.
 *
 * @p cb optional: onCornerChange fires for each corner immediately before its
 * UV is written, so a meshlog captures the pre-state for undo.
 */
int reprojectVertUVs(MeshBase *m,
                     std::span<const int> verts,
                     std::span<const litestl::math::float3> oldCo,
                     MeshCallbacks *cb = nullptr);

} // namespace sculptcore::mesh::uvproj
