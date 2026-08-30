#pragma once

#include <webgpu/webgpu.h>

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

namespace sculptcore::webgpu {

struct WgpuContext;
struct WgpuTarget;

/* Same shape as vulkan::DrawUniforms — synthesized for callers that haven't
 * attached their own UniformBlockInstance to a DrawCommand/DrawBatch. */
struct DrawUniforms {
  litestl::math::mat4 drawMatrix;
  litestl::math::mat4 normalMatrix;
  litestl::math::float4 uColor{1.0f, 1.0f, 1.0f, 1.0f};
};

/** Walks gpu::GPUManager resources and records WebGPU draws into a render
 *  pass opened by beginFrame*()/endFrame*(). Mirrors vulkan::VulkanBackend.
 *
 *  Pipelines bake the color target format `colorFormat` handed in at
 *  construction (the swap-chain format for the surface path, RGBA8Unorm for
 *  offscreen). Build one backend per target format. */
struct WebGpuBackend : public sculptcore::gpu::GPUResourceObserver {
  WebGpuBackend(sculptcore::gpu::GPUManager *mgr,
                WgpuContext *ctx,
                WGPUTextureFormat colorFormat);
  WebGpuBackend(const WebGpuBackend &) = delete;
  ~WebGpuBackend() override;

  /** GPUResourceObserver: release the cached WGPUBuffer for `buf`. WebGPU
   *  ref-counts native handles, so no Vulkan-style deferred destroy is
   *  needed — the in-flight submit retains its own reference. */
  void onBufferDestroyed(sculptcore::gpu::Buffer *buf) override;

  /** Offscreen: open a command encoder + render pass on `target`. */
  bool beginFrame(WgpuTarget &target, float r, float g, float b, float a);
  void endFrame();

  /** Surface (swap-chain): acquire the current surface texture, ensure a
   *  matching depth texture, open a render pass. endFrameSurface() submits
   *  and presents. */
  bool beginFrameSurface(int w, int h, float r, float g, float b, float a);
  bool endFrameSurface();

  /** Issue every command in `batch` with `u` as fallback uniforms. Call
   *  between beginFrame*()/endFrame*(). */
  void draw(sculptcore::gpu::DrawBatch *batch, const DrawUniforms &u);

  /** Drop all cached WebGPU objects. */
  void invalidate();

  WgpuContext *context() const
  {
    return ctx_;
  }

private:
  struct BufferEntry {
    WGPUBuffer buffer = nullptr;
    uint64_t size = 0;
  };
  struct PipelineEntry {
    WGPUShaderModule shaderModule = nullptr;
    WGPUBindGroupLayout bgLayout = nullptr;
    WGPUPipelineLayout layout = nullptr;
    WGPURenderPipeline pipeline = nullptr;
    WGPUBuffer ubo = nullptr;
    WGPUBindGroup bindGroup = nullptr;
    uint64_t uboSize = 0;
  };

  BufferEntry &ensureBuffer(sculptcore::gpu::Buffer *buf);
  PipelineEntry *ensurePipeline(sculptcore::gpu::ShaderDef *def);
  void issue(sculptcore::gpu::DrawBatch *batch,
             sculptcore::gpu::DrawCommand *cmd,
             const DrawUniforms &u);

  void ensureSurfaceDepth(int w, int h);

  sculptcore::gpu::GPUManager *mgr_;
  WgpuContext *ctx_;
  WGPUTextureFormat colorFormat_;
  WGPUTextureFormat depthFormat_ = WGPUTextureFormat_Depth24Plus;

  litestl::util::Map<sculptcore::gpu::Buffer *, BufferEntry> buffer_cache_;
  litestl::util::Map<sculptcore::gpu::ShaderDef *, PipelineEntry> pipeline_cache_;

  /* Active-frame state, valid between beginFrame*() and endFrame*(). */
  WGPUCommandEncoder encoder_ = nullptr;
  WGPURenderPassEncoder pass_ = nullptr;
  bool inFrame_ = false;

  /* Surface path: per-frame acquired color view + the depth texture we own,
   * lazily (re)created to surface dimensions. */
  WGPUTexture surfaceTex_ = nullptr;
  WGPUTextureView surfaceView_ = nullptr;
  WGPUTexture depthTexture_ = nullptr;
  WGPUTextureView depthView_ = nullptr;
  int depthW_ = 0, depthH_ = 0;
};

} // namespace sculptcore::webgpu
