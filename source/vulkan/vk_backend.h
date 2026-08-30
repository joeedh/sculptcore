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
struct Swapchain;

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
struct VulkanBackend : public sculptcore::gpu::GPUResourceObserver {
  VulkanBackend(sculptcore::gpu::GPUManager *mgr,
                VkContext *ctx,
                VkRenderPass renderPass);
  VulkanBackend(const VulkanBackend &) = delete;
  ~VulkanBackend() override;

  /** GPUResourceObserver: invoked from GPUManager::destroyBuffer just before
   *  the gpu::Buffer is freed. Erases the cache entry keyed by `buf` and
   *  pushes its VkBuffer / VkDeviceMemory onto the deferred-destroy list so
   *  they outlive any in-flight or currently-recording command buffer that
   *  still references them. */
  void onBufferDestroyed(sculptcore::gpu::Buffer *buf) override;

  /** Open a primary command buffer, begin it, and begin the supplied render
   *  pass on `target`. Pair every beginFrame() with one endFrame(). */
  bool beginFrame(OffscreenTarget &target, float r, float g, float b, float a);

  /** Swapchain variant: allocates the command buffer, begins it, and
   *  begins the swapchain render pass on `imageIndex`. Pair with
   *  endFrameSwapchain(). */
  bool beginFrameSwapchain(
      Swapchain &sw, uint32_t imageIndex, float r, float g, float b, float a);

  /** Issue every command in `batch` with `u` as uniforms. Must be called
   *  between beginFrame() / endFrame(). */
  void draw(sculptcore::gpu::DrawBatch *batch, const DrawUniforms &u);

  /** End the render pass, close the command buffer, submit, and wait. */
  void endFrame();

  /** Swapchain variant: closes the command buffer and submits via
   *  Swapchain::submitAndPresent (with image-acquire / render-complete
   *  semaphores + in-flight fence). Returns false if present reported
   *  out-of-date — caller should recreate the swapchain. */
  bool endFrameSwapchain(Swapchain &sw, uint32_t imageIndex);

  /** Command buffer currently being recorded, between beginFrame*() and
   *  endFrame*(). Returns VK_NULL_HANDLE when not in a frame. */
  VkCommandBuffer activeCommandBuffer() const
  {
    return activeCb_;
  }

  /** Drop all cached Vulkan objects. Call before destroying VkContext. */
  void invalidate();

  VkContext *context() const
  {
    return ctx_;
  }

  /** Force the VkBuffer backing `buf` to exist (creating/growing it) with
   *  STORAGE usage so a compute pass can write it, and return the handle.
   *  Used by the GPU-resident stroke path to scatter into render VBOs outside
   *  the draw loop. Sets buf->gpu_storage. */
  VkBuffer ensureStorageVkBuffer(sculptcore::gpu::Buffer *buf);

private:
  struct BufferEntry {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    bool hostVisible = false;
    bool storage = false; /* created with STORAGE usage (gpu_storage) */
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
  void issue(sculptcore::gpu::DrawBatch *batch,
             sculptcore::gpu::DrawCommand *cmd,
             const DrawUniforms &u);

  sculptcore::gpu::GPUManager *mgr_;
  VkContext *ctx_;
  VkRenderPass renderPass_;

  litestl::util::Map<sculptcore::gpu::Buffer *, BufferEntry> buffer_cache_;
  litestl::util::Map<sculptcore::gpu::ShaderDef *, PipelineEntry> pipeline_cache_;

  /** VkBuffer / VkDeviceMemory pairs released by onBufferDestroyed() or by
   *  the grow path in ensureBuffer(). They may still be bound to a recording
   *  or in-flight command buffer, so destruction is deferred until the
   *  next frame boundary (after vkQueueWaitIdle in endFrame*) or invalidate(). */
  struct PendingBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
  };
  litestl::util::Vector<PendingBuffer> deferred_buffers_;
  void drainDeferredBuffers_();

  VkCommandBuffer activeCb_ = VK_NULL_HANDLE;
  bool inFrame_ = false;
};

} // namespace sculptcore::vulkan
