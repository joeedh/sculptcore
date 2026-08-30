#include "wgpu_stencil.h"
#include "wgpu_context.h"

#ifndef __EMSCRIPTEN__
#include <webgpu/wgpu.h> // wgpu-native extension: wgpuDevicePoll
#endif

#include "subdiv/subdiv.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace sculptcore::webgpu {

using litestl::math::float3;
using litestl::util::Vector;

namespace {

WGPUStringView strView(const char *s)
{
  WGPUStringView v;
  v.data = s;
  v.length = s ? std::strlen(s) : 0;
  return v;
}

void drain(WgpuContext *ctx)
{
#ifndef __EMSCRIPTEN__
  wgpuDevicePoll(ctx->device, /*wait=*/true, nullptr);
#endif
  wgpuInstanceProcessEvents(ctx->instance);
}

struct MapReq {
  bool done = false;
  bool ok = false;
};

void onMap(WGPUMapAsyncStatus status, WGPUStringView message, void *ud1, void *)
{
  auto *r = static_cast<MapReq *>(ud1);
  r->ok = status == WGPUMapAsyncStatus_Success;
  if (!r->ok) {
    std::fprintf(stderr,
                 "wgpu_stencil mapAsync failed: %.*s\n",
                 int(message.length),
                 message.data ? message.data : "");
  }
  r->done = true;
}

WGPUBuffer
makeBuffer(WgpuContext *ctx, uint64_t size, WGPUBufferUsage usage, const void *data)
{
  size = (size + 3u) & ~uint64_t(3u);
  if (size == 0) {
    size = 16;
  }
  WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
  bd.usage = usage;
  bd.size = size;
  WGPUBuffer b = wgpuDeviceCreateBuffer(ctx->device, &bd);
  if (b && data) {
    wgpuQueueWriteBuffer(ctx->queue, b, 0, data, size_t(size));
  }
  return b;
}

/* One thread per fine vert: evaluate its CSR row over the previous level's
 * positions. Separate mul + add per component, entries ascending by source id
 * — the exact StencilTable::eval arithmetic (bit-consistency contract). */
const char *kSpmvWgsl = R"(
struct Params { fineCount : u32, wgCountX : u32, pad1 : u32, pad2 : u32 }
@group(0) @binding(0) var<storage, read> offsets : array<u32>;
@group(0) @binding(1) var<storage, read> indices : array<u32>;
@group(0) @binding(2) var<storage, read> weights : array<f32>;
@group(0) @binding(3) var<storage, read> src : array<f32>;
@group(0) @binding(4) var<storage, read_write> dst : array<f32>;
@group(0) @binding(5) var<uniform> params : Params;

@compute @workgroup_size(64)
fn main(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_id) lid : vec3<u32>) {
  // 2D-linearized dispatch: workgroup counts per dimension cap at 65535,
  // which a >4M-vert level exceeds in x alone.
  let i = (wg.y * params.wgCountX + wg.x) * 64u + lid.x;
  if (i >= params.fineCount) {
    return;
  }
  var px = 0.0;
  var py = 0.0;
  var pz = 0.0;
  let e = offsets[i + 1u];
  for (var k = offsets[i]; k < e; k = k + 1u) {
    let s = indices[k] * 3u;
    let w = weights[k];
    // Explicit fma matches StencilTable::eval's std::fma chain bit-for-bit
    // (single IEEE rounding); plain mul+add is driver-contractable and drifts.
    px = fma(src[s], w, px);
    py = fma(src[s + 1u], w, py);
    pz = fma(src[s + 2u], w, pz);
  }
  dst[i * 3u] = px;
  dst[i * 3u + 1u] = py;
  dst[i * 3u + 2u] = pz;
}
)";

} // namespace

WgpuStencilAmplify::~WgpuStencilAmplify()
{
  release();
}

