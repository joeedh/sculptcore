#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

#include "brush/compute_dispatch.h"
#include "brush/compute_layout.h"

namespace sculptcore::vulkan {

struct VkContext;

/* The host-mirror compute structs now live in brush/compute_layout.h (one
 * source of truth shared with wgpu_compute). Alias them in so existing
 * vulkan:: call sites are unchanged. */
using brush::ComputeBrushUniforms;
using brush::ComputeCtxUniforms;
using brush::ComputeNodeMeta;
using brush::ComputeStrokeSample;
using brush::ComputeVertNbr;
using brush::kAutomaskBinding;
using brush::kDabStampBinding;
using brush::kDispBinding;

/* Runs sbrush WGSL->SPIR-V compute kernels against a mesh's vertex buffers.
 * Self-contained: owns its descriptor pool, pipeline, buffers, and a 1x1
 * white texture/sampler for the (unused-by-DRAW) brush-texture bindings. The
 * device/queue/command-pool come from VkContext. One dispatcher per stroke is
 * fine for the debug app; co/no/mask persist across dabs so each dab reads the
 * previous dab's result (matching the C++ executor). */
struct BrushComputeDispatch : brush::IBrushComputeDispatch {
  explicit BrushComputeDispatch(VkContext *ctx) : ctx_(ctx)
  {
  }
  BrushComputeDispatch(const BrushComputeDispatch &) = delete;
  ~BrushComputeDispatch() override;

  /* Load the SPIR-V kernel and build the pipeline + descriptor layout.
   * Idempotent per instance; call once. */
  bool loadKernel(const char *path) override;

  /* Upload full-mesh vertex arrays (one entry per global vertex). `co`/`no`
   * are tightly packed xyz triples; mask is one float per vertex. */
  bool beginStroke(const float *co,
                   const float *no,
                   const float *mask,
                   int vertCount) override;

  /* Dispatch one brush dab. `uniqueVerts` are global vertex indices (flattened
   * across this dab's nodes); `nodes` index into it with count<=64 each, one
   * workgroup per node. `falloffLut` is 256 floats. `strokePath` length must
   * equal brushU.stroke_path_count (may be 0). */
  bool dab(const ComputeBrushUniforms &brushU,
           const ComputeCtxUniforms &ctxU,
           const uint32_t *uniqueVerts,
           int uniqueVertCount,
           const ComputeNodeMeta *nodes,
           int nodeCount,
           const float *falloffLut,
           const ComputeStrokeSample *strokePath,
           int strokeCount) override;

  /* dab() split into a CPU prepare + a record-into-cb half, so the interactive
   * path can batch a dab and the dependent normal recompute into one submit.
   * prepareDab() marshals + binds and stashes the workgroup count; recordDab()
   * records the dispatch into a caller-supplied command buffer. Call
   * prepareDab() then recordDab(cb) inside the shared command buffer. */
  bool prepareDab(const ComputeBrushUniforms &brushU,
                  const ComputeCtxUniforms &ctxU,
                  const uint32_t *uniqueVerts,
                  int uniqueVertCount,
                  const ComputeNodeMeta *nodes,
                  int nodeCount,
                  const float *falloffLut,
                  const ComputeStrokeSample *strokePath,
                  int strokeCount);
  void recordDab(VkCommandBuffer cb);

  /* Upload the CSR neighbor topology (binding 12/13) for for_neighbor kernels
   * like Smooth. `meta` has one entry per global vertex; `nbrVerts` is the
   * flat neighbor-index array. Static across a stroke — call once after
   * beginStroke. Untextured non-neighbor kernels (Draw/Clay) never call this;
   * their bindings stay bound to dummies. */
  bool setNeighbors(const ComputeVertNbr *meta,
                    int vertCount,
                    const uint32_t *nbrVerts,
                    int nbrCount) override;

  bool setAutomask(const float *automask, int vertCount) override;

  /* Upload a grayscale brush texture (row-major, w*h floats) and rebind it to
   * binding 8, replacing the 1x1 white dummy. The WGSL kernel does its own
   * clamp-to-edge bilinear via textureLoad, so the image is R32_SFLOAT with no
   * filtering — bit-modulo-fp identical to Brush::sampleTexBilinear. Call once
   * per stroke after beginStroke when a texture is bound; untextured strokes
   * skip it and keep the white dummy (sampleBrushTex then returns 1.0). */
  bool setBrushTexture(const float *pixels, int width, int height) override;

