#pragma once

#include <vulkan/vulkan.h>

#include "gpu/manager.h"
#include "gpu/types.h"

#include "litestl/math/matrix.h"
#include "litestl/math/vector.h"
#include "litestl/util/map.h"

namespace sculptcore::gpu {
struct Buffer;
struct DrawBatch;
struct DrawCommand;
struct ShaderDef;
} // namespace sculptcore::gpu

namespace sculptcore::vulkan {

struct VkContext;
struct OffscreenTarget;

struct DrawUniforms {
  litestl::math::mat4 drawMatrix;
  litestl::math::mat4 normalMatrix;
  litestl::math::float4 uColor{1.0f, 1.0f, 1.0f, 1.0f};
};

/** Walks gpu::GPUManager resources and records Vulkan draws into a command
 *  buffer the caller supplies via beginFrame()/endFrame(). Mirrors GLBackend
 *  in role.
 *
 *  Lifetime: build the backend after VkContext::init() and after the
 *  OffscreenTarget render pass is created — the pipelines bake in the render
 *  pass handle. Call invalidate() before tearing down VkContext. */
struct VulkanBackend {
  VulkanBackend(sculptcore::gpu::GPUManager *mgr, VkContext *ctx, VkRenderPass renderPass);
  VulkanBackend(const VulkanBackend &) = delete;
  ~VulkanBackend();

  /** Open a primary command buffer, begin it, and begin the supplied render
   *  pass on `target`. Pair every beginFrame() with one endFrame(). */
  bool beginFrame(OffscreenTarget &target, float r, float g, float b, float a);

  /** Issue every command in `batch` with `u` as uniforms. Must be called
   *  between beginFrame() / endFrame(). */
  void draw(sculptcore::gpu::DrawBatch *batch, const DrawUniforms &u);

  /** End the render pass, close the command buffer, submit, and wait. */
  void endFrame();

  /** Drop all cached Vulkan objects. Call before destroying VkContext. */
  void invalidate();

  VkContext *context() const { return ctx_; }

private:
  struct BufferEntry {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    bool hostVisible = false;
  };
  struct PipelineEntry {
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    /* Per-pipeline uniform buffer + descriptor set. Single-draw debug app so
     * no need to multi-buffer; we map once and memcpy per issue(). */
    VkBuffer uboBuffer = VK_NULL_HANDLE;
    VkDeviceMemory uboMemory = VK_NULL_HANDLE;
    void *uboMapped = nullptr;
    VkDeviceSize uboSize = 0;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    /* Cached vertex stride/format per attribute, in declaration order. */
  };

  BufferEntry &ensureBuffer(sculptcore::gpu::Buffer *buf);
  PipelineEntry *ensurePipeline(sculptcore::gpu::ShaderDef *def);
  void issue(sculptcore::gpu::DrawCommand *cmd, const DrawUniforms &u);

  sculptcore::gpu::GPUManager *mgr_;
  VkContext *ctx_;
  VkRenderPass renderPass_;

  litestl::util::Map<sculptcore::gpu::Buffer *, BufferEntry> buffer_cache_;
  litestl::util::Map<sculptcore::gpu::ShaderDef *, PipelineEntry> pipeline_cache_;

  VkCommandBuffer activeCb_ = VK_NULL_HANDLE;
  bool inFrame_ = false;
};

} // namespace sculptcore::vulkan
