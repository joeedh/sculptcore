#pragma once

/* Quad extraction from the integer-grid map (M6).
 *
 * M5 leaves a per-corner integer-grid map in .remesh.c.uv: within each triangle
 * the (u, v) is linear, and across every edge the two sides agree up to an
 * integer-grid automorphism (a 90-degree rotation `.remesh.e.period` plus an
 * integer translation). The output quad mesh is the preimage of the integer
 * lattice: its vertices are the surface points whose (u, v) is an integer pair,
 * its edges are the unit segments of the iso-lines u=int / v=int, and its faces
 * are the unit grid cells.
 *
 * We extract it QEx-style (Ebke 2013): enumerate the integer lattice points per
 * triangle and weld them by 3D position into grid vertices; trace each unit
 * iso-line segment across the triangulation (switching charts by the per-edge
 * transition) to recover grid arcs; then walk the arc rotation system to emit
 * one quad per grid cell. Because M5 made every iso-line close (loop-closure
 * residual 0), the traced arcs close into quads with no spiraling.
 *
 * Reads .remesh.c.uv (M5). Returns a freshly-allocated all-quad Mesh; the input
 * is left intact. See documentation/plans/quad-remeshing.md. */

#include "litestl/math/vector.h"

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::remesh {

struct ExtractParams {
  /* Weld tolerance for identifying grid vertices, as a fraction of the input
   * bounding-box diagonal. Two lattice points closer than this in 3D are the
   * same grid vertex (the map is continuous, so a shared point computed from two
   * triangles agrees to float precision — well below this). */
  double weld_tol = 1e-5;
  /* Close *odd*-length cap rims too. An odd loop is unquadable (a quad disk has
   * even boundary), so closing it costs one cap triangle — trading the all-quad
   * guarantee for a watertight surface. Default off: odd rims are left open (the
   * all-quad pipeline contract). On organic inputs with fold tangles this fills
   * the residual odd holes. Even rims are always capped regardless. */
  bool cap_odd_holes = false;
};

struct ExtractStats {
  int num_grid_verts = 0;
  int num_quads = 0;
  int num_arcs = 0;          // directed grid arcs traced
  int open_arcs = 0;         // arcs that ran off a boundary (no neighbour quad)
  int nonquad_cells = 0;     // cell walks that did not close in 4 steps
  bool ok = false;
};

/* Extract the quad mesh encoded by .remesh.c.uv. The input must already carry
 * the M5 integer-grid map (run computeQuantization first). Thaws topology.
 * Returns nullptr if no map / no lattice points are present. */
mesh::Mesh *extractQuadMesh(mesh::Mesh &m, const ExtractParams &params,
                            ExtractStats &stats);

} // namespace sculptcore::remesh
