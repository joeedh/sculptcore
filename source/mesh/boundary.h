#pragma once

// Boundary-condition flags (boundary-conditions wave). A mesh carries structured
// overlays — projected "curve" edges, sharp edges, UV seams, poly groups, UV
// charts — that sculpting and (later) remeshing must respect. This module is the
// data model:
//
//   * Source-of-truth edge flags (projected/sharp/seam) are user-set bool edge
//     attributes that persist with the mesh.
//   * Derived edge flags (poly-group boundary, UV-chart boundary) are computed
//     from face poly-group ids / UV layers and are TEMP.
//   * A per-vertex classification bitmask summarizes which boundary types touch
//     each vertex — consumed by the boundary-aware smooth brush.
//
// Recompute is LAZY: changing a source flag (or a poly id / UV) marks the
// affected verts/edges "boundary-dirty"; recomputeDirty() refreshes only the
// dirty elements and clears the markers. Derived recompute walks face/edge
// connectivity, so it requires live topology (not the frozen-topo brush path).

#include "litestl/util/vector.h"

namespace sculptcore::mesh {
struct MeshBase;
struct BoolAttrView;
}

namespace sculptcore::mesh::boundary {

// Source-of-truth edge flags (user-set; persistent). The seam/boundary marking
// tool sets these; smoothing + remeshing consume them.
inline constexpr const char *EDGE_PROJECTED = ".boundary.edge.projected";
inline constexpr const char *EDGE_SHARP = ".boundary.edge.sharp";
inline constexpr const char *EDGE_SEAM = ".boundary.edge.seam";

// Derived edge flags — recomputed from poly-group face ids / UV layers (TEMP).
inline constexpr const char *EDGE_POLYGROUP = ".boundary.edge.polygroup";
inline constexpr const char *EDGE_UVCHART = ".boundary.edge.uvchart";

// Lazy dirty markers (TEMP): set when sources change, cleared on recompute.
inline constexpr const char *EDGE_DIRTY = ".boundary.edge.dirty";
inline constexpr const char *VERT_DIRTY = ".boundary.vert.dirty";

// Derived per-vertex classification bitmask (TEMP int): the union of boundary
// types present on the vertex's incident edges.
inline constexpr const char *VERT_CLASS = ".boundary.vert.class";

// Poly-group face attribute (INT) painted by the polygroup brush; its
// inter-group edges are derived poly-group boundaries.
inline constexpr const char *FACE_GROUP = "group";

// Per-vertex classification bits (stored in VERT_CLASS).
//
// Bits 0..4 are the boundary-type union (which types touch the vertex). Bit 5
// (BC_ENDPOINT) is a *derived* flag: the vertex has exactly one constraint edge
// of its dominant boundary type, so it is the dangling end of a feature chain.
// A smooth brush relaxes such a vertex like an interior one (consider all
// neighbors) so the chain end follows the surface instead of collapsing onto
// its single neighbor. SHARP overrides the smooth types when picking the
// dominant type for both the endpoint count and the smoothing rule (a sharp
// crease vertex slides along the crease tangent; a projected/seam/etc. vertex
// slides in the surface tangent plane). Keep BC_ENDPOINT out of BC_TYPE_MASK so
// the neighbor-share test (dom & nb.class) only compares type bits.
enum BoundaryClass : int {
  BC_NONE = 0,
  BC_PROJECTED = 1 << 0,
  BC_SHARP = 1 << 1,
  BC_SEAM = 1 << 2,
  BC_POLYGROUP = 1 << 3,
  BC_UVCHART = 1 << 4,
  BC_TYPE_MASK = 0x1F, // BC_PROJECTED..BC_UVCHART

  BC_ENDPOINT = 1 << 5, // derived: exactly one dominant-type constraint edge
};

// Set a source-of-truth edge flag (one of EDGE_PROJECTED/SHARP/SEAM), creating
// the layer on demand, and mark the edge + its endpoint verts boundary-dirty.
void setEdgeFlag(MeshBase *m, const char *flagName, int e, bool state);

// Read a source/derived edge flag (false if the layer doesn't exist).
bool edgeFlag(MeshBase *m, const char *flagName, int e);

// Resolve an edge-flag bool view by name (nullptr if the layer doesn't exist),
// so a hot loop can read flags via the view instead of a string-keyed lookup
// per element (what `edgeFlag` does each call).
BoolAttrView *findBoolEdgeView(MeshBase *m, const char *flagName);

// Mark elements boundary-dirty so the next recomputeDirty refreshes them.
void markEdgeDirty(MeshBase *m, int e);
void markVertDirty(MeshBase *m, int v);
void markAllDirty(MeshBase *m);

// Mark every edge + vertex of face @p f boundary-dirty. Used by the poly-group
// brush: painting a face can change the inter-group boundary on any of its
// edges, so its incident edges/verts must be reclassified. Walks the face loop
// — requires live topology.
void markFaceDirty(MeshBase *m, int f);

// Recompute derived edge flags (poly-group; UV-chart is a follow-up) and the
// per-vertex classification for every dirty element, clearing the markers.
// Requires live topology.
void recomputeDirty(MeshBase *m);

// Read a vertex's classification bitmask (0 if not yet computed).
int vertClass(MeshBase *m, int v);

// Polyline-graph stats over the union of every boundary edge flag (source +
// derived): out = [flaggedEdges, graphVerts, non2ValenceVerts, components].
// Non-2-valence vertices (endpoints/junctions) and component count are
// invariant under feature-preserving remeshing (splits/collapses along a
// feature curve only add/remove 2-valence chain verts), so they detect
// constraint-network damage. Call recomputeDirty first.
void graphStats(MeshBase *m, litestl::util::Vector<int> &out);

} // namespace sculptcore::mesh::boundary