void WgpuStencilAmplify::release()
{
  for (LevelPass &lp : levels_) {
    if (lp.bindGroup)
      wgpuBindGroupRelease(lp.bindGroup);
    if (lp.offsets)
      wgpuBufferRelease(lp.offsets);
    if (lp.indices)
      wgpuBufferRelease(lp.indices);
    if (lp.weights)
      wgpuBufferRelease(lp.weights);
    if (lp.params)
      wgpuBufferRelease(lp.params);
    if (lp.dst)
      wgpuBufferRelease(lp.dst);
  }
  levels_.clear();
  if (src_)
    wgpuBufferRelease(src_);
  if (staging_)
    wgpuBufferRelease(staging_);
  if (pipeline_)
    wgpuComputePipelineRelease(pipeline_);
  if (pipeLayout_)
    wgpuPipelineLayoutRelease(pipeLayout_);
  if (bgLayout_)
    wgpuBindGroupLayoutRelease(bgLayout_);
  if (module_)
    wgpuShaderModuleRelease(module_);
  src_ = staging_ = nullptr;
  pipeline_ = nullptr;
  pipeLayout_ = nullptr;
  bgLayout_ = nullptr;
  module_ = nullptr;
}

bool WgpuStencilAmplify::init(WgpuContext *ctx,
                              subdiv::Refiner &refiner,
                              int fromLevel,
                              int toLevel)
{
  release();
  ctx_ = ctx;
  if (fromLevel < 1 || toLevel <= fromLevel || toLevel > int(refiner.levels.size())) {
    std::fprintf(stderr, "wgpu_stencil: bad level range %d..%d\n", fromLevel, toLevel);
    return false;
  }
  srcCount_ = refiner.levels[fromLevel - 1].vertCount;

  WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
  wgsl.code = strView(kSpmvWgsl);
  WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  smd.nextInChain = &wgsl.chain;
  module_ = wgpuDeviceCreateShaderModule(ctx_->device, &smd);
  if (!module_) {
    return false;
  }

  WGPUBindGroupLayoutEntry entries[6] = {};
  for (int i = 0; i < 6; i++) {
    entries[i] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[i].binding = uint32_t(i);
    entries[i].visibility = WGPUShaderStage_Compute;
    entries[i].buffer.type = i == 4   ? WGPUBufferBindingType_Storage
                             : i == 5 ? WGPUBufferBindingType_Uniform
                                      : WGPUBufferBindingType_ReadOnlyStorage;
  }
  WGPUBindGroupLayoutDescriptor bgld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
  bgld.entryCount = 6;
  bgld.entries = entries;
  bgLayout_ = wgpuDeviceCreateBindGroupLayout(ctx_->device, &bgld);

  WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
  pld.bindGroupLayoutCount = 1;
  pld.bindGroupLayouts = &bgLayout_;
  pipeLayout_ = wgpuDeviceCreatePipelineLayout(ctx_->device, &pld);

  WGPUComputePipelineDescriptor cpd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
  cpd.layout = pipeLayout_;
  cpd.compute.module = module_;
  cpd.compute.entryPoint = strView("main");
  pipeline_ = wgpuDeviceCreateComputePipeline(ctx_->device, &cpd);
  if (!bgLayout_ || !pipeLayout_ || !pipeline_) {
    return false;
  }

  const WGPUBufferUsage ro = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
  src_ = makeBuffer(ctx_, uint64_t(srcCount_) * 3 * sizeof(float), ro, nullptr);
  if (!src_) {
    return false;
  }

  WGPUBuffer prev = src_;
  for (int l = fromLevel + 1; l <= toLevel; l++) {
    subdiv::StencilTable &st = refiner.levels[l - 1].stencil;
    LevelPass lp;
    lp.fineCount = st.fineCount;

    uint64_t offB = uint64_t(st.offsets.size()) * 4;
    uint64_t idxB = uint64_t(st.indices.size()) * 4;
    uint64_t wB = uint64_t(st.weights.size()) * 4;
    uint64_t dstB = uint64_t(st.fineCount) * 3 * sizeof(float);
    std::fprintf(stderr,
                 "wgpu_stencil L%d: rows=%d offsets=%.1fMB indices=%.1fMB "
                 "weights=%.1fMB dst=%.1fMB\n",
                 l,
                 st.fineCount,
                 offB / 1e6,
                 idxB / 1e6,
                 wB / 1e6,
                 dstB / 1e6);

    lp.offsets = makeBuffer(ctx_, offB, ro, st.offsets.data());
    lp.indices = makeBuffer(ctx_, idxB, ro, st.indices.data());
    lp.weights = makeBuffer(ctx_, wB, ro, st.weights.data());
    uint32_t groups = uint32_t((st.fineCount + 63) / 64);
    lp.wgCountX = groups < 65535u ? groups : 65535u;
    lp.wgCountY = (groups + lp.wgCountX - 1) / lp.wgCountX;
    uint32_t params[4] = {uint32_t(st.fineCount), lp.wgCountX, 0, 0};
    lp.params = makeBuffer(
        ctx_, sizeof(params), WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, params);
    lp.dst = makeBuffer(
        ctx_, dstB, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc, nullptr);
    if (!lp.offsets || !lp.indices || !lp.weights || !lp.params || !lp.dst) {
      return false;
    }

    WGPUBindGroupEntry bge[6] = {};
    WGPUBuffer bufs[6] = {lp.offsets, lp.indices, lp.weights, prev, lp.dst, lp.params};
    for (int i = 0; i < 6; i++) {
      bge[i] = WGPU_BIND_GROUP_ENTRY_INIT;
      bge[i].binding = uint32_t(i);
      bge[i].buffer = bufs[i];
      bge[i].offset = 0;
      bge[i].size = WGPU_WHOLE_SIZE;
    }
    WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgd.layout = bgLayout_;
    bgd.entryCount = 6;
    bgd.entries = bge;
    lp.bindGroup = wgpuDeviceCreateBindGroup(ctx_->device, &bgd);
    if (!lp.bindGroup) {
      return false;
    }

    prev = lp.dst;
    levels_.append(lp);
  }
  return true;
}

