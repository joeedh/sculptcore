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
  float mu = 1.0f;                   // offset 56 — kelvinlet (else unused)
  float nu = 0.4f;                   // offset 60 — kelvinlet; rounds struct to 64
};

/* binding 6 — std140. Base block (surfacePos/surfaceNo/render_matrix) is 96
 * bytes; the global-brush tail starts at offset 96. Kelvinlet's grab vectors
 * and pose's cage arrays both begin there in their respective kernels'
 * CtxUniforms, so they alias in a union — only one kernel's view is live per
 * dispatch (size = 96 + 128 = 224). */
struct ComputeCtxUniforms {
  float surfacePos[3] = {0, 0, 0};
  uint32_t _pad0 = 0;
  float surfaceNo[3] = {0, 0, 1};
  uint32_t _pad1 = 0;
  float render_matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  union {
    struct {                              // kelvinlet.wgsl CtxUniforms tail
      float grabFrom[3]; uint32_t _kpad0;  // offset 96
      float grabTo[3];   uint32_t _kpad1;  // offset 112
    } kelvinlet;
    struct {                    // pose.wgsl tail — std140 array<vec3> stride 16
      float poseCageRest[4][4];  // offset 96  ([i][0..2]=xyz, [i][3]=pad)
      float poseCageNow[4][4];   // offset 160
    } pose;
  } global = {};
};

/* binding 10 element — std430, matching WGSL `struct StrokeSample`. A
 * vec3<f32> has 16-byte alignment but only 12-byte size, so `arclen` packs
 * into the tail of `normal`'s 16-byte slot at offset 28, giving stride 32 (NOT
 * 48 — over-padding here makes the shader read arclen out of the wrong slot). */
struct ComputeStrokeSample {
  float pos[3] = {0, 0, 0};     // offset 0
  uint32_t _pad0 = 0;           // pad to normal's 16-byte alignment
  float normal[3] = {0, 0, 1};  // offset 16
  float arclen = 0.0f;          // offset 28
};

/* One spatial-node chunk: a (offset,count) window into the flattened
 * unique_verts array. count must be <= 64 (the kernel's workgroup size);
 * the host splits larger nodes into multiple chunks. */
struct ComputeNodeMeta {
  uint32_t vert_offset = 0;
  uint32_t vert_count = 0;
};

/* binding 12 element — std430 vec2<u32>, stride 8. CSR neighbor index: for
 * global vertex i, its neighbors are nbr_verts[offset .. offset+count). */
struct ComputeVertNbr {
  uint32_t offset = 0;
  uint32_t count = 0;
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

  /* Upload the CSR neighbor topology (binding 12/13) for for_neighbor kernels
   * like Smooth. `meta` has one entry per global vertex; `nbrVerts` is the
   * flat neighbor-index array. Static across a stroke — call once after
   * beginStroke. Untextured non-neighbor kernels (Draw/Clay) never call this;
   * their bindings stay bound to dummies. */
  bool setNeighbors(const ComputeVertNbr *meta, int vertCount,
                    const uint32_t *nbrVerts, int nbrCount);

  /* Upload a grayscale brush texture (row-major, w*h floats) and rebind it to
   * binding 8, replacing the 1x1 white dummy. The WGSL kernel does its own
   * clamp-to-edge bilinear via textureLoad, so the image is R32_SFLOAT with no
   * filtering — bit-modulo-fp identical to Brush::sampleTexBilinear. Call once
   * per stroke after beginStroke when a texture is bound; untextured strokes
   * skip it and keep the white dummy (sampleBrushTex then returns 1.0). */
  bool setBrushTexture(const float *pixels, int width, int height);

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
  bool hasNeighbors_ = false;
  Buf co_, no_, mask_;             // bindings 0,1,2 (persistent per stroke)
  Buf unique_, nodes_;             // bindings 3,4 (per dab)
  Buf brushU_, ctxU_;             // bindings 5,6
  Buf falloff_, stroke_;          // bindings 7,10
  Buf coPrev_, nbrMeta_, nbrVerts_;  // bindings 11,12,13 (neighbor kernels)
};

} // namespace sculptcore::vulkan
