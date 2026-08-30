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
  /* Close *odd*-length cap rims too. An odd rim is unquadable alone (a quad
   * patch has even boundary), but odd rims come in pairs per component: each
   * pair is joined by splitting the quad strip between them lengthwise, growing
   * both rims one vert (even) — all-quad, at the cost of valence defects along
   * the strip. Unpairable odd rims chord-split like even ones (each split sheds
   * an even piece; the odd remainder shrinks to fan size) and close with one
   * cap triangle total. Off: odd rims are left open (strict all-quad). Even
   * rims are always capped. */
  bool cap_odd_holes = true;
};

struct ExtractStats {
  int num_grid_verts = 0;
  int num_quads = 0;
  int num_arcs = 0;      // directed grid arcs traced
  int open_arcs = 0;     // arcs that ran off a boundary (no neighbour quad)
  int nonquad_cells = 0; // cell walks that did not close in 4 steps
  // Cap-path hole accounting: every output boundary rim (pinched rims first
  // split into simple sub-loops, counted individually) lands in exactly one
  // bucket — capped, or one open_* skip reason (holes_open sums the open_*).
  int holes_capped = 0;
  int holes_capped_odd = 0;    // odd rims closed (paired ones all-quad; one
                               // cap triangle each on the unpaired fallback)
  int odd_rims_paired = 0;     // odd rims made even by a strip ladder split
  int holes_pinched_split = 0; // rims split at repeated verts into simple loops
  int holes_open = 0;
  int holes_open_border = 0;   // real border: rim tracks the input boundary
  int holes_open_odd = 0;      // odd rim with cap_odd_holes off
  int holes_open_size = 0;     // n < 3 or n > the cap-loop limit
  int holes_open_untraced = 0; // rim trace failed (tangled boundary)
  int cap_max_fan = 0;         // largest center-fan rim emitted while capping
  bool ok = false;
};

/* Extract the quad mesh encoded by .remesh.c.uv. The input must already carry
 * the M5 integer-grid map (run computeQuantization first). Thaws topology.
 * Returns nullptr if no map / no lattice points are present. */
mesh::Mesh *
extractQuadMesh(mesh::Mesh &m, const ExtractParams &params, ExtractStats &stats);

} // namespace sculptcore::remesh
