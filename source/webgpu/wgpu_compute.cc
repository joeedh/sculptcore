#include "wgpu_compute.h"
#include "wgpu_context.h"

#ifndef __EMSCRIPTEN__
#include <webgpu/wgpu.h> // wgpu-native extension: wgpuDevicePoll
#endif

#include "litestl/util/vector.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace sculptcore::webgpu {

using brush::ComputeBrushUniforms;
using brush::ComputeCtxUniforms;
using brush::ComputeNodeMeta;
using brush::ComputeStrokeSample;
using brush::ComputeVertNbr;

static constexpr uint64_t kVec3Stride = 16; // std430 array<vec3<f32>>

namespace {

WGPUStringView strView(const char *s)
{
  WGPUStringView v{};
  v.data = s;
  v.length = s ? std::strlen(s) : 0;
  return v;
}

/* Drain the device until pending submits/maps complete. On native this is the
 * wgpu-native wgpuDevicePoll(wait=true) the screenshot path uses; processEvents
 * fires the AllowProcessEvents callbacks. */
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
                 "wgpu_compute mapAsync failed: %.*s\n",
                 int(message.length),
                 message.data ? message.data : "");
  }
  r->done = true;
}

} // namespace

WgpuBrushComputeDispatch::~WgpuBrushComputeDispatch()
{
  destroyBuf(co_);
  destroyBuf(no_);
  destroyBuf(mask_);
  destroyBuf(unique_);
  destroyBuf(nodes_);
  destroyBuf(brushU_);
  destroyBuf(ctxU_);
  destroyBuf(falloff_);
  destroyBuf(stroke_);
  destroyBuf(coPrev_);
  destroyBuf(nbrMeta_);
  destroyBuf(nbrVerts_);
  destroyBuf(disp_);
  destroyBuf(dabStamp_);
  destroyBuf(automask_);
  destroyBuf(texParams_);
  destroyBuf(readback_);
  destroyBrushTexture();
  if (sampler_)
    wgpuSamplerRelease(sampler_);
  if (whiteView_)
    wgpuTextureViewRelease(whiteView_);
  if (whiteTexture_)
    wgpuTextureRelease(whiteTexture_);
  if (pipeline_)
    wgpuComputePipelineRelease(pipeline_);
  if (pipeLayout_)
    wgpuPipelineLayoutRelease(pipeLayout_);
  if (bgLayout_)
    wgpuBindGroupLayoutRelease(bgLayout_);
  if (module_)
    wgpuShaderModuleRelease(module_);
}

void WgpuBrushComputeDispatch::destroyBuf(Buf &b)
{
  if (b.buffer)
    wgpuBufferRelease(b.buffer);
  b.buffer = nullptr;
  b.size = 0;
}

bool WgpuBrushComputeDispatch::ensureBuf(Buf &b, uint64_t size, WGPUBufferUsage usage)
{
  if (size == 0)
    size = 16;
  size = (size + 3u) & ~uint64_t(3u); // WebGPU buffer sizes are 4-multiples.
  if (b.buffer && b.size >= size)
    return true;
  destroyBuf(b);
  uint64_t rounded = 256;
  while (rounded < size)
    rounded *= 2;
  WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
  bd.usage = usage;
  bd.size = rounded;
  b.buffer = wgpuDeviceCreateBuffer(ctx_->device, &bd);
  b.size = rounded;
  return b.buffer != nullptr;
}

bool WgpuBrushComputeDispatch::hasBinding(uint32_t bind) const
{
  for (const auto &b : bindings_) {
    if (b.binding == bind)
      return true;
  }
  return false;
}

bool WgpuBrushComputeDispatch::createWhiteTexture()
{
  WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
  td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
  td.dimension = WGPUTextureDimension_2D;
  td.size = {1, 1, 1};
  td.format = WGPUTextureFormat_R32Float;
  whiteTexture_ = wgpuDeviceCreateTexture(ctx_->device, &td);
  if (!whiteTexture_)
    return false;

  float white = 1.0f;
  WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
  dst.texture = whiteTexture_;
  WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
  layout.bytesPerRow = sizeof(float);
  layout.rowsPerImage = 1;
  WGPUExtent3D ext{1, 1, 1};
  wgpuQueueWriteTexture(ctx_->queue, &dst, &white, sizeof(float), &layout, &ext);

  whiteView_ = wgpuTextureCreateView(whiteTexture_, nullptr);
  texView_ = whiteView_;

  WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
  sampler_ = wgpuDeviceCreateSampler(ctx_->device, &sd);
  return whiteView_ && sampler_;
}

