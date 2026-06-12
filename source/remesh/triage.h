#pragma once

/* Tier-1 input triage (plans/quad-remeshing-filtering.md): make the working tri
 * copy sane before any field math. buildTriCopy assumes clean, hole-free,
 * manifold input — messy/scanned/Meshy characters violate all of that.
 *
 * triageMesh does the high-value, low-risk subset, IN PLACE on the work copy:
 *   - welds near-coincident verts (spatial hash + union-find over a bbox-scaled
 *     tolerance), then REBUILDS topology on the remapped verts, dropping faces
 *     that degenerate after the remap and duplicate faces,
 *   - drops degenerate / zero-area faces and residual face-less (wire) edges,
 *   - optionally drops tiny disconnected components (feeds Tier 6),
 *   - DETECTS (does not repair) non-manifold edges/verts and records counts.
 *
 * Every step is targeted + gated: on clean input nothing is welded, no face is
 * degenerate, no edge is wire, tiny-component drop is opt-in — so the mesh is
 * left byte-identical (the Tier-1 no-op-on-good-input contract). Operates on the
 * triangulated work mesh (dedup keys triangles).
 *
 * fillInputHoles is the separate Tier-6.3 pre-solve input hole policy: close
 * tiny input boundary loops (punctures, nostrils) by triangulating them before
 * the field solve, preserving large boundaries (cuffs, mouths) as open. It is
 * distinct from the output cap path in extract/quad_extract.cc, which closes
 * residual rims of the *extracted* quad mesh. */

#include "mesh/mesh.h"

namespace sculptcore::remesh {

struct TriageParams {
  // Weld tolerance as a fraction of the mesh bbox diagonal. <= 0 disables welding.
  float weld_rel = 1e-5f;
  // Drop connected components with fewer than this fraction of the total verts.
  // 0 = keep all (the default; tiny-component removal is opt-in).
  float min_component_frac = 0.0f;
};

struct TriageReport {
  bool ran = false;                 // triageMesh actually executed
  int welded_verts = 0;             // verts merged away by welding
  int removed_degenerate_faces = 0; // zero-area / repeated-vertex faces dropped
  int removed_duplicate_faces = 0;  // faces with a duplicate vertex set dropped
  int removed_wire_edges = 0;       // residual face-less edges killed
  int removed_components = 0;       // tiny disconnected components dropped
  int removed_component_verts = 0;  // verts removed with those components
  // Detect-only (no repair in v1): counts on the post-cleanup mesh.
  int non_manifold_edges = 0; // edges with radial multiplicity > 2
  int non_manifold_verts = 0; // verts with > 2 incident boundary edges (pinch)
  // Tier 6.3 input hole policy (fillInputHoles).
  int input_holes_filled = 0;    // tiny boundary loops triangulated closed
  int input_holes_kept = 0;      // boundary loops preserved (large/untraceable)
  int input_hole_fill_faces = 0; // triangles added by the fill
  // Tier 6.6 thin double-sided sheet detection (detectThinSheets) — detect-only.
  int thin_sampled_faces = 0;  // tri faces probed (stride-sampled, max 4096)
  int thin_paired_faces = 0;   // probed faces with an opposing face within range
  float thin_area_frac = 0.0f; // area-weighted thin fraction of the sample
  float thin_thickness = 0.0f; // distance threshold used (caller-derived)
  bool thin_sheet = false;     // majority of sampled area is double-sided
};

/* Clean @p m in place per @p params, recording what was removed in @p report.
 * Recomputes normals only if a mutation actually occurred. */
void triageMesh(mesh::Mesh &m, const TriageParams &params, TriageReport &report);

/* Tier 6.3: fill (triangulate) input boundary loops whose rim length is below
 * @p max_frac of the total boundary length; longer loops, and loops touching a
 * pinch vert, are preserved. Counts into @p report's input_holes_* fields. */
void fillInputHoles(mesh::Mesh &m, float max_frac, TriageReport &report);

/* Tier 6.6: detect thin double-sided sheets — detect-only, no repair. A face is
 * "thin-paired" when an opposing-normal triangle (unit-normal dot < -0.5, no
 * shared vertex) passes within @p thickness of its centroid, stacked along its
 * normal — the near-zero-thickness-shell configuration the cross field / param
 * cannot resolve at quad scale. Probes a deterministic stride sample (<= 4096
 * tri faces) against a uniform triangle grid; fills @p report's thin_* fields
 * (thin_sheet = over half the sampled area is paired). Triangle faces only. */
void detectThinSheets(mesh::Mesh &m, float thickness, TriageReport &report);

} // namespace sculptcore::remesh
