#pragma once

#include <webgpu/webgpu.h>

#include <cstdint>

#include "brush/compute_dispatch.h"
#include "brush/compute_layout.h"

#include "litestl/util/vector.h"

namespace sculptcore::webgpu {

struct WgpuContext;

/* WebGPU analogue of vulkan::BrushComputeDispatch: runs the sbrush WGSL compute
 * kernels against a mesh's vertex buffers through webgpu.h (wgpu-native natively,
 * emdawnwebgpu under WASM — only the native path is built/verified for now). It
 * is the batch/readback backend: upload the mesh once, dispatch one dab at a time
 * (each reads the previous dab's result), read co/no/mask back at stroke end.
 *
 * Unlike Vulkan it cannot share buffers with a separate-API renderer, so the
 * GPU-resident live-scatter extras (prepareDab/recordDab/coBuffer) have no
 * WebGPU analogue here — the IBrushComputeDispatch surface is the whole API. */
struct WgpuBrushComputeDispatch : brush::IBrushComputeDispatch {
  explicit WgpuBrushComputeDispatch(WgpuContext *ctx) : ctx_(ctx) {}
  WgpuBrushComputeDispatch(const WgpuBrushComputeDispatch &) = delete;
  ~WgpuBrushComputeDispatch() override;

  /* Read the .wgsl text, create the shader module, parse its actual binding
   * table (kinds + access qualifiers) and build a matching bind-group layout +
   * compute pipeline (entry point `main`). Idempotent per instance. */
  bool loadKernel(const char *path) override;

  bool beginStroke(const float *co, const float *no, const float *mask,
                   int vertCount) override;

  bool dab(const brush::ComputeBrushUniforms &brushU,
           const brush::ComputeCtxUniforms &ctxU, const uint32_t *uniqueVerts,
           int uniqueVertCount, const brush::ComputeNodeMeta *nodes,
           int nodeCount, const float *falloffLut,
           const brush::ComputeStrokeSample *strokePath, int strokeCount) override;

  bool setNeighbors(const brush::ComputeVertNbr *meta, int vertCount,
                    const uint32_t *nbrVerts, int nbrCount) override;

  bool setAutomask(const float *automask, int vertCount) override;

  bool setBrushTexture(const float *pixels, int width, int height) override;

  bool endStroke(float *coOut, float *noOut, float *maskOut) override;

  bool readbackVerts(const uint32_t *verts, int count, float *coOut,
                     float *noOut) override;

private:
  /* Mirrors parseBindings/layoutEntry in tests/webgpu/replay.mjs. */
  enum class BindKind { Uniform, StorageRW, StorageRO, Texture, Sampler };
  struct BindingInfo {
    uint32_t binding = 0;
    BindKind kind = BindKind::StorageRW;
  };

  struct Buf {
    WGPUBuffer buffer = nullptr;
    uint64_t size = 0;
  };

  /* Grow `b` to at least `size` (rounded up) if needed; `usage` is the WebGPU
   * usage flags the buffer must carry. */
  bool ensureBuf(Buf &b, uint64_t size, WGPUBufferUsage usage);
  void destroyBuf(Buf &b);

  /* (Re)build the per-dab bind group from the current buffer/texture handles,
   * including only the bindings the loaded kernel actually declares. */
  WGPUBindGroup buildBindGroup();

  bool createWhiteTexture();
  void destroyBrushTexture();

  /* copyBufferToBuffer src → a MAP_READ staging buffer, drain, map, and unpad
   * stride-16 vec3 slots into packed xyz (vec3=true) or copy f32 (vec3=false).
   * `n` is the element count. */
  bool readbackBuffer(const Buf &src, int n, bool vec3, float *out);
  bool hasBinding(uint32_t b) const;

  WgpuContext *ctx_ = nullptr;

  WGPUShaderModule module_ = nullptr;
  WGPUBindGroupLayout bgLayout_ = nullptr;
  WGPUPipelineLayout pipeLayout_ = nullptr;
  WGPUComputePipeline pipeline_ = nullptr;
  litestl::util::Vector<BindingInfo> bindings_;

  /* binding 8/9 — 1x1 white dummy at load; setBrushTexture swaps a real
   * R32Float image into texView_. sampler_ (binding 9) is declared by every
   * kernel but unused (the kernel filters in-shader via textureLoad). */
  WGPUTexture whiteTexture_ = nullptr;
  WGPUTextureView whiteView_ = nullptr;
  WGPUSampler sampler_ = nullptr;
  WGPUTexture texTexture_ = nullptr;
  WGPUTextureView texView_ = nullptr;  // bound at binding 8 (white or real)

  int vertCount_ = 0;
  bool hasNeighbors_ = false;
  Buf co_, no_, mask_;               // bindings 0,1,2
  Buf unique_, nodes_;               // bindings 3,4
  Buf brushU_, ctxU_;                // bindings 5,6
  Buf falloff_, stroke_;             // bindings 7,10
  Buf coPrev_, nbrMeta_, nbrVerts_;  // bindings 11,12,13
  /* binding 22 (kOrigCoBinding) — read-only stroke-start co for non-accumulate
   * mode; a copy of the beginStroke co upload, static across the stroke. */
  Buf origCo_;
  /* binding 23 (kDabStampBinding) — grab-class per-vertex first-touch stamps
   * (@grabmode kernels), zero-filled at beginStroke. */
  Buf dabStamp_;
  /* binding 24 (kAutomaskBinding) — read-only per-vertex cavity automask factor.
   * beginStroke fills identity 1.0; setAutomask overrides with real factors. */
  Buf automask_;

  /* Persistent MAP_READ staging buffer reused across every readback. Allocating
   * a fresh host-visible buffer per dab churns vkAllocateMemory/vkFreeMemory on
   * the wgpu-native device, which periodically stalls the shared GPU and hitches
   * the Vulkan renderer's present (same class as the Vulkan uncached-readback
   * fix). Grown on demand, never freed mid-session. */
  Buf readback_;
};

} // namespace sculptcore::webgpu