bool WgpuStencilAmplify::dispatch(const float *srcCo, int srcCount)
{
  if (!pipeline_ || srcCount != srcCount_) {
    std::fprintf(stderr, "wgpu_stencil: dispatch before init / count mismatch\n");
    return false;
  }
  auto t0 = std::chrono::steady_clock::now();
  wgpuQueueWriteBuffer(ctx_->queue, src_, 0, srcCo, size_t(srcCount) * 3 * sizeof(float));

  WGPUCommandEncoderDescriptor ced = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(ctx_->device, &ced);
  for (LevelPass &lp : levels_) {
    WGPUComputePassDescriptor cpd = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, &cpd);
    wgpuComputePassEncoderSetPipeline(pass, pipeline_);
    wgpuComputePassEncoderSetBindGroup(pass, 0, lp.bindGroup, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, lp.wgCountX, lp.wgCountY, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
  }
  WGPUCommandBufferDescriptor cbd = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, &cbd);
  wgpuQueueSubmit(ctx_->queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);
  drain(ctx_);

  lastDispatchMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
          .count();
  return true;
}

bool WgpuStencilAmplify::readback(Vector<float3> &out)
{
  if (levels_.size() == 0) {
    return false;
  }
  LevelPass &last = levels_.last();
  uint64_t bytes = uint64_t(last.fineCount) * 3 * sizeof(float);

  if (!staging_) {
    staging_ = makeBuffer(
        ctx_, bytes, WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead, nullptr);
    if (!staging_) {
      return false;
    }
  }

  WGPUCommandEncoderDescriptor ced = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(ctx_->device, &ced);
  wgpuCommandEncoderCopyBufferToBuffer(enc, last.dst, 0, staging_, 0, bytes);
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(ctx_->queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);
  drain(ctx_);

  MapReq mreq;
  WGPUBufferMapCallbackInfo mci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
  mci.mode = WGPUCallbackMode_AllowProcessEvents;
  mci.callback = onMap;
  mci.userdata1 = &mreq;
  wgpuBufferMapAsync(staging_, WGPUMapMode_Read, 0, bytes, mci);
  while (!mreq.done) {
    drain(ctx_);
  }
  if (!mreq.ok) {
    return false;
  }
  const auto *mapped =
      static_cast<const float *>(wgpuBufferGetConstMappedRange(staging_, 0, bytes));
  if (!mapped) {
    wgpuBufferUnmap(staging_);
    return false;
  }
  out.resize(last.fineCount);
  std::memcpy(out.data(), mapped, size_t(bytes));
  wgpuBufferUnmap(staging_);
  return true;
}

} // namespace sculptcore::webgpu