void WgpuBrushComputeDispatch::destroyBrushTexture()
{
  if (texView_ && texView_ != whiteView_)
    wgpuTextureViewRelease(texView_);
  if (texTexture_)
    wgpuTextureRelease(texTexture_);
  texTexture_ = nullptr;
  texView_ = whiteView_;
}

bool WgpuBrushComputeDispatch::setBrushTexture(const float *pixels, int width, int height)
{
  if (width <= 0 || height <= 0 || !pixels)
    return false;
  destroyBrushTexture(); // one texture per stroke; drop any previous.

  WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
  td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
  td.dimension = WGPUTextureDimension_2D;
  td.size = {uint32_t(width), uint32_t(height), 1};
  td.format = WGPUTextureFormat_R32Float; // exact float match for tex_pixels.
  texTexture_ = wgpuDeviceCreateTexture(ctx_->device, &td);
  if (!texTexture_)
    return false;

  // wgpuQueueWriteTexture has no 256-byte row alignment requirement (unlike
  // copyBufferToTexture), so the source rows are tightly packed w floats.
  WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
  dst.texture = texTexture_;
  WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
  layout.bytesPerRow = uint32_t(width) * sizeof(float);
  layout.rowsPerImage = uint32_t(height);
  WGPUExtent3D ext{uint32_t(width), uint32_t(height), 1};
  wgpuQueueWriteTexture(ctx_->queue,
                        &dst,
                        pixels,
                        size_t(width) * size_t(height) * sizeof(float),
                        &layout,
                        &ext);

  texView_ = wgpuTextureCreateView(texTexture_, nullptr);
  return texView_ != nullptr;
}

bool WgpuBrushComputeDispatch::loadKernel(const char *path)
{
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    std::fprintf(stderr, "WgpuBrushComputeDispatch: cannot open '%s'\n", path);
    return false;
  }
  std::streamsize n = f.tellg();
  f.seekg(0);
  std::string text(size_t(n), '\0');
  if (!f.read(text.data(), n)) {
    std::fprintf(stderr, "WgpuBrushComputeDispatch: cannot read '%s'\n", path);
    return false;
  }
  return loadKernelSource(text.c_str(), path);
}