  /* Read co/no/mask back into caller arrays (packed xyz / xyz / f32). Any
   * pointer may be null to skip that readback. */
  bool endStroke(float *coOut, float *noOut, float *maskOut) override;

  /* Read back just the listed global vertex indices (packed xyz). Used by the
   * GPU-resident path to refresh the CPU copy of the verts a dab moved (for
   * ray-pick + node bounds) without a full-mesh readback. coOut/noOut are
   * caller arrays of `count*3` floats; either may be null. */
  bool
  readbackVerts(const uint32_t *verts, int count, float *coOut, float *noOut) override;

  /* Custom attribute layers (binding >=14): upload once after beginStroke,
   * read back at end. byteSize bytes in the kernel's GPU layout. */
  bool setAttr(uint32_t slot, const void *data, size_t byteSize) override;
  bool readbackAttr(uint32_t slot, void *out, size_t byteSize) override;

  /* GPU-resident stroke path (debug app): the persistent stride-16 co/no
   * STORAGE buffers and the vertex count, so a sibling GpuNormalPass can
   * recompute normals and scatter into render VBOs with no CPU roundtrip.
   * Valid between beginStroke() and the dispatcher's destruction. */
  VkBuffer coBuffer() const
  {
    return co_.buffer;
  }
  VkBuffer noBuffer() const
  {
    return no_.buffer;
  }
  int vertCount() const
  {
    return vertCount_;
  }

private:
  struct Buf {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr;
  };

  bool createBuf(Buf &b, VkDeviceSize size, VkBufferUsageFlags usage);
  void destroyBuf(Buf &b);
  /* Grow `b` to at least `size` (rounded up) if needed, keeping it mapped. */
  bool ensureBuf(Buf &b, VkDeviceSize size, VkBufferUsageFlags usage);

  void writeStorage(uint32_t binding, const Buf &b);
  void writeUniform(uint32_t binding, const Buf &b);

  bool createWhiteTexture();
  void destroyBrushTexture();

  VkContext *ctx_ = nullptr;

  VkShaderModule module_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet set_ = VK_NULL_HANDLE;

  /* binding 8/9 — the white dummy is bound at load; setBrushTexture swaps a
   * real R32_SFLOAT image into binding 8. sampler_ (binding 9) is declared by
   * every kernel but unused (the kernel filters in-shader via textureLoad). */
  VkImage whiteImage_ = VK_NULL_HANDLE;
  VkDeviceMemory whiteMem_ = VK_NULL_HANDLE;
  VkImageView whiteView_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;

  /* binding 8 real texture (set by setBrushTexture, replacing whiteView_). */
  VkImage texImage_ = VK_NULL_HANDLE;
  VkDeviceMemory texMem_ = VK_NULL_HANDLE;
  VkImageView texView_ = VK_NULL_HANDLE;

  int vertCount_ = 0;
  int dabNodeCount_ = 0; // workgroup count stashed by prepareDab for recordDab
  bool hasNeighbors_ = false;
  Buf co_, no_, mask_;              // bindings 0,1,2 (persistent per stroke)
  Buf unique_, nodes_;              // bindings 3,4 (per dab)
  Buf brushU_, ctxU_;               // bindings 5,6
  Buf falloff_, stroke_;            // bindings 7,10
  Buf coPrev_, nbrMeta_, nbrVerts_; // bindings 11,12,13 (neighbor kernels)
  /* binding 23 (kDabStampBinding) — grab-class per-vertex first-touch stamps
   * (@grabmode kernels), zero-filled at beginStroke. */
  Buf dabStamp_;
  /* binding 24 (kAutomaskBinding) — read-only per-vertex cavity automask factor.
   * beginStroke fills identity 1.0; setAutomask overrides with real factors. */
  Buf automask_;
  /* binding 25 (kDispBinding) — read_write accumulated brush displacement for
   * non-accumulate mode, zero-filled at beginStroke; the base is `co - disp`. */
  Buf disp_;

  /* Custom DSL attribute layers. The descriptor layout always declares a
   * superset of attr slots (kAttrBase..kAttrBase+kMaxAttrBindings-1) so one
   * pipeline layout serves attr and non-attr kernels; unused slots bind to
   * attrDummy_. setAttr fills the used ones, persistent across dabs. */
  static constexpr int kAttrBase = 14;
  static constexpr int kMaxAttrBindings = 8;
  Buf attrDummy_;
  Buf attrBuf_[kMaxAttrBindings];
};

} // namespace sculptcore::vulkan
