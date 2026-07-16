#include "vk_backend.h"
#include "vk_context.h"
#include "vk_swapchain.h"

#include "gpu/batch.h"
#include "gpu/command.h"
#include "gpu/manager.h"
#include "gpu/shader.h"
#include "gpu/uniform_link.h"
#include "gpu/vbo.h"

#include "litestl/util/vector.h"

#include <cstdio>
#include <cstring>

namespace sculptcore::vulkan {

using namespace sculptcore::gpu;

/* Find a uniform block instance by name in a vector of layer-owned instances. */
static UniformBlockInstance *findInstanceByName(
    const litestl::util::Vector<UniformBlockInstance *> &blocks,
    const litestl::util::string &name)
{
  for (auto *inst : blocks) {
    if (inst && inst->def && inst->def->name == name) {
      return inst;
    }
  }
  return nullptr;
}

/* Backward-compat shim: write `u`'s drawMatrix/normalMatrix/uColor into the
 * std140 blob `dst` (sized to `block->packedBytes`) at each field's resolved
 * offset. Fields with unrecognised names are left zero-initialised.
 *
 * Called when the caller hasn't yet migrated to attaching a
 * UniformBlockInstance to its DrawCommand/DrawBatch. Goes away when every
 * caller provides its own instance. */
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
    }
    else if (strcmp(nm, "normalMatrix") == 0 && avail >= sizeof(u.normalMatrix)) {
      std::memcpy(p, &u.normalMatrix, sizeof(u.normalMatrix));
    }
    else if (strcmp(nm, "uColor") == 0 && avail >= sizeof(u.uColor)) {
      std::memcpy(p, &u.uColor, sizeof(u.uColor));
    }
  }
}