bool WgpuBrushComputeDispatch::loadKernelSource(const char *srcText, const char *label)
{
  std::string src(srcText);

  // Parse the actual `@group(0) @binding(N) var<...> name: type;` table so the
  // bind-group layout matches the kernel's declared access qualifiers exactly
  // (mirrors parseBindings/layoutEntry in tests/webgpu/replay.mjs). Non-neighbor
  // kernels declare bindings 0-10; for_neighbor kernels add 11-13.
  bindings_.clear();
  size_t pos = 0;
  const std::string tag = "@binding(";
  while ((pos = src.find(tag, pos)) != std::string::npos) {
    size_t np = pos + tag.size();
    uint32_t bind = 0;
    bool any = false;
    while (np < src.size() && src[np] >= '0' && src[np] <= '9') {
      bind = bind * 10 + uint32_t(src[np] - '0');
      np++;
      any = true;
    }
    pos = np;
    if (!any)
      continue;
    size_t semi = src.find(';', np);
    if (semi == std::string::npos)
      break;
    std::string decl = src.substr(np, semi - np); // ") var<...> name: type"
    auto strip = decl;
    // Collapse whitespace to classify the var<...> qualifier / type.
    std::string flat;
    for (char c : strip) {
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
        flat += c;
    }
    BindKind kind;
    if (flat.find("var<uniform>") != std::string::npos) {
      kind = BindKind::Uniform;
    } else if (flat.find("var<storage,read_write>") != std::string::npos) {
      kind = BindKind::StorageRW;
    } else if (flat.find("var<storage,read>") != std::string::npos) {
      kind = BindKind::StorageRO;
    } else if (flat.find("texture_2d") != std::string::npos) {
      kind = BindKind::Texture;
    } else if (flat.find(":sampler") != std::string::npos) {
      kind = BindKind::Sampler;
    } else {
      std::fprintf(stderr, "WgpuBrushComputeDispatch: unhandled binding %u\n", bind);
      return false;
    }
    bindings_.append(BindingInfo{bind, kind});
  }
  if (bindings_.size() == 0) {
    std::fprintf(stderr, "WgpuBrushComputeDispatch: no bindings in '%s'\n", label);
    return false;
  }

  WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
  wgsl.code = strView(src.c_str());
  WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  smd.nextInChain = &wgsl.chain;
  module_ = wgpuDeviceCreateShaderModule(ctx_->device, &smd);
  if (!module_) {
    std::fprintf(stderr, "WgpuBrushComputeDispatch: shader module failed '%s'\n", label);
    return false;
  }

  litestl::util::Vector<WGPUBindGroupLayoutEntry> entries;
  for (const auto &b : bindings_) {
    WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    e.binding = b.binding;
    e.visibility = WGPUShaderStage_Compute;
    switch (b.kind) {
    case BindKind::Uniform:
      e.buffer.type = WGPUBufferBindingType_Uniform;
      break;
    case BindKind::StorageRW:
      e.buffer.type = WGPUBufferBindingType_Storage;
      break;
    case BindKind::StorageRO:
      e.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
      break;
    case BindKind::Texture:
      e.texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
      e.texture.viewDimension = WGPUTextureViewDimension_2D;
      break;
    case BindKind::Sampler:
      e.sampler.type = WGPUSamplerBindingType_NonFiltering;
      break;
    }
    entries.append(e);
  }

  WGPUBindGroupLayoutDescriptor bgld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
  bgld.entryCount = entries.size();
  bgld.entries = entries.data();
  bgLayout_ = wgpuDeviceCreateBindGroupLayout(ctx_->device, &bgld);
  if (!bgLayout_)
    return false;

  WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
  pld.bindGroupLayoutCount = 1;
  pld.bindGroupLayouts = &bgLayout_;
  pipeLayout_ = wgpuDeviceCreatePipelineLayout(ctx_->device, &pld);
  if (!pipeLayout_)
    return false;

  WGPUComputePipelineDescriptor cpd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
  cpd.layout = pipeLayout_;
  cpd.compute.module = module_;
  cpd.compute.entryPoint = strView("main");
  pipeline_ = wgpuDeviceCreateComputePipeline(ctx_->device, &cpd);
  if (!pipeline_) {
    std::fprintf(stderr, "WgpuBrushComputeDispatch: pipeline failed '%s'\n", label);
    return false;
  }

  return createWhiteTexture();
}

bool WgpuBrushComputeDispatch::beginStroke(const float *co,
                                           const float *no,
                                           const float *mask,
                                           int vertCount)
{
  vertCount_ = vertCount;
  hasNeighbors_ = false;
  const WGPUBufferUsage rw =
      WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
  const WGPUBufferUsage ro = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
  if (!ensureBuf(co_, uint64_t(vertCount) * kVec3Stride, rw) ||
      !ensureBuf(no_, uint64_t(vertCount) * kVec3Stride, rw) ||
      !ensureBuf(mask_, uint64_t(vertCount) * sizeof(float), rw) ||
      !ensureBuf(coPrev_, uint64_t(vertCount) * kVec3Stride, ro) ||
      !ensureBuf(disp_, uint64_t(vertCount) * kVec3Stride, rw) ||
      !ensureBuf(dabStamp_, uint64_t(vertCount) * sizeof(uint32_t), rw) ||
      !ensureBuf(automask_, uint64_t(vertCount) * sizeof(float), ro) ||
      !ensureBuf(nbrMeta_, 0, ro) || !ensureBuf(nbrVerts_, 0, ro))
  {
    return false;
  }

  // Expand packed xyz into 16-byte std430 vec3 slots, then upload.
  litestl::util::Vector<float> tmp;
  tmp.resize(size_t(vertCount) * 4);
  for (int i = 0; i < vertCount; i++) {
    tmp[i * 4 + 0] = co[i * 3 + 0];
    tmp[i * 4 + 1] = co[i * 3 + 1];
    tmp[i * 4 + 2] = co[i * 3 + 2];
    tmp[i * 4 + 3] = 0.0f;
  }
  wgpuQueueWriteBuffer(
      ctx_->queue, co_.buffer, 0, tmp.data(), size_t(vertCount) * kVec3Stride);
  {
    // Accumulated displacement starts at zero: nothing has been deposited yet,
    // so `co - disp` is the stroke-start surface for every vert. This is the
    // whole of the CPU generational stamp on a static-topology GPU stroke.
    std::memset(tmp.data(), 0, size_t(vertCount) * kVec3Stride);
    wgpuQueueWriteBuffer(
        ctx_->queue, disp_.buffer, 0, tmp.data(), size_t(vertCount) * kVec3Stride);
  }
  for (int i = 0; i < vertCount; i++) {
    tmp[i * 4 + 0] = no[i * 3 + 0];
    tmp[i * 4 + 1] = no[i * 3 + 1];
    tmp[i * 4 + 2] = no[i * 3 + 2];
    tmp[i * 4 + 3] = 0.0f;
  }
  wgpuQueueWriteBuffer(
      ctx_->queue, no_.buffer, 0, tmp.data(), size_t(vertCount) * kVec3Stride);
  wgpuQueueWriteBuffer(
      ctx_->queue, mask_.buffer, 0, mask, size_t(vertCount) * sizeof(float));
  {
    // Zero the grab first-touch stamps: gen 0 never matches (gens start at 1).
    litestl::util::Vector<uint32_t> zeros;
    zeros.resize(vertCount);
    std::memset(zeros.data(), 0, size_t(vertCount) * sizeof(uint32_t));
    wgpuQueueWriteBuffer(ctx_->queue,
                         dabStamp_.buffer,
                         0,
                         zeros.data(),
                         size_t(vertCount) * sizeof(uint32_t));
  }
  {
    // Cavity automask defaults to identity 1.0; setAutomask overrides it when
    // cavity masking is on. Identity keeps strength*1.0 == strength.
    litestl::util::Vector<float> ones;
    ones.resize(vertCount);
    for (int i = 0; i < vertCount; i++) {
      ones[i] = 1.0f;
    }
    wgpuQueueWriteBuffer(
        ctx_->queue, automask_.buffer, 0, ones.data(), size_t(vertCount) * sizeof(float));
  }
  return true;
}

