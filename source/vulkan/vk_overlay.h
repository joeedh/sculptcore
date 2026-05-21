#pragma once

#include "litestl/math/matrix.h"
#include "litestl/math/vector.h"

namespace sculptcore::vulkan {

struct VkContext;
struct VulkanBackend;

/** Placeholder counterpart to opengl::Overlay. drawAxes / drawBrushCursor
 *  are no-ops for now; the Vulkan port deferred them until the basic
 *  scene-render path is verified. Re-implement on top of basicLineShader
 *  with a small dynamic vertex buffer once the rest of the renderer works. */
struct Overlay {
  Overlay() = default;
  Overlay(const Overlay &) = delete;
  ~Overlay() { release(); }

  bool ensure(VkContext * /*ctx*/) { return true; }
  void release() {}

  void drawAxes(const litestl::math::mat4 & /*drawMatrix*/, float /*scale*/ = 1.0f) {}
  void drawBrushCursor(const litestl::math::mat4 & /*drawMatrix*/,
                       litestl::math::float3 /*center*/,
                       litestl::math::float3 /*normal*/,
                       float /*radius*/,
                       litestl::math::float4 /*color*/ = {1.0f, 1.0f, 0.0f, 1.0f}) {}
};

} // namespace sculptcore::vulkan
