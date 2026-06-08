#pragma once

#include "gpu/manager.h"
#include "litestl/math/matrix.h"
#include "litestl/math/vector.h"

namespace sculptcore::vulkan {

struct VulkanBackend;

/** Line overlays drawn on top of the scene: world axes + brush-cursor
 *  ring. Each call allocates a transient line batch through the supplied
 *  GPUManager, records a draw via the supplied backend, then destroys the
 *  batch. This matches how `buildLeafBoundsBatch` is consumed today —
 *  per-frame allocation is fine for the debug app's hand-fed scenes. */
struct Overlay {
  Overlay() = default;
  Overlay(const Overlay &) = delete;
  ~Overlay() = default;

  void drawAxes(sculptcore::gpu::GPUManager &mgr,
                VulkanBackend &backend,
                const litestl::math::mat4 &drawMatrix,
                float scale = 1.0f);

  void drawBrushCursor(sculptcore::gpu::GPUManager &mgr,
                       VulkanBackend &backend,
                       const litestl::math::mat4 &drawMatrix,
                       litestl::math::float3 center,
                       litestl::math::float3 normal,
                       float radius,
                       litestl::math::float4 color = {1.0f, 1.0f, 0.0f, 1.0f});

  /** Draw a caller-supplied world-space line list (vertCount = 2× segments,
   *  one color per vertex). Rounded down to an even count. Goes through the
   *  same depth-tested line pipeline as the axes, so segments behind nearer
   *  geometry are occluded. Colors are opaque (the pipeline has no blending). */
  void drawLines(sculptcore::gpu::GPUManager &mgr,
                 VulkanBackend &backend,
                 const litestl::math::mat4 &drawMatrix,
                 const litestl::math::float3 *positions,
                 const litestl::math::float4 *colors,
                 int vertCount);
};

} // namespace sculptcore::vulkan