bool WgpuBrushComputeDispatch::setAutomask(const float *automask, int vertCount)
{
  if (!automask_.buffer || vertCount > vertCount_) {
    return false;
  }
  wgpuQueueWriteBuffer(
      ctx_->queue, automask_.buffer, 0, automask, size_t(vertCount) * sizeof(float));
  return true;
}

bool WgpuBrushComputeDispatch::setTexParams(const float *data, int count)
{
  if (count < 1) {
    return false;
  }
  const WGPUBufferUsage ro = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
  if (!ensureBuf(texParams_, uint64_t(count) * sizeof(float), ro)) {
    return false;
  }
  wgpuQueueWriteBuffer(
      ctx_->queue, texParams_.buffer, 0, data, size_t(count) * sizeof(float));
  return true;
}

bool WgpuBrushComputeDispatch::setNeighbors(const ComputeVertNbr *meta,
                                            int vertCount,
                                            const uint32_t *nbrVerts,
                                            int nbrCount)
{
  const WGPUBufferUsage ro = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
  if (!ensureBuf(nbrMeta_, uint64_t(vertCount) * sizeof(ComputeVertNbr), ro) ||
      !ensureBuf(nbrVerts_, uint64_t(nbrCount < 1 ? 1 : nbrCount) * sizeof(uint32_t), ro))
  {
    return false;
  }
  wgpuQueueWriteBuffer(
      ctx_->queue, nbrMeta_.buffer, 0, meta, size_t(vertCount) * sizeof(ComputeVertNbr));
  if (nbrCount > 0) {
    wgpuQueueWriteBuffer(
        ctx_->queue, nbrVerts_.buffer, 0, nbrVerts, size_t(nbrCount) * sizeof(uint32_t));
  }
  hasNeighbors_ = true;
  return true;
}

