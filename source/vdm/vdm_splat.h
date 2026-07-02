#pragma once

/** VDM brush splatter (displacementAndSubSurf plan, V2): rasterize a dab's
 * footprint into the store's UV tiles — the pbvhTexPaint pattern with a
 * float3 payload and world-space falloff.
 *
 * Per dab: the spatial tree answers "which faces are under the brush"
 * (filterNodes + the `.detail.carrier == VDM` gate), then each face's UV
 * triangles are rasterized at store resolution. Per texel: the base position
 * and the F3 frame (smoothed normal + cross-field tangent, displace/frames.h)
 * are barycentrically interpolated, falloff is evaluated in world space from
 * `base + frame·texel` (the *displaced* point), and the world-space
 * displacement is inverted through the frame into tangent texels. The total
 * texel magnitude is clamped to `alpha · ρ_min` of the base surface (the
 * offset-surface fold bound; ρ_min from the 1-ring shape operator, min over
 * the triangle's verts). Clamp counts feed V4's promotion predicate.
 *
 * The caller owns the undo bracket: store.beginDelta() → splatDab(...) →
 * store.endDelta() → MeshLog::appendChunk(VdmLogChunk) inside the dab's step.
 * splatDab refreshes the touched faces' `.detail.bound` pads through
 * SpatialTree::setFaceDisplacementBounds (bounds-only dirty).
 *
 * UV-seam skirts (one-texel copies for seamless bilinear reads) are not yet
 * implemented — V3's render path lands them together with the GPU upload.
 */

#include "litestl/math/vector.h"
#include "vdm_store.h"

namespace sculptcore::mesh {
struct Mesh;
}
namespace sculptcore::spatial {
struct SpatialTree;
}

namespace sculptcore::vdm {

struct VdmSplatParams {
  float3 center{0.0f, 0.0f, 0.0f}; // world dab center
  float3 normal{0.0f, 0.0f, 1.0f}; // dab direction (world displacement axis)
  float radius = 0.1f;
  float strength = 0.5f;
  bool invert = false;
  /* Fold-bound clamp fraction α: |texel| ≤ α·ρ_min. <= 0 disables. */
  float alpha = 0.5f;
};

struct VdmSplatStats {
  int facesTouched = 0;
  int texelsTouched = 0;
  int texelsClamped = 0;
};

/* Requires current vertex normals and F3 frames over the dab region
 * (displace::updateFramesRegion/All — VDM regions are static, so frames are
 * computed once per region, not per dab). */
VdmSplatStats splatDab(mesh::Mesh &m,
                       spatial::SpatialTree &tree,
                       VdmStore &store,
                       const VdmSplatParams &params);

} // namespace sculptcore::vdm
