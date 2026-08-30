#pragma once

/** GPU stencil-table amplification (displacementAndSubSurf plan, S5): chained
 * per-level SpMV of the S1 refiner's cached stencil tables, coarse (edit)
 * level -> render level, on the WebGPU device. Each level's kernel evaluates
 * its CSR rows in ascending-index order as an fma() chain — the arithmetic
 * StencilTable::eval defines (std::fma per component; single IEEE rounding on
 * both sides) — so the amplified positions are bit-identical to the CPU chain
 * (a composed single-SpMV can't be; see the S1 status note). Caveat: WGSL
 * permits (but desktop drivers don't do) unfused fma lowering — the gate
 * test asserts bit-equality, so a deviating driver is caught, not silent. Deliberately
 * VDM- and displacement-agnostic: stored deltas above the edit level are the CPU chain's
 * job; this pass gives fine DISPLAY density from an edit level (X3 reuses it for the
 * tessellated tier).
 *
 * The result buffer stays on-device (tight xyz f32 triplets, Storage|CopySrc
 * usage) for a future draw tier; readback() is the test/verify path. */

#include <webgpu/webgpu.h>

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

namespace sculptcore::subdiv {
struct Refiner;
}

namespace sculptcore::webgpu {

struct WgpuContext;

struct WgpuStencilAmplify {
  WgpuStencilAmplify() = default;
  WgpuStencilAmplify(const WgpuStencilAmplify &) = delete;
  ~WgpuStencilAmplify();

  /** Upload the stencil chain for levels (fromLevel, toLevel] (1-based
   * refiner levels; fromLevel >= 1 so source ids are dense) and build the
   * SpMV pipeline + per-level bind groups. Prints per-level buffer byte
   * sizes (the plan's storage-budget validation). */
  bool init(WgpuContext *ctx, subdiv::Refiner &refiner, int fromLevel, int toLevel);

  /** Upload the coarse level's positions (dense vert ids, xyz f32 triplets)
   * and run the chained dispatches. Blocks until the queue drains;
   * lastDispatchMs records the wall time (the per-frame budget). */
  bool dispatch(const float *srcCo, int srcCount);

  /** Read the finest level's positions back (fineCount() float3s). */
  bool readback(litestl::util::Vector<litestl::math::float3> &out);

  int fineCount()
  {
    return levels_.size() ? levels_.last().fineCount : 0;
  }

  /** On-device result (tight xyz f32; valid after dispatch). */
  WGPUBuffer resultBuffer()
  {
    return levels_.size() ? levels_.last().dst : nullptr;
  }

  double lastDispatchMs = 0.0;

private:
  struct LevelPass {
    int fineCount = 0;
    uint32_t wgCountX = 1, wgCountY = 1; /* 2D grid: 65535-per-dim dispatch cap */
    WGPUBuffer offsets = nullptr;
    WGPUBuffer indices = nullptr;
    WGPUBuffer weights = nullptr;
    WGPUBuffer params = nullptr;
    WGPUBuffer dst = nullptr;
    WGPUBindGroup bindGroup = nullptr;
  };

  void release();

  WgpuContext *ctx_ = nullptr;
  int srcCount_ = 0;
  WGPUBuffer src_ = nullptr;
  WGPUBuffer staging_ = nullptr;
  WGPUShaderModule module_ = nullptr;
  WGPUBindGroupLayout bgLayout_ = nullptr;
  WGPUPipelineLayout pipeLayout_ = nullptr;
  WGPUComputePipeline pipeline_ = nullptr;
  litestl::util::Vector<LevelPass> levels_;
};

} // namespace sculptcore::webgpu