WGPUBindGroup WgpuBrushComputeDispatch::buildBindGroup()
{
  litestl::util::Vector<WGPUBindGroupEntry> entries;
  for (const auto &b : bindings_) {
    WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
    e.binding = b.binding;
    const Buf *buf = nullptr;
    switch (b.binding) {
    case 0:
      buf = &co_;
      break;
    case 1:
      buf = &no_;
      break;
    case 2:
      buf = &mask_;
      break;
    case 3:
      buf = &unique_;
      break;
    case 4:
      buf = &nodes_;
      break;
    case 5:
      buf = &brushU_;
      break;
    case 6:
      buf = &ctxU_;
      break;
    case 7:
      buf = &falloff_;
      break;
    case 8:
      e.textureView = texView_;
      break;
    case 9:
      e.sampler = sampler_;
      break;
    case 10:
      buf = &stroke_;
      break;
    case 11:
      buf = &coPrev_;
      break;
    case 12:
      buf = &nbrMeta_;
      break;
    case 13:
      buf = &nbrVerts_;
      break;
    case brush::kDabStampBinding:
      buf = &dabStamp_;
      break;
    case brush::kAutomaskBinding:
      buf = &automask_;
      break;
    case brush::kDispBinding:
      buf = &disp_;
      break;
    case brush::kTexParamsBinding:
      buf = &texParams_;
      break;
    default:
      break;
    }
    if (buf) {
      e.buffer = buf->buffer;
      e.offset = 0;
      e.size = buf->size;
    }
    entries.append(e);
  }
  WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
  bgd.layout = bgLayout_;
  bgd.entryCount = entries.size();
  bgd.entries = entries.data();
  return wgpuDeviceCreateBindGroup(ctx_->device, &bgd);
}

bool WgpuBrushComputeDispatch::dab(const ComputeBrushUniforms &brushU,
                                   const ComputeCtxUniforms &ctxU,
                                   const uint32_t *uniqueVerts,
                                   int uniqueVertCount,
                                   const ComputeNodeMeta *nodes,
                                   int nodeCount,
                                   const float *falloffLut,
                                   const ComputeStrokeSample *strokePath,
                                   int strokeCount)
{
  if (nodeCount == 0)
    return true;
  const WGPUBufferUsage ro = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
  const WGPUBufferUsage uni = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
  if (!ensureBuf(unique_, uint64_t(uniqueVertCount) * sizeof(uint32_t), ro) ||
      !ensureBuf(nodes_, uint64_t(nodeCount) * sizeof(ComputeNodeMeta), ro) ||
      !ensureBuf(brushU_, sizeof(ComputeBrushUniforms), uni) ||
      !ensureBuf(ctxU_, sizeof(ComputeCtxUniforms), uni) ||
      !ensureBuf(falloff_, 256 * sizeof(float), uni) ||
      !ensureBuf(stroke_,
                 uint64_t(strokeCount < 1 ? 1 : strokeCount) *
                     sizeof(ComputeStrokeSample),
                 ro))
  {
    return false;
  }
  // A spliced kernel declares kTexParamsBinding; if the host never uploaded a
  // slab (defensive — the driver always does), bind a 1-float dummy rather
  // than a null buffer, which would fail bind-group creation.
  if (hasBinding(brush::kTexParamsBinding) && !ensureBuf(texParams_, sizeof(float), ro)) {
    return false;
  }

  wgpuQueueWriteBuffer(ctx_->queue,
                       unique_.buffer,
                       0,
                       uniqueVerts,
                       size_t(uniqueVertCount) * sizeof(uint32_t));
  wgpuQueueWriteBuffer(
      ctx_->queue, nodes_.buffer, 0, nodes, size_t(nodeCount) * sizeof(ComputeNodeMeta));
  wgpuQueueWriteBuffer(
      ctx_->queue, brushU_.buffer, 0, &brushU, sizeof(ComputeBrushUniforms));
  wgpuQueueWriteBuffer(ctx_->queue, ctxU_.buffer, 0, &ctxU, sizeof(ComputeCtxUniforms));
  wgpuQueueWriteBuffer(ctx_->queue, falloff_.buffer, 0, falloffLut, 256 * sizeof(float));
  if (strokeCount > 0) {
    wgpuQueueWriteBuffer(ctx_->queue,
                         stroke_.buffer,
                         0,
                         strokePath,
                         size_t(strokeCount) * sizeof(ComputeStrokeSample));
  }

  WGPUBindGroup bindGroup = buildBindGroup();
  if (!bindGroup)
    return false;

  WGPUCommandEncoderDescriptor ced = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(ctx_->device, &ced);

  // Jacobi snapshot: capture pre-dab positions so for_neighbor reads a
  // consistent state (co_buf is written in place by the dispatch). The copy
  // and compute pass share one command buffer, so WebGPU orders the dependency.
  if (hasNeighbors_) {
    wgpuCommandEncoderCopyBufferToBuffer(
        enc, co_.buffer, 0, coPrev_.buffer, 0, uint64_t(vertCount_) * kVec3Stride);
  }

  WGPUComputePassDescriptor cpd = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
  WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, &cpd);
  wgpuComputePassEncoderSetPipeline(pass, pipeline_);
  wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
  wgpuComputePassEncoderDispatchWorkgroups(pass, uint32_t(nodeCount), 1, 1);
  wgpuComputePassEncoderEnd(pass);
  wgpuComputePassEncoderRelease(pass);

  WGPUCommandBufferDescriptor cbd = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, &cbd);
  wgpuQueueSubmit(ctx_->queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);
  wgpuBindGroupRelease(bindGroup);
  // No CPU drain here: all dabs submit to one queue, which executes them in
  // order, so dab N+1's read of co_ (and the coPrev_ snapshot) already sees
  // dab N's result. Forcing a host wait per dab is what serialized a catch-up
  // burst of dabs into the periodic hitch. The next readback (per-frame flush
  // or endStroke) drains.
  return true;
}

