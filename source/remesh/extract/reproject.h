#pragma once

/* M6 reprojection: snap the extracted quad mesh back onto the original input
 * surface using the M1 BVH closest-point query, with optional Laplacian
 * smoothing between snaps to relax kinks without drifting off the surface.
 * See documentation/plans/quad-remeshing.md. */

#include "mesh/mesh.h"

namespace sculptcore::remesh {

struct ReprojectParams {
  int iterations = 1;         // [smooth*k -> snap] passes
  int smooth_iterations = 0;  // Laplacian passes before each snap (0 = pure snap)
  float smooth_lambda = 0.5f; // Laplacian step in [0,1]
  bool pin_boundary = true;   // keep boundary-loop vertices fixed while smoothing
  /* Sheet filter: a snap candidate triangle is rejected when its unit normal
   * dots with the vertex normal below this. -0.5 rejects only clearly-opposite
   * sheets (the thin-feature fold-back) while tolerating noisy vertex normals
   * near singularities; <= -1 disables the filter. */
  float sheet_min_dot = -0.5f;
  /* Tier 9g: the pre-remeshed work mesh carrying .remesh.v.src_face anchors into
   * `input`. When set, snaps transport those anchors (one work-tree query, then
   * local walks on `input`) instead of global queries; failures fall back to the
   * filtered global path. null = the classic global snap. */
  mesh::Mesh *anchor_work = nullptr;
};

struct ReprojectStats {
  int num_verts = 0;
  float max_dist = 0.0f;  // farthest a vertex was moved on the final snap
  float mean_dist = 0.0f; // mean snap distance on the final snap
};

/* Snap every vertex of `out` onto the closest point of `input`'s surface.
 * `input` is read-only (used only to build a BVH); `out` is modified in place.
 * `input` is expected to be triangulated by the caller (the SpatialTree fans
 * n-gons internally, but the pipeline already triangulates). */
ReprojectStats reprojectToSurface(mesh::Mesh &out, mesh::Mesh &input,
                                  const ReprojectParams &params = {});

} // namespace sculptcore::remesh