static VkFormat attrFormatVk(GPUType type, int elemSize)
{
  if (type == GPUType::FLOAT32) {
    switch (elemSize) {
    case 1: return VK_FORMAT_R32_SFLOAT;
    case 2: return VK_FORMAT_R32G32_SFLOAT;
    case 3: return VK_FORMAT_R32G32B32_SFLOAT;
    case 4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
  }
  /* No other types currently used by the spatial shaders. */
  return VK_FORMAT_UNDEFINED;
}

static VkPrimitiveTopology topologyVk(GPUCmdType t)
{
  switch (t) {
  case GPUCmdType::DRAW_TRIS: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  case GPUCmdType::DRAW_TRI_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  case GPUCmdType::DRAW_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
  case GPUCmdType::DRAW_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
  }
  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VulkanBackend::VulkanBackend(GPUManager *mgr, VkContext *ctx, VkRenderPass rp)
    : mgr_(mgr), ctx_(ctx), renderPass_(rp)
{
  if (mgr_) {
    mgr_->addObserver(this);
  }
}

VulkanBackend::~VulkanBackend()
{
  if (mgr_) {
    mgr_->removeObserver(this);
  }
  invalidate();
}

void VulkanBackend::onBufferDestroyed(Buffer *buf)
{
  BufferEntry *entry = buffer_cache_.lookup_ptr(buf);
  if (!entry) {
    return;
  }
  if (entry->buffer != VK_NULL_HANDLE || entry->memory != VK_NULL_HANDLE) {
    deferred_buffers_.append({entry->buffer, entry->memory});
  }
  buffer_cache_.remove(buf);
}

void VulkanBackend::drainDeferredBuffers_()
{
  if (deferred_buffers_.size() == 0 || !ctx_ || !ctx_->device) {
    deferred_buffers_.clear();
    return;
  }
  for (auto &p : deferred_buffers_) {
    if (p.buffer) vkDestroyBuffer(ctx_->device, p.buffer, nullptr);
    if (p.memory) vkFreeMemory(ctx_->device, p.memory, nullptr);
  }
  deferred_buffers_.clear();
}

void VulkanBackend::invalidate()
{
  if (!ctx_) return;
  VkDevice d = ctx_->device;
  if (!d) return;

  /* Make sure the queue is idle before destroying any in-flight resources. */
  vkDeviceWaitIdle(d);

  /* Anything sitting in the deferred-destroy queue from in-frame
   * destroyBatch / ensureBuffer growth is now safe to release. */
  drainDeferredBuffers_();

  /* litestl::util::Map has no public clear(); collect keys and remove them
   * after destroying the handles. ensureBuffer/ensurePipeline both insert
   * fresh entries on miss, so re-population is automatic. */
  litestl::util::Vector<sculptcore::gpu::Buffer *> bufKeys;
  for (auto &kv : buffer_cache_) {
    auto &e = kv.value;
    if (e.buffer) vkDestroyBuffer(d, e.buffer, nullptr);
    if (e.memory) vkFreeMemory(d, e.memory, nullptr);
    bufKeys.append(kv.key);
  }
  for (auto *k : bufKeys) buffer_cache_.remove(k);

  litestl::util::Vector<sculptcore::gpu::ShaderDef *> pipeKeys;
  for (auto &kv : pipeline_cache_) {
    auto &e = kv.value;
    if (e.pipeline) vkDestroyPipeline(d, e.pipeline, nullptr);
    if (e.layout) vkDestroyPipelineLayout(d, e.layout, nullptr);
    if (e.dsLayout) vkDestroyDescriptorSetLayout(d, e.dsLayout, nullptr);
    if (e.shaderModule) vkDestroyShaderModule(d, e.shaderModule, nullptr);
    if (e.uboMapped) vkUnmapMemory(d, e.uboMemory);
    if (e.uboBuffer) vkDestroyBuffer(d, e.uboBuffer, nullptr);
    if (e.uboMemory) vkFreeMemory(d, e.uboMemory, nullptr);
    /* descriptorSet is freed via descriptorPool reset (or pool destroy). */
    pipeKeys.append(kv.key);
  }
  for (auto *k : pipeKeys) pipeline_cache_.remove(k);
}

VulkanBackend::BufferEntry &VulkanBackend::ensureBuffer(Buffer *buf)
{
  static BufferEntry empty;
  if (!buf || !buf->data || buf->size <= 0) return empty;

  BufferEntry *entry = buffer_cache_.lookup_ptr(buf);
  VkDeviceSize bytes = VkDeviceSize(buf->size) * buf->elemsize * gpu_sizeof(buf->type);

  /* A buffer cached from an earlier draw lacks STORAGE usage; if gpu_storage
   * was set since (the GPU-resident stroke path flips it on pos/nor), the
   * existing VkBuffer can't be bound as a storage descriptor — recreate it. */
  bool needStorage = buf->gpu_storage;
  if (!entry || !entry->buffer || entry->size < bytes ||
      (needStorage && !entry->storage)) {
    if (entry && (entry->buffer || entry->memory)) {
      /* Defer destruction — the old VkBuffer may still be referenced by the
       * currently-recording command buffer (we may have bound it in an earlier
       * issue() call this frame) or by an in-flight one. drainDeferredBuffers_()
       * runs after vkQueueWaitIdle at the end of the next frame. */
      deferred_buffers_.append({entry->buffer, entry->memory});
    }
    BufferEntry e;
    e.size = bytes;
    e.hostVisible = true;
    e.storage = needStorage;

    VkBufferUsageFlags usage = (buf->target == BUFFER_INDEX)
                                   ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                                   : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (buf->gpu_storage) {
      usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(ctx_->device, &bci, nullptr, &e.buffer);

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(ctx_->device, e.buffer, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = ctx_->findMemoryType(
        mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(ctx_->device, &mai, nullptr, &e.memory);
    vkBindBufferMemory(ctx_->device, e.buffer, e.memory, 0);
    buffer_cache_[buf] = e;
    entry = buffer_cache_.lookup_ptr(buf);
    buf->markDirtyAll();
  }

  /* gpu_owned buffers are filled GPU-side (compute scatter); never clobber
   * them with stale host data. The VkBuffer is created/grown above regardless. */
  if (buf->update_buffer && !buf->gpu_owned && entry && entry->memory) {
    void *p = nullptr;
    vkMapMemory(ctx_->device, entry->memory, 0, bytes, 0, &p);
    memcpy(p, buf->data, size_t(bytes));
    vkUnmapMemory(ctx_->device, entry->memory);
    buf->update_buffer = false;
    buf->uploaded = true;
  }
  return *entry;
}

VkBuffer VulkanBackend::ensureStorageVkBuffer(Buffer *buf)
{
  if (!buf) return VK_NULL_HANDLE;
  buf->gpu_storage = true;
  return ensureBuffer(buf).buffer;
}

VulkanBackend::PipelineEntry *VulkanBackend::ensurePipeline(ShaderDef *def)
{
  if (!def) return nullptr;
  PipelineEntry *cached = pipeline_cache_.lookup_ptr(def);
  if (cached && cached->pipeline) return cached;
  if (!def->spirv || def->spirvSize == 0) {
    fprintf(stderr, "VulkanBackend: shader '%s' has no SPIR-V\n", def->name.c_str());
    return nullptr;
  }

  PipelineEntry e;

  VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = def->spirvSize * sizeof(uint32_t);
  smci.pCode = def->spirv;
  if (vkCreateShaderModule(ctx_->device, &smci, nullptr, &e.shaderModule) != VK_SUCCESS) {
    return nullptr;
  }

  /* Build a descriptor set layout from the shader's first uniform block.
   * Current shaders declare exactly one block ("DefaultBlock") at
   * (set=0, binding=0); the layout reflects whatever `block->binding`
   * the link pass stamped. */
  if (def->uniforms.size() == 0) {
    fprintf(stderr,
            "VulkanBackend: shader '%s' has no uniform blocks\n",
            def->name.c_str());
    return nullptr;
  }
  UniformBlockDef *block0 = def->uniforms[0];
  if (block0->packedBytes == 0) {
    /* Shader wasn't linked at construction — do it lazily. Idempotent. */
    linkShaderDef(def);
  }
  VkDescriptorSetLayoutBinding b{};
  b.binding = block0->binding;
  b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  b.descriptorCount = 1;
  b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo dsci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dsci.bindingCount = 1;
  dsci.pBindings = &b;
  vkCreateDescriptorSetLayout(ctx_->device, &dsci, nullptr, &e.dsLayout);

  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &e.dsLayout;
  vkCreatePipelineLayout(ctx_->device, &plci, nullptr, &e.layout);

  /* Uniform buffer (host-visible coherent), sized to the linked block. */
  e.uboSize = block0->packedBytes;
  VkBufferCreateInfo ubi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ubi.size = e.uboSize;
  ubi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  ubi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  vkCreateBuffer(ctx_->device, &ubi, nullptr, &e.uboBuffer);
  VkMemoryRequirements umr;
  vkGetBufferMemoryRequirements(ctx_->device, e.uboBuffer, &umr);
  VkMemoryAllocateInfo umai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  umai.allocationSize = umr.size;
  umai.memoryTypeIndex = ctx_->findMemoryType(
      umr.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  vkAllocateMemory(ctx_->device, &umai, nullptr, &e.uboMemory);
  vkBindBufferMemory(ctx_->device, e.uboBuffer, e.uboMemory, 0);
  vkMapMemory(ctx_->device, e.uboMemory, 0, e.uboSize, 0, &e.uboMapped);

  /* Descriptor set. */
  VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = ctx_->descriptorPool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &e.dsLayout;
  vkAllocateDescriptorSets(ctx_->device, &dsai, &e.descriptorSet);

  VkDescriptorBufferInfo dbi{};
  dbi.buffer = e.uboBuffer;
  dbi.offset = 0;
  dbi.range = e.uboSize;
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = e.descriptorSet;
  w.dstBinding = block0->binding;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  w.pBufferInfo = &dbi;
  vkUpdateDescriptorSets(ctx_->device, 1, &w, 0, nullptr);

  /* Vertex input: one binding per attribute, location matches order. */
  const int nattr = int(def->attrs.size());
  litestl::util::Vector<VkVertexInputBindingDescription> vbinds;
  litestl::util::Vector<VkVertexInputAttributeDescription> vattrs;
  vbinds.resize(nattr);
  vattrs.resize(nattr);
  for (int i = 0; i < nattr; i++) {
    vbinds[i].binding = uint32_t(i);
    vbinds[i].stride = uint32_t(def->attrs[i].elemSize) * gpu_sizeof(def->attrs[i].type);
    vbinds[i].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    vattrs[i].location = uint32_t(i);
    vattrs[i].binding = uint32_t(i);
    vattrs[i].format = attrFormatVk(def->attrs[i].type, def->attrs[i].elemSize);
    vattrs[i].offset = 0;
  }

  VkPipelineVertexInputStateCreateInfo vi{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = uint32_t(vbinds.size());
  vi.pVertexBindingDescriptions = vbinds.data();
  vi.vertexAttributeDescriptionCount = uint32_t(vattrs.size());
  vi.pVertexAttributeDescriptions = vattrs.data();

  /* Topology comes from the DrawCommand type. We bake one pipeline per shader
   * for now, defaulting to the shader's "natural" topology — line shader has
   * no normal attribute, mesh shader does. Refactor to dynamic topology when
   * a shader is used at multiple primitive types in one frame. */
  bool naturallyLines = true;
  for (const auto &a : def->attrs) {
    if (strcmp(a.name, "normal") == 0) {
      naturallyLines = false;
      break;
    }
  }
  VkPipelineInputAssemblyStateCreateInfo ia{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = naturallyLines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                               : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo vps{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vps.viewportCount = 1;
  vps.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rs{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo ms{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo ds{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_TRUE;
  ds.depthWriteEnable = VK_TRUE;
  ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

  VkPipelineColorBlendAttachmentState cba{};
  cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &cba;

  VkDynamicState dyns[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dy{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = 2;
  dy.pDynamicStates = dyns;

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = e.shaderModule;
  stages[0].pName = "vs_main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = e.shaderModule;
  stages[1].pName = "fs_main";

  VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gpci.stageCount = 2;
  gpci.pStages = stages;
  gpci.pVertexInputState = &vi;
  gpci.pInputAssemblyState = &ia;
  gpci.pViewportState = &vps;
  gpci.pRasterizationState = &rs;
  gpci.pMultisampleState = &ms;
  gpci.pDepthStencilState = &ds;
  gpci.pColorBlendState = &cb;
  gpci.pDynamicState = &dy;
  gpci.layout = e.layout;
  gpci.renderPass = renderPass_;
  gpci.subpass = 0;
  if (vkCreateGraphicsPipelines(ctx_->device, VK_NULL_HANDLE, 1, &gpci, nullptr, &e.pipeline) !=
      VK_SUCCESS) {
    fprintf(stderr, "VulkanBackend: failed to create pipeline for '%s'\n", def->name.c_str());
    return nullptr;
  }

  pipeline_cache_[def] = e;
  return pipeline_cache_.lookup_ptr(def);
}

void VulkanBackend::issue(DrawBatch *batch, DrawCommand *cmd, const DrawUniforms &u)
{
  if (!cmd || !cmd->shader || !activeCb_) return;
  PipelineEntry *pe = ensurePipeline(cmd->shader);
  if (!pe) return;

  /* Resolve the shader's first block against (cmd, batch). If neither layer
   * provides an instance, synthesize one from `u` for the duration of the
   * draw — backward-compat shim until callers attach their own. */
  UniformBlockDef *blockDef = cmd->shader->uniforms[0];
  UniformBlockInstance *inst = findInstanceByName(cmd->blocks, blockDef->name);
  if (!inst && batch) {
    inst = findInstanceByName(batch->blocks, blockDef->name);
  }
  if (inst && inst->data.size() == size_t(pe->uboSize)) {
    memcpy(pe->uboMapped, inst->data.data(), size_t(pe->uboSize));
  }
  else {
    writeFromDrawUniforms(blockDef, u, pe->uboMapped, size_t(pe->uboSize));
  }

  vkCmdBindPipeline(activeCb_, VK_PIPELINE_BIND_POINT_GRAPHICS, pe->pipeline);
  vkCmdBindDescriptorSets(activeCb_,
                          VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pe->layout,
                          0,
                          1,
                          &pe->descriptorSet,
                          0,
                          nullptr);

  /* Bind vertex buffers, one per attribute, in declaration order. */
  const auto &shader_attrs = cmd->shader->attrs;
  int n = int(cmd->attrs.size());
  if (n > int(shader_attrs.size())) n = int(shader_attrs.size());
  litestl::util::Vector<VkBuffer> vbufs;
  litestl::util::Vector<VkDeviceSize> voffs;
  vbufs.resize(n);
  voffs.resize(n);
  bool allOk = true;
  for (int i = 0; i < n; i++) {
    auto &be = ensureBuffer(cmd->attrs[i]);
    if (!be.buffer) {
      allOk = false;
      break;
    }
    vbufs[i] = be.buffer;
    voffs[i] = 0;
  }
  if (!allOk) return;
  if (n > 0) {
    vkCmdBindVertexBuffers(activeCb_, 0, uint32_t(n), vbufs.data(), voffs.data());
  }

  /* The pipeline was baked with the shader's natural topology (tris for mesh,
   * lines for line shader). If the DrawCommand requests a different prim, we
   * just trust the pipeline; topologyVk() exists for future split. */
  (void)topologyVk(cmd->type);

  uint32_t vcount = uint32_t(cmd->end - cmd->start);
  if (vcount > 0) {
    vkCmdDraw(activeCb_, vcount, 1, uint32_t(cmd->start), 0);
  }
}

bool VulkanBackend::beginFrame(OffscreenTarget &target, float r, float g, float b, float a)
{
  if (inFrame_) return false;
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = ctx_->commandPool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(ctx_->device, &ai, &activeCb_) != VK_SUCCESS) return false;

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(activeCb_, &bi);
  target.beginRenderPass(activeCb_, r, g, b, a);
  inFrame_ = true;
  return true;
}

void VulkanBackend::endFrame()
{
  if (!inFrame_) return;
  vkCmdEndRenderPass(activeCb_);
  vkEndCommandBuffer(activeCb_);

  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &activeCb_;
  vkQueueSubmit(ctx_->graphicsQueue, 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(ctx_->graphicsQueue);
  vkFreeCommandBuffers(ctx_->device, ctx_->commandPool, 1, &activeCb_);
  activeCb_ = VK_NULL_HANDLE;
  inFrame_ = false;

  /* Command buffer just freed; any VkBuffer it referenced is now safe to
   * destroy. Drain anything queued during the frame. */
  drainDeferredBuffers_();
}

bool VulkanBackend::beginFrameSwapchain(Swapchain &sw, uint32_t imageIndex,
                                        float r, float g, float b, float a)
{
  if (inFrame_) return false;
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = ctx_->commandPool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(ctx_->device, &ai, &activeCb_) != VK_SUCCESS) return false;

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(activeCb_, &bi);
  sw.beginRenderPass(activeCb_, imageIndex, r, g, b, a);
  inFrame_ = true;
  return true;
}

bool VulkanBackend::endFrameSwapchain(Swapchain &sw, uint32_t imageIndex)
{
  if (!inFrame_) return true;
  vkCmdEndRenderPass(activeCb_);
  vkEndCommandBuffer(activeCb_);

  bool ok = sw.submitAndPresent(activeCb_, imageIndex);

  /* The command buffer is referenced by the in-flight fence the
   * swapchain just signalled — wait on that fence before freeing.
   * Single-frame-in-flight model, so wait device-idle. */
  vkQueueWaitIdle(ctx_->graphicsQueue);
  vkFreeCommandBuffers(ctx_->device, ctx_->commandPool, 1, &activeCb_);
  activeCb_ = VK_NULL_HANDLE;
  inFrame_ = false;

  /* Command buffer just freed; any VkBuffer it referenced is now safe to
   * destroy. Drain anything queued during the frame. */
  drainDeferredBuffers_();
  return ok;
}

void VulkanBackend::draw(DrawBatch *batch, const DrawUniforms &u)
{
  if (!batch || !inFrame_) return;
  for (DrawCommand *cmd : batch->commands) {
    issue(batch, cmd, u);
  }
}

} // namespace sculptcore::vulkan
