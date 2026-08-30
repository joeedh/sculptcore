#include "wgpu_backend.h"
#include "wgpu_context.h"

#include "gpu/batch.h"
#include "gpu/command.h"
#include "gpu/manager.h"
#include "gpu/shader.h"
#include "gpu/uniform_link.h"
#include "gpu/vbo.h"

#include "litestl/util/vector.h"

#include <cstdio>
#include <cstring>

namespace sculptcore::webgpu {

using namespace sculptcore::gpu;

static WGPUStringView strView(const char *s)
{
  WGPUStringView v{};
  v.data = s;
  v.length = s ? std::strlen(s) : 0;
  return v;
}

static UniformBlockInstance *
findInstanceByName(const litestl::util::Vector<UniformBlockInstance *> &blocks,
                   const litestl::util::string &name)
{
  for (auto *inst : blocks) {
    if (inst && inst->def && inst->def->name == name) {
      return inst;
    }
  }
  return nullptr;
}

/* Backward-compat shim mirroring vulkan::writeFromDrawUniforms: scatter `u`'s
 * fields into the std140 blob `dst` by resolved offset. */
static void writeFromDrawUniforms(const UniformBlockDef *block,
                                  const DrawUniforms &u,
                                  void *dst,
                                  size_t maxBytes)
{
  std::memset(dst, 0, maxBytes);
  for (size_t i = 0; i < block->fields.size(); i++) {
    const UniformDefBase *f = block->fields[i];
    uint32_t off = block->fieldOffsets[i];
    if (off >= maxBytes) {
      continue;
    }
    uint8_t *p = static_cast<uint8_t *>(dst) + off;
    size_t avail = maxBytes - off;
    const char *nm = f->name.c_str();
    if (strcmp(nm, "drawMatrix") == 0 && avail >= sizeof(u.drawMatrix)) {
      std::memcpy(p, &u.drawMatrix, sizeof(u.drawMatrix));
    } else if (strcmp(nm, "normalMatrix") == 0 && avail >= sizeof(u.normalMatrix)) {
      std::memcpy(p, &u.normalMatrix, sizeof(u.normalMatrix));
    } else if (strcmp(nm, "uColor") == 0 && avail >= sizeof(u.uColor)) {
      std::memcpy(p, &u.uColor, sizeof(u.uColor));
    }
  }
}

static WGPUVertexFormat attrFormatWgpu(GPUType type, int elemSize)
{
  if (type == GPUType::FLOAT32) {
    switch (elemSize) {
    case 2:
      return WGPUVertexFormat_Float32x2;
    case 3:
      return WGPUVertexFormat_Float32x3;
    case 4:
      return WGPUVertexFormat_Float32x4;
    }
  }
  /* No other types currently used by the spatial shaders. */
  return WGPUVertexFormat_Float32x3;
}

WebGpuBackend::WebGpuBackend(GPUManager *mgr,
                             WgpuContext *ctx,
                             WGPUTextureFormat colorFormat)
    : mgr_(mgr), ctx_(ctx), colorFormat_(colorFormat)
{
  if (mgr_) {
    mgr_->addObserver(this);
  }
}

WebGpuBackend::~WebGpuBackend()
{
  if (mgr_) {
    mgr_->removeObserver(this);
  }
  invalidate();
}

void WebGpuBackend::onBufferDestroyed(Buffer *buf)
{
  BufferEntry *entry = buffer_cache_.lookup_ptr(buf);
  if (!entry) {
    return;
  }
  if (entry->buffer) {
    wgpuBufferRelease(entry->buffer);
  }
  buffer_cache_.remove(buf);
}

void WebGpuBackend::invalidate()
{
  litestl::util::Vector<Buffer *> bufKeys;
  for (auto &kv : buffer_cache_) {
    if (kv.value.buffer)
      wgpuBufferRelease(kv.value.buffer);
    bufKeys.append(kv.key);
  }
  for (auto *k : bufKeys)
    buffer_cache_.remove(k);

  litestl::util::Vector<ShaderDef *> pipeKeys;
  for (auto &kv : pipeline_cache_) {
    auto &e = kv.value;
    if (e.bindGroup)
      wgpuBindGroupRelease(e.bindGroup);
    if (e.ubo)
      wgpuBufferRelease(e.ubo);
    if (e.pipeline)
      wgpuRenderPipelineRelease(e.pipeline);
    if (e.layout)
      wgpuPipelineLayoutRelease(e.layout);
    if (e.bgLayout)
      wgpuBindGroupLayoutRelease(e.bgLayout);
    if (e.shaderModule)
      wgpuShaderModuleRelease(e.shaderModule);
    pipeKeys.append(kv.key);
  }
  for (auto *k : pipeKeys)
    pipeline_cache_.remove(k);

  if (depthView_) {
    wgpuTextureViewRelease(depthView_);
    depthView_ = nullptr;
  }
  if (depthTexture_) {
    wgpuTextureRelease(depthTexture_);
    depthTexture_ = nullptr;
  }
  depthW_ = depthH_ = 0;
}

WebGpuBackend::BufferEntry &WebGpuBackend::ensureBuffer(Buffer *buf)
{
  static BufferEntry empty;
  if (!buf || !buf->data || buf->size <= 0)
    return empty;

  BufferEntry *entry = buffer_cache_.lookup_ptr(buf);
  uint64_t bytes = uint64_t(buf->size) * buf->elemsize * gpu_sizeof(buf->type);
  /* WebGPU buffer sizes (and writeBuffer ranges) must be 4-byte multiples. */
  uint64_t paddedBytes = (bytes + 3u) & ~uint64_t(3u);

  if (!entry || !entry->buffer || entry->size < paddedBytes) {
    if (entry && entry->buffer) {
      wgpuBufferRelease(entry->buffer);
    }
    WGPUBufferUsage usage =
        ((buf->target == BUFFER_INDEX) ? WGPUBufferUsage_Index : WGPUBufferUsage_Vertex) |
        WGPUBufferUsage_CopyDst;
    if (buf->gpu_storage) {
      usage |= WGPUBufferUsage_Storage;
    }
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.usage = usage;
    bd.size = paddedBytes;
    BufferEntry e;
    e.buffer = wgpuDeviceCreateBuffer(ctx_->device, &bd);
    e.size = paddedBytes;
    buffer_cache_[buf] = e;
    entry = buffer_cache_.lookup_ptr(buf);
    buf->markDirtyAll();
  }

  /* gpu_owned buffers are produced GPU-side; never clobber with host data. */
  if (buf->update_buffer && !buf->gpu_owned && entry && entry->buffer) {
    /* writeBuffer copies `bytes` (4-multiple) from host `data`. */
    wgpuQueueWriteBuffer(ctx_->queue, entry->buffer, 0, buf->data, size_t(paddedBytes));
    buf->update_buffer = false;
    buf->uploaded = true;
  }
  return *entry;
}

WebGpuBackend::PipelineEntry *WebGpuBackend::ensurePipeline(ShaderDef *def)
{
  if (!def)
    return nullptr;
  PipelineEntry *cached = pipeline_cache_.lookup_ptr(def);
  if (cached && cached->pipeline)
    return cached;
  if (def->wgslSource.size() == 0) {
    fprintf(stderr, "WebGpuBackend: shader '%s' has no WGSL\n", def->name.c_str());
    return nullptr;
  }
  if (def->uniforms.size() == 0) {
    fprintf(
        stderr, "WebGpuBackend: shader '%s' has no uniform blocks\n", def->name.c_str());
    return nullptr;
  }

  PipelineEntry e;

  WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
  wgsl.code = strView(def->wgslSource.c_str());
  WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  smd.nextInChain = &wgsl.chain;
  e.shaderModule = wgpuDeviceCreateShaderModule(ctx_->device, &smd);
  if (!e.shaderModule) {
    fprintf(stderr,
            "WebGpuBackend: shader module creation failed for '%s'\n",
            def->name.c_str());
    return nullptr;
  }

  UniformBlockDef *block0 = def->uniforms[0];
  if (block0->packedBytes == 0) {
    linkShaderDef(def);
  }

  /* Bind group layout: single uniform buffer at block0->binding. */
  WGPUBindGroupLayoutEntry bgle = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
  bgle.binding = block0->binding;
  bgle.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
  bgle.buffer.type = WGPUBufferBindingType_Uniform;
  WGPUBindGroupLayoutDescriptor bgld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
  bgld.entryCount = 1;
  bgld.entries = &bgle;
  e.bgLayout = wgpuDeviceCreateBindGroupLayout(ctx_->device, &bgld);

  WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
  pld.bindGroupLayoutCount = 1;
  pld.bindGroupLayouts = &e.bgLayout;
  e.layout = wgpuDeviceCreatePipelineLayout(ctx_->device, &pld);

  /* Uniform buffer + bind group. */
  e.uboSize = (uint64_t(block0->packedBytes) + 15u) & ~uint64_t(15u);
  WGPUBufferDescriptor ubd = WGPU_BUFFER_DESCRIPTOR_INIT;
  ubd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
  ubd.size = e.uboSize;
  e.ubo = wgpuDeviceCreateBuffer(ctx_->device, &ubd);

  WGPUBindGroupEntry bge = WGPU_BIND_GROUP_ENTRY_INIT;
  bge.binding = block0->binding;
  bge.buffer = e.ubo;
  bge.offset = 0;
  bge.size = e.uboSize;
  WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
  bgd.layout = e.bgLayout;
  bgd.entryCount = 1;
  bgd.entries = &bge;
  e.bindGroup = wgpuDeviceCreateBindGroup(ctx_->device, &bgd);

  /* Vertex input: one buffer per attribute, location matches order. */
  const int nattr = int(def->attrs.size());
  litestl::util::Vector<WGPUVertexAttribute> vattrs;
  litestl::util::Vector<WGPUVertexBufferLayout> vbufs;
  vattrs.resize(nattr);
  vbufs.resize(nattr);
  for (int i = 0; i < nattr; i++) {
    vattrs[i] = WGPU_VERTEX_ATTRIBUTE_INIT;
    vattrs[i].format = attrFormatWgpu(def->attrs[i].type, def->attrs[i].elemSize);
    vattrs[i].offset = 0;
    vattrs[i].shaderLocation = uint32_t(i);

    vbufs[i] = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vbufs[i].stepMode = WGPUVertexStepMode_Vertex;
    vbufs[i].arrayStride =
        uint64_t(def->attrs[i].elemSize) * gpu_sizeof(def->attrs[i].type);
    vbufs[i].attributeCount = 1;
    vbufs[i].attributes = &vattrs[i];
  }

  /* Natural topology: line shader has no "normal" attribute, mesh shader does.
   * Same heuristic as vulkan::ensurePipeline. */
  bool naturallyLines = true;
  for (const auto &a : def->attrs) {
    if (strcmp(a.name, "normal") == 0) {
      naturallyLines = false;
      break;
    }
  }

  WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
  colorTarget.format = colorFormat_;
  colorTarget.writeMask = WGPUColorWriteMask_All;

  WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
  fs.module = e.shaderModule;
  fs.entryPoint = strView("fs_main");
  fs.targetCount = 1;
  fs.targets = &colorTarget;

  WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
  depth.format = depthFormat_;
  depth.depthWriteEnabled = WGPUOptionalBool_True;
  depth.depthCompare = WGPUCompareFunction_LessEqual;

  WGPURenderPipelineDescriptor rpd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  rpd.label = strView(def->name.c_str());
  rpd.layout = e.layout;
  rpd.vertex.module = e.shaderModule;
  rpd.vertex.entryPoint = strView("vs_main");
  rpd.vertex.bufferCount = uint32_t(vbufs.size());
  rpd.vertex.buffers = vbufs.data();
  rpd.primitive.topology = naturallyLines ? WGPUPrimitiveTopology_LineList
                                          : WGPUPrimitiveTopology_TriangleList;
  rpd.primitive.frontFace = WGPUFrontFace_CCW;
  rpd.primitive.cullMode = WGPUCullMode_None;
  rpd.depthStencil = &depth;
  rpd.fragment = &fs;

  e.pipeline = wgpuDeviceCreateRenderPipeline(ctx_->device, &rpd);
  if (!e.pipeline) {
    fprintf(
        stderr, "WebGpuBackend: failed to create pipeline for '%s'\n", def->name.c_str());
    if (e.bindGroup)
      wgpuBindGroupRelease(e.bindGroup);
    if (e.ubo)
      wgpuBufferRelease(e.ubo);
    if (e.layout)
      wgpuPipelineLayoutRelease(e.layout);
    if (e.bgLayout)
      wgpuBindGroupLayoutRelease(e.bgLayout);
    if (e.shaderModule)
      wgpuShaderModuleRelease(e.shaderModule);
    return nullptr;
  }

  pipeline_cache_[def] = e;
  return pipeline_cache_.lookup_ptr(def);
}

void WebGpuBackend::issue(DrawBatch *batch, DrawCommand *cmd, const DrawUniforms &u)
{
  if (!cmd || !cmd->shader || !pass_)
    return;
  PipelineEntry *pe = ensurePipeline(cmd->shader);
  if (!pe)
    return;

  /* Resolve the shader's first block against (cmd, batch); else synthesize. */
  UniformBlockDef *blockDef = cmd->shader->uniforms[0];
  UniformBlockInstance *inst = findInstanceByName(cmd->blocks, blockDef->name);
  if (!inst && batch) {
    inst = findInstanceByName(batch->blocks, blockDef->name);
  }
  if (inst && inst->data.size() >= blockDef->packedBytes && blockDef->packedBytes > 0) {
    wgpuQueueWriteBuffer(
        ctx_->queue, pe->ubo, 0, inst->data.data(), size_t(blockDef->packedBytes));
  } else {
    /* Stack scratch sized to the (16-aligned) UBO, scattered from `u`. */
    uint8_t scratch[256];
    size_t n = size_t(pe->uboSize);
    if (n > sizeof(scratch))
      n = sizeof(scratch);
    writeFromDrawUniforms(blockDef, u, scratch, n);
    wgpuQueueWriteBuffer(ctx_->queue, pe->ubo, 0, scratch, n);
  }

  wgpuRenderPassEncoderSetPipeline(pass_, pe->pipeline);
  wgpuRenderPassEncoderSetBindGroup(pass_, 0, pe->bindGroup, 0, nullptr);

  /* Bind one vertex buffer per attribute, declaration order. */
  const auto &shader_attrs = cmd->shader->attrs;
  int n = int(cmd->attrs.size());
  if (n > int(shader_attrs.size()))
    n = int(shader_attrs.size());
  for (int i = 0; i < n; i++) {
    auto &be = ensureBuffer(cmd->attrs[i]);
    if (!be.buffer) {
      return;
    }
    wgpuRenderPassEncoderSetVertexBuffer(pass_, uint32_t(i), be.buffer, 0, be.size);
  }

  uint32_t vcount = uint32_t(cmd->end - cmd->start);
  if (vcount > 0) {
    wgpuRenderPassEncoderDraw(pass_, vcount, 1, uint32_t(cmd->start), 0);
  }
}

bool WebGpuBackend::beginFrame(WgpuTarget &target, float r, float g, float b, float a)
{
  if (inFrame_ || !target.colorView || !target.depthView)
    return false;

  WGPUCommandEncoderDescriptor ced = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
  encoder_ = wgpuDeviceCreateCommandEncoder(ctx_->device, &ced);

  WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
  color.view = target.colorView;
  color.loadOp = WGPULoadOp_Clear;
  color.storeOp = WGPUStoreOp_Store;
  color.clearValue = {double(r), double(g), double(b), double(a)};

  WGPURenderPassDepthStencilAttachment depth =
      WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
  depth.view = target.depthView;
  depth.depthLoadOp = WGPULoadOp_Clear;
  depth.depthStoreOp = WGPUStoreOp_Store;
  depth.depthClearValue = 1.0f;

  WGPURenderPassDescriptor rpd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
  rpd.colorAttachmentCount = 1;
  rpd.colorAttachments = &color;
  rpd.depthStencilAttachment = &depth;
  pass_ = wgpuCommandEncoderBeginRenderPass(encoder_, &rpd);
  inFrame_ = true;
  return true;
}

void WebGpuBackend::endFrame()
{
  if (!inFrame_)
    return;
  wgpuRenderPassEncoderEnd(pass_);
  wgpuRenderPassEncoderRelease(pass_);
  pass_ = nullptr;

  WGPUCommandBuffer cb = wgpuCommandEncoderFinish(encoder_, nullptr);
  wgpuQueueSubmit(ctx_->queue, 1, &cb);
  wgpuCommandBufferRelease(cb);
  wgpuCommandEncoderRelease(encoder_);
  encoder_ = nullptr;
  inFrame_ = false;
}

void WebGpuBackend::ensureSurfaceDepth(int w, int h)
{
  if (depthTexture_ && depthW_ == w && depthH_ == h)
    return;
  if (depthView_) {
    wgpuTextureViewRelease(depthView_);
    depthView_ = nullptr;
  }
  if (depthTexture_) {
    wgpuTextureRelease(depthTexture_);
    depthTexture_ = nullptr;
  }

  WGPUTextureDescriptor dtd = WGPU_TEXTURE_DESCRIPTOR_INIT;
  dtd.usage = WGPUTextureUsage_RenderAttachment;
  dtd.dimension = WGPUTextureDimension_2D;
  dtd.size = {uint32_t(w), uint32_t(h), 1};
  dtd.format = depthFormat_;
  depthTexture_ = wgpuDeviceCreateTexture(ctx_->device, &dtd);
  depthView_ = wgpuTextureCreateView(depthTexture_, nullptr);
  depthW_ = w;
  depthH_ = h;
}

bool WebGpuBackend::beginFrameSurface(int w, int h, float r, float g, float b, float a)
{
  if (inFrame_ || !ctx_->surface)
    return false;

  WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
  wgpuSurfaceGetCurrentTexture(ctx_->surface, &st);
  if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
      st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
  {
    fprintf(
        stderr, "WebGpuBackend: surface getCurrentTexture status %d\n", int(st.status));
    return false;
  }
  surfaceTex_ = st.texture;
  surfaceView_ = wgpuTextureCreateView(surfaceTex_, nullptr);
  ensureSurfaceDepth(w, h);

  WGPUCommandEncoderDescriptor ced = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
  encoder_ = wgpuDeviceCreateCommandEncoder(ctx_->device, &ced);

  WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
  color.view = surfaceView_;
  color.loadOp = WGPULoadOp_Clear;
  color.storeOp = WGPUStoreOp_Store;
  color.clearValue = {double(r), double(g), double(b), double(a)};

  WGPURenderPassDepthStencilAttachment depth =
      WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
  depth.view = depthView_;
  depth.depthLoadOp = WGPULoadOp_Clear;
  depth.depthStoreOp = WGPUStoreOp_Store;
  depth.depthClearValue = 1.0f;

  WGPURenderPassDescriptor rpd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
  rpd.colorAttachmentCount = 1;
  rpd.colorAttachments = &color;
  rpd.depthStencilAttachment = &depth;
  pass_ = wgpuCommandEncoderBeginRenderPass(encoder_, &rpd);
  inFrame_ = true;
  return true;
}

bool WebGpuBackend::endFrameSurface()
{
  if (!inFrame_)
    return true;
  wgpuRenderPassEncoderEnd(pass_);
  wgpuRenderPassEncoderRelease(pass_);
  pass_ = nullptr;

  WGPUCommandBuffer cb = wgpuCommandEncoderFinish(encoder_, nullptr);
  wgpuQueueSubmit(ctx_->queue, 1, &cb);
  wgpuCommandBufferRelease(cb);
  wgpuCommandEncoderRelease(encoder_);
  encoder_ = nullptr;

#ifndef __EMSCRIPTEN__
  /* On the web the browser presents automatically when the callback returns;
   * wgpuSurfacePresent is a no-op there but harmless. */
  wgpuSurfacePresent(ctx_->surface);
#endif

  if (surfaceView_) {
    wgpuTextureViewRelease(surfaceView_);
    surfaceView_ = nullptr;
  }
  if (surfaceTex_) {
    wgpuTextureRelease(surfaceTex_);
    surfaceTex_ = nullptr;
  }
  inFrame_ = false;
  return true;
}

void WebGpuBackend::draw(DrawBatch *batch, const DrawUniforms &u)
{
  if (!batch || !inFrame_)
    return;
  for (DrawCommand *cmd : batch->commands) {
    issue(batch, cmd, u);
  }
}

} // namespace sculptcore::webgpu
