#pragma once

/** Frame provider (displacementAndSubSurf plan, workstream F3).
 *
 * One module owns the smooth tangent frame — a smoothed per-vertex normal plus
 * a cross-field tangent (4-RoSy representative) — computed on demand for a
 * static vertex set and stored as persistent per-vertex attributes. It is the
 * single synchronization anchor: the VDM brush splatter (inverting world
 * edits into tangent texels), promotion bakes, the fragment shader, and the
 * stencil-amplification pass must all read this one field — never derive a
 * frame two different ways on two sides of a bake
 * (final-displacement-architecture.md §7).
 *
 * Determinism contract: computation is serial Gauss-Seidel in the caller's
 * vertex order with no unordered float reductions, so identical inputs give
 * bit-identical frames on the WASM and native backends (the same contract as
 * brush/feature_field.cc, whose seeding/diffusion this mirrors — this is the
 * cheap heat-diffusion variant; a KCPS eigensolve can swap in behind the same
 * API if seam artifacts demand it). Bitangent = n × t, derived at read sites.
 */

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"

namespace sculptcore::displace {
using litestl::math::float3;
namespace util = litestl::util;

/* Persistent NOINTERP per-vertex frame attributes: the smoothed unit normal
 * and the unit cross-field tangent (⊥ to it). NOINTERP because averaging two
 * cross directions across a split can cancel; frames live on static bases and
 * are recomputed per region instead. */
constexpr const char *FRAME_NORMAL_ATTR = ".frames.v.normal";
constexpr const char *FRAME_TANGENT_ATTR = ".frames.v.tangent";

struct FrameProviderParams {
  /* Gauss-Seidel rounds averaging 1-ring normals into the smoothed normal. */
  int normal_smooth_iters = 2;
  /* Gauss-Seidel rounds of 4-fold-aware tangent diffusion. */
  int diffuse_iters = 8;
  /* Hard-pin tangents along boundary feature edges (sharp/seam/border/group). */
  bool use_features = true;
  /* Soft-seed unset tangents from the 1-ring shape operator's principal dir. */
  bool use_curvature = true;
};

/* Ensure both frame attributes exist (persistent, NOINTERP). */
void ensureFrameAttrs(mesh::Mesh &m);

/* Compute frames for `verts`. The caller's order is the determinism anchor —
 * pass a stable order (spatial-node unique_verts in node order, or vert-id
 * order). Vertex normals (m.v.no) must be current. */
void updateFramesRegion(mesh::Mesh &m,
                        const util::Vector<int> &verts,
                        const FrameProviderParams &params);

/* Whole-mesh convenience: every live vert in vert-id order. */
void updateFramesAll(mesh::Mesh &m, const FrameProviderParams &params);

/* Sum of the tangent field's per-face winding indices in quarter-turn units
 * (each face: the accumulated nearest-90°-image rotation of the corner
 * tangents projected into the face plane). Poincaré–Hopf: == 4·χ on a closed
 * mesh (8 on a sphere/cube). Validation/test helper. */
int crossFieldIndexSum(mesh::Mesh &m);

} // namespace sculptcore::displace
