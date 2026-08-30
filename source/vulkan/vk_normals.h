#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace sculptcore::vulkan {

struct VkContext;

/* GPU-resident vertex-normal recompute + scatter into render VBOs, used by the
 * debug app's WGSL stroke path. A sibling of BrushComputeDispatch: it does not
 * touch the brush's fixed 14-binding layout, it just borrows the dispatcher's
 * persistent stride-16 `co`/`no` STORAGE buffers (via coBuffer()/noBuffer()).
 *
 * Three compute pipelines, all driven through VkContext::runOneShot (submit +
 * wait), so no barriers are needed between them:
 *   - face: per triangle -> normalize(cross(b-a, c-a)) into a stride-16 scratch
 *   - vert: per vertex    -> normalize(sum of incident triangle normals) -> no
 *   - scatter: per render-VBO slot -> pos[s]=co[slotVertex[s]], nor[s]=no[...]
 *
 * Topology (triangle vertex indices + a vertex->incident-triangle CSR) is built
 * once per stroke on the host and uploaded via setTopology(); the mesh is static
 * during a stroke so it never changes mid-stroke. Normals here are a global sum
 * over all incident triangles — they are render-only and intentionally NOT
 * bit-identical to the per-node CPU normals (see brush_compute / the plan). */
struct GpuNormalPass {
  explicit GpuNormalPass(VkContext *ctx) : ctx_(ctx)
  {
  }
  GpuNormalPass(const GpuNormalPass &) = delete;
  ~GpuNormalPass();

  /* Load the three .spv from VK_COMPUTE_SPV_DIR and build pipelines. Idempotent
   * per instance; call once. Returns false if a shader is missing. */
  bool init();

  /* Upload the global triangle topology. `triVerts` is 3*triCount global vertex
   * indices (fan-triangulated faces, matching regen_node_tris). `vtriMeta` is
   * one (offset,count) per global vertex into `vtriList`, the flat list of
   * incident triangle indices. */
  bool setTopology(const uint32_t *triVerts,
                   int triCount,
                   const uint32_t *vtriMeta,
                   int vertCount,
                   const uint32_t *vtriList,
                   int listCount);

  /* Recompute per-vertex normals into `no` from positions `co` (both the
   * dispatcher's stride-16 STORAGE buffers) over the whole mesh. Runs the face
   * pass then the vert pass, each its own submit. Used once at stroke begin. */
  bool computeNormals(VkBuffer co, VkBuffer no);

  /* Localized recompute, split into a CPU prepare + a record-into-cb half so
   * the caller can batch the two normal dispatches with the brush dab into a
   * single submit. `workTris`/`workVerts` are the triangle/vertex indices to
   * recompute (a dab's affected 1-ring); pass identity lists for a full pass.
   * Call prepareNormals(), then recordNormals(cb) inside the shared command
   * buffer (a face→vert memory barrier is recorded between the two). */
  bool prepareNormals(VkBuffer co,
                      VkBuffer no,
                      const uint32_t *workTris,
                      int triWorkCount,
                      const uint32_t *workVerts,
                      int vertWorkCount);
  void recordNormals(VkCommandBuffer cb);

  /* Scatter one GPU node's slice into its render VBOs: for slot s,
   * pos[s] = co[slotVertex[s]].xyz, nor[s] = no[slotVertex[s]].xyz. `pos`/`nor`
   * are tightly-packed float3 VkBuffers (STORAGE-capable render VBOs);
   * `slotVertex` is a uint-per-slot STORAGE buffer. */
  bool scatter(VkBuffer co,
               VkBuffer no,
               VkBuffer slotVertex,
               VkBuffer pos,
               VkBuffer nor,
               int slotCount);

  /* Record-into-cb variant of scatter() so several GPU-node scatters can ride
   * the dab+normals submit instead of each costing its own queue-wait. Call
   * beginScatterBatch() once per command buffer, then recordScatter() per node:
   * each grabs a fresh descriptor set (one cb can't reuse a single set across
   * dispatches). The caller is responsible for a compute-write->read barrier
   * before the first scatter (the dab/vert passes write the co/no it reads). */
  void beginScatterBatch();
  void recordScatter(VkCommandBuffer cb,
                     VkBuffer co,
                     VkBuffer no,
                     VkBuffer slotVertex,
                     VkBuffer pos,
                     VkBuffer nor,
                     int slotCount);

  /* A compute-write -> compute-read global memory barrier, for chaining
   * dependent dispatches inside one command buffer (e.g. dab -> normals). */
  static void computeBarrier(VkCommandBuffer cb);

private:
  struct Buf {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr;
  };

  /* One compute pipeline + its descriptor layout/set. `bindings` storage
   * buffers, plus a single push-constant uint (the element count). */
  struct Pipe {
    VkDescriptorSetLayout dsLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    int bindings = 0;
  };

  bool createBuf(Buf &b, VkDeviceSize size);
  void destroyBuf(Buf &b);
  bool ensureBuf(Buf &b, VkDeviceSize size);

  bool loadModule(const char *name, VkShaderModule &out);
  bool buildPipe(Pipe &p, VkShaderModule module, int bindingCount);
  void bindStorage(VkDescriptorSet set, uint32_t binding, VkBuffer buf);
  bool dispatch(const Pipe &p, uint32_t count);
  VkDescriptorSet nextScatterSet();

  VkContext *ctx_ = nullptr;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;

  /* Dedicated pool of scatter descriptor sets for recordScatter(): one set per
   * GPU node touched in a dab, reused round-robin across submits. Grown on
   * demand from this pool. */
  VkDescriptorPool scatterPool_ = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> scatterSets_;
  size_t scatterCursor_ = 0;

  Pipe face_, vert_, scatter_;

  Buf triVerts_;  // uint[3*triCount]
  Buf triNo_;     // vec4[triCount] (stride-16 scratch)
  Buf vtriMeta_;  // uvec2[vertCount]
  Buf vtriList_;  // uint[listCount]
  Buf workTris_;  // uint[] dispatch index list (face pass)
  Buf workVerts_; // uint[] dispatch index list (vert pass)

  int triCount_ = 0;
  int vertCount_ = 0;
  int triWork_ = 0;  // current face-pass work count (for recordNormals)
  int vertWork_ = 0; // current vert-pass work count
};

} // namespace sculptcore::vulkan
