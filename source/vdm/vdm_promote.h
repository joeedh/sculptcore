#pragma once

/** VDM region partition + promotion (displacementAndSubSurf plan, V4; design:
 * dyntopo-vdm-region-hybrid.md §2/§4/§5).
 *
 * The eligibility predicate is the offset-surface stability boundary: a VDM
 * face stays VDM-carried only while (1) its stored |D| bound respects the
 * fold bound `|D| ≤ α·ρ_min` of the base and (2) the displaced surface stays
 * a height field over the base (`angle(n_q, n) ≤ θ_max`). Faces that violate
 * either are promotion candidates.
 *
 * `promoteRegion` turns candidates into live geometry, one-way and local:
 * subdivide the faces (pattern subdivide, callbacks threaded so meshlog +
 * spatial stay current), seed every region vert to `base + frame·D(uv)` (the
 * displaced base position — boundary verts shared with a VDM neighbour land
 * exactly on the surface the neighbour still renders, so the seam is C0),
 * clear the promoted footprint's texels (they are geometry now; leaving them
 * would double-apply in the fragment path), and mark the resulting
 * carrier-boundary edges `EDGE_LAYER_REGION` so dyntopo never remeshes across
 * the parameterization seam. Children faces default to GEOM carrier (the
 * carrier attr is NOCOPY, so subdivision doesn't inherit it); the caller's
 * open MeshLog step plus the store's open delta make one undo press revert
 * topology, seeds, texels, AND carriers (VdmCarrierLogChunk).
 *
 * Promotion is one-way here: demotion (GEOM → VDM) is an explicit deferred
 * op (workstream X4). Hysteresis therefore reduces to the promote threshold
 * α_promote > the splatter's clamp α, so a clamped region doesn't oscillate
 * at the boundary.
 */

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"
#include "vdm_store.h"

#include <span>

namespace sculptcore::mesh {
struct Mesh;
struct MeshCallbacks;
} // namespace sculptcore::mesh
namespace sculptcore::spatial {
struct SpatialTree;
}
namespace sculptcore::meshlog {
struct MeshLog;
}

namespace sculptcore::vdm {

struct VdmPromoteParams {
  /* Fold-bound fraction: candidate when faceBound > alpha_promote·ρ_min.
   * Keep above the splat clamp α (hysteresis against boundary thrash).
   * <= 0 disables the fold test. */
  float alpha_promote = 0.6f;
  /* Overhang: candidate when angle(displaced normal, base normal) exceeds
   * this (degrees). <= 0 disables the overhang test. */
  float theta_max_deg = 60.0f;
  /* Pattern-subdivide cuts per edge (1 = quad→4). */
  int subdiv_cuts = 1;
  /* Test aid: treat every VDM face passed in as a candidate. */
  bool force = false;
};

struct VdmPromoteStats {
  int candidates = 0;
  int promoted = 0;    // original faces promoted (pre-subdivide count)
  int seededVerts = 0; // verts moved onto base + VDM
  int clearedTexels = 0;
  int regionEdges = 0; // edges marked EDGE_LAYER_REGION
};

/* Evaluate the eligibility predicate over `faces` (non-VDM faces skipped);
 * appends violating faces to `out`. Requires current F3 frames + normals. */
void collectPromotionCandidates(mesh::Mesh &m,
                                spatial::SpatialTree &tree,
                                VdmStore &store,
                                std::span<const int> faces,
                                const VdmPromoteParams &params,
                                util::Vector<int> &out);

/* Promote `faces` (must be VDM-carried). The caller holds an open MeshLog
 * step and an open store delta bracket; `cb` is the combined meshlog+spatial
 * callback bundle. Appends a VdmCarrierLogChunk to `log` for carrier undo. */
VdmPromoteStats promoteRegion(mesh::Mesh &m,
                              spatial::SpatialTree &tree,
                              VdmStore &store,
                              std::span<const int> faces,
                              const VdmPromoteParams &params,
                              mesh::MeshCallbacks *cb,
                              meshlog::MeshLog *log);

} // namespace sculptcore::vdm