bool WgpuBrushComputeDispatch::readbackBuffer(const Buf &src,
                                              int n,
                                              bool vec3,
                                              float *out)
{
  if (n <= 0 || !out || !src.buffer)
    return true;
  const uint64_t stride = vec3 ? kVec3Stride : sizeof(float);
  const uint64_t bytes = uint64_t(n) * stride;

  // Reuse one persistent staging buffer instead of allocating per readback —
  // see readback_'s declaration for why the churn hitches the renderer.
  if (!ensureBuf(readback_, bytes, WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead)) {
    return false;
  }
  WGPUBuffer staging = readback_.buffer;

  WGPUCommandEncoderDescriptor ced = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(ctx_->device, &ced);
  wgpuCommandEncoderCopyBufferToBuffer(enc, src.buffer, 0, staging, 0, bytes);
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
  wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, bytes, mci);
  while (!mreq.done) {
    drain(ctx_);
  }

  bool ok = false;
  if (mreq.ok) {
    const auto *mapped =
        static_cast<const float *>(wgpuBufferGetConstMappedRange(staging, 0, bytes));
    if (mapped) {
      if (vec3) {
        for (int i = 0; i < n; i++) {
          out[i * 3 + 0] = mapped[i * 4 + 0];
          out[i * 3 + 1] = mapped[i * 4 + 1];
          out[i * 3 + 2] = mapped[i * 4 + 2];
        }
      } else {
        std::memcpy(out, mapped, size_t(n) * sizeof(float));
      }
      ok = true;
    }
    wgpuBufferUnmap(staging);
  }
  return ok;
}

bool WgpuBrushComputeDispatch::endStroke(float *coOut, float *noOut, float *maskOut)
{
  bool ok = true;
  if (coOut)
    ok = readbackBuffer(co_, vertCount_, true, coOut) && ok;
  if (noOut)
    ok = readbackBuffer(no_, vertCount_, true, noOut) && ok;
  if (maskOut)
    ok = readbackBuffer(mask_, vertCount_, false, maskOut) && ok;
  return ok;
}

bool WgpuBrushComputeDispatch::readbackVerts(const uint32_t *verts,
                                             int count,
                                             float *coOut,
                                             float *noOut)
{
  if (count <= 0)
    return true;
  // WebGPU has no scatter readback; pull the whole buffer and gather. Cheap on
  // the demo meshes the debug app uses, and only the interactive path calls it.
  litestl::util::Vector<float> full;
  if (coOut) {
    full.resize(size_t(vertCount_) * 3);
    if (!readbackBuffer(co_, vertCount_, true, full.data()))
      return false;
    for (int i = 0; i < count; i++) {
      uint32_t v = verts[i];
      coOut[i * 3 + 0] = full[v * 3 + 0];
      coOut[i * 3 + 1] = full[v * 3 + 1];
      coOut[i * 3 + 2] = full[v * 3 + 2];
    }
  }
  if (noOut) {
    full.resize(size_t(vertCount_) * 3);
    if (!readbackBuffer(no_, vertCount_, true, full.data()))
      return false;
    for (int i = 0; i < count; i++) {
      uint32_t v = verts[i];
      noOut[i * 3 + 0] = full[v * 3 + 0];
      noOut[i * 3 + 1] = full[v * 3 + 1];
      noOut[i * 3 + 2] = full[v * 3 + 2];
    }
  }
  return true;
}

} // namespace sculptcore::webgpu
