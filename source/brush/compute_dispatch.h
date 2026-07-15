#pragma once

#include "compute_layout.h"

#include <cstdint>

namespace sculptcore::brush {

/* Backend-neutral surface for a GPU brush-compute dispatcher. The batch
 * stroke path (upload mesh once, dispatch one dab at a time reading the
 * previous dab's result, read back at end) is identical across backends —
 * only the GPU API differs — so GpuStrokeSession marshals against this
 * interface and the concrete backend (vk_compute / wgpu_compute) is chosen at
 * begin().
 *
 * The GPU-resident live-render extras (prepareDab/recordDab into a shared
 * command buffer, coBuffer/noBuffer for a sibling normal pass) are NOT here:
 * they are Vulkan-only (cross-API buffer sharing with the renderer), so they
 * stay on the concrete vulkan::BrushComputeDispatch and are reached via that
 * type on the live path. */
struct IBrushComputeDispatch {
  virtual ~IBrushComputeDispatch() = default;

  /* Load the kernel and build the pipeline + bind-group layout. Backend picks
   * its own format (SPIR-V for Vulkan, WGSL text for WebGPU). Call once. */
  virtual bool loadKernel(const char *path) = 0;

  /* Upload full-mesh vertex arrays (one entry per global vertex). co/no are
   * tightly packed xyz triples; mask is one float per vertex. */
  virtual bool beginStroke(const float *co, const float *no, const float *mask,
                           int vertCount) = 0;

  /* Dispatch one brush dab. uniqueVerts are global vertex indices (flattened
   * across this dab's nodes); nodes index into it with count<=64 each, one
   * workgroup per node. falloffLut is 256 floats. strokePath length must equal
   * brushU.stroke_path_count (may be 0). */
  virtual bool dab(const ComputeBrushUniforms &brushU,
                   const ComputeCtxUniforms &ctxU, const uint32_t *uniqueVerts,
                   int uniqueVertCount, const ComputeNodeMeta *nodes,
                   int nodeCount, const float *falloffLut,
                   const ComputeStrokeSample *strokePath, int strokeCount) = 0;

  /* Upload the CSR neighbor topology for for_neighbor kernels (e.g. Smooth).
   * meta has one entry per global vertex; nbrVerts is the flat neighbor-index
   * array. Static across a stroke — call once after beginStroke. */
  virtual bool setNeighbors(const ComputeVertNbr *meta, int vertCount,
                            const uint32_t *nbrVerts, int nbrCount) = 0;

  /* Override the per-vertex cavity automask factor (binding 24). beginStroke
   * seeds identity 1.0; call this once after beginStroke when cavity masking is
   * on. `automask` is one float per global vertex. Default no-op (identity 1.0
   * stays, i.e. no masking) for backends without the buffer. */
  virtual bool setAutomask(const float *automask, int vertCount)
  {
    (void)automask;
    (void)vertCount;
    return false;
  }

  /* Upload a grayscale brush texture (row-major, w*h floats), replacing the
   * 1x1 white dummy. Call once per stroke after beginStroke when textured. */
  virtual bool setBrushTexture(const float *pixels, int width, int height) = 0;

  /* Read co/no/mask back into caller arrays (packed xyz / xyz / f32). Any
   * pointer may be null to skip that readback. */
  virtual bool endStroke(float *coOut, float *noOut, float *maskOut) = 0;

  /* Read back just the listed global vertex indices (packed xyz). coOut/noOut
   * are caller arrays of count*3 floats; either may be null. */
  virtual bool readbackVerts(const uint32_t *verts, int count, float *coOut,
                             float *noOut) = 0;

  /* Upload a custom per-vertex attribute layer into the storage buffer at the
   * given binding slot (>=14, assigned by the kernel's attr manifest in
   * declaration order). `data` is `byteSize` bytes already in the kernel's GPU
   * layout (e.g. tightly packed float4 for a color attr). Persists across dabs
   * like co/no; call once per stroke after beginStroke. Default no-op for
   * backends without attribute support (only the Vulkan path implements it). */
  virtual bool setAttr(uint32_t slot, const void *data, size_t byteSize)
  {
    (void)slot;
    (void)data;
    (void)byteSize;
    return false;
  }
  /* Read a custom attribute layer back (byteSize bytes). Default no-op. */
  virtual bool readbackAttr(uint32_t slot, void *out, size_t byteSize)
  {
    (void)slot;
    (void)out;
    (void)byteSize;
    return false;
  }
};

} // namespace sculptcore::brush
