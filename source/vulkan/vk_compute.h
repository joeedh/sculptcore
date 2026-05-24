#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace sculptcore::vulkan {

struct VkContext;

/* Host mirrors of the WGSL uniform/storage structs emit_wgsl.cc produces.
 * Field offsets MUST match the shader's std140 (uniform) / std430 (storage)
 * layout — see source/brush/compiler/emit_wgsl.cc. Explicit padding makes the
 * layout independent of the C++ ABI. */

/* binding 5 — std140, size 64. */
struct ComputeBrushUniforms {
  float strength = 0.0f;
  float radius = 1.0f;
  float spacing = 0.25f;
  uint32_t invert = 0;
  uint32_t falloff_kind = 0;
  uint32_t falloff_shape = 0;
  uint32_t _pad0[2] = {0, 0};        // pad so falloff_dir (vec3) lands at 32
  float falloff_dir[3] = {0, 0, 1};  // offset 32
  uint32_t coord_space = 0;          // offset 44
  float tex_repeat = 1.0f;           // offset 48
  uint32_t stroke_path_count = 0;    // offset 52
  uint32_t _pad1[2] = {0, 0};        // round struct to 64
};

/* binding 6 — std140, size 96. */
struct ComputeCtxUniforms {
  float surfacePos[3] = {0, 0, 0};
  uint32_t _pad0 = 0;
  float surfaceNo[3] = {0, 0, 1};
  uint32_t _pad1 = 0;
  float render_matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

/* binding 10 element — std430, stride 48. */
struct ComputeStrokeSample {
  float pos[3] = {0, 0, 0};
  uint32_t _pad0 = 0;
  float normal[3] = {0, 0, 1};
  uint32_t _pad1 = 0;
  float arclen = 0.0f;
  uint32_t _pad2[3] = {0, 0, 0};
};

/* One spatial-node chunk: a (offset,count) window into the flattened
 * unique_verts array. count must be <= 64 (the kernel's workgroup size);
 * the host splits larger nodes into multiple chunks. */
struct ComputeNodeMeta {
  uint32_t vert_offset = 0;
  uint32_t vert_count = 0;
};

/* Runs sbrush WGSL->SPIR-V compute kernels against a mesh's vertex buffers.
 * Self-contained: owns its descriptor pool, pipeline, buffers, and a 1x1
 * white texture/sampler for the (unused-by-DRAW) brush-texture bindings. The
 * device/queue/command-pool come from VkContext. One dispatcher per stroke is
 * fine for the debug app; co/no/mask persist across dabs so each dab reads the
 * previous dab's result (matching the C++ executor). */
struct BrushComputeDispatch {
  explicit BrushComputeDispatch(VkContext *ctx) : ctx_(ctx) {}
  BrushComputeDispatch(const BrushComputeDispatch &) = delete;
  ~BrushComputeDispatch();

  /* Load the kernel and build the pipeline + descriptor layout. Idempotent
   * per instance; call once. */
  bool loadSpirv(const char *path);

  /* Upload full-mesh vertex arrays (one entry per global vertex). `co`/`no`
   * are tightly packed xyz triples; mask is one float per vertex. */
  bool beginStroke(const float *co, const float *no, const float *mask,
                   int vertCount);

  /* Dispatch one brush dab. `uniqueVerts` are global vertex indices (flattened
   * across this dab's nodes); `nodes` index into it with count<=64 each, one
   * workgroup per node. `falloffLut` is 256 floats. `strokePath` length must
   * equal brushU.stroke_path_count (may be 0). */
  bool dab(const ComputeBrushUniforms &brushU, const ComputeCtxUniforms &ctxU,
           const uint32_t *uniqueVerts, int uniqueVertCount,
           const ComputeNodeMeta *nodes, int nodeCount, const float *falloffLut,
           const ComputeStrokeSample *strokePath, int strokeCount);

  /* Read co/no/mask back into caller arrays (packed xyz / xyz / f32). Any
   * pointer may be null to skip that readback. */
  bool endStroke(float *coOut, float *noOut, float *maskOut);

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

  VkContext *ctx_ = nullptr;

  VkShaderModule module_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet set_ = VK_NULL_HANDLE;

  /* binding 8/9 — bound once, never used by DRAW but the shader declares them. */
  VkImage whiteImage_ = VK_NULL_HANDLE;
  VkDeviceMemory whiteMem_ = VK_NULL_HANDLE;
  VkImageView whiteView_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;

  int vertCount_ = 0;
  Buf co_, no_, mask_;             // bindings 0,1,2 (persistent per stroke)
  Buf unique_, nodes_;             // bindings 3,4 (per dab)
  Buf brushU_, ctxU_;             // bindings 5,6
  Buf falloff_, stroke_;          // bindings 7,10
};

} // namespace sculptcore::vulkan
