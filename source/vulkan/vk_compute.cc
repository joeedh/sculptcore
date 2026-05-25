#include "vk_compute.h"
#include "vk_context.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace sculptcore::vulkan {

static constexpr VkDeviceSize kVec3Stride = 16; // std430 array<vec3<f32>>

BrushComputeDispatch::~BrushComputeDispatch()
{
  if (!ctx_ || ctx_->device == VK_NULL_HANDLE) return;
  VkDevice d = ctx_->device;
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
  if (sampler_) vkDestroySampler(d, sampler_, nullptr);
  if (whiteView_) vkDestroyImageView(d, whiteView_, nullptr);
  if (whiteImage_) vkDestroyImage(d, whiteImage_, nullptr);
  if (whiteMem_) vkFreeMemory(d, whiteMem_, nullptr);
  if (pipeline_) vkDestroyPipeline(d, pipeline_, nullptr);
  if (pipeLayout_) vkDestroyPipelineLayout(d, pipeLayout_, nullptr);
  if (setLayout_) vkDestroyDescriptorSetLayout(d, setLayout_, nullptr);
  if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
  if (module_) vkDestroyShaderModule(d, module_, nullptr);
}

bool BrushComputeDispatch::createBuf(Buf &b, VkDeviceSize size,
                                     VkBufferUsageFlags usage)
{
  VkDevice d = ctx_->device;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(d, &bi, nullptr, &b.buffer) != VK_SUCCESS) return false;

  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(d, b.buffer, &mr);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = ctx_->findMemoryType(
      mr.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (mai.memoryTypeIndex == ~0u ||
      vkAllocateMemory(d, &mai, nullptr, &b.mem) != VK_SUCCESS) {
    vkDestroyBuffer(d, b.buffer, nullptr);
    b.buffer = VK_NULL_HANDLE;
    return false;
  }
  vkBindBufferMemory(d, b.buffer, b.mem, 0);
  vkMapMemory(d, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped);
  b.size = size;
  return true;
}

void BrushComputeDispatch::destroyBuf(Buf &b)
{
  VkDevice d = ctx_->device;
  if (b.mapped) {
    vkUnmapMemory(d, b.mem);
    b.mapped = nullptr;
  }
  if (b.buffer) vkDestroyBuffer(d, b.buffer, nullptr);
  if (b.mem) vkFreeMemory(d, b.mem, nullptr);
  b.buffer = VK_NULL_HANDLE;
  b.mem = VK_NULL_HANDLE;
  b.size = 0;
}

bool BrushComputeDispatch::ensureBuf(Buf &b, VkDeviceSize size,
                                     VkBufferUsageFlags usage)
{
  if (size == 0) size = 16;
  if (b.buffer != VK_NULL_HANDLE && b.size >= size) return true;
  destroyBuf(b);
  // Round up to reduce churn across dabs of varying size.
  VkDeviceSize rounded = 256;
  while (rounded < size) rounded *= 2;
  return createBuf(b, rounded, usage);
}

void BrushComputeDispatch::writeStorage(uint32_t binding, const Buf &b)
{
  VkDescriptorBufferInfo bi{b.buffer, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = set_;
  w.dstBinding = binding;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w.pBufferInfo = &bi;
  vkUpdateDescriptorSets(ctx_->device, 1, &w, 0, nullptr);
}

void BrushComputeDispatch::writeUniform(uint32_t binding, const Buf &b)
{
  VkDescriptorBufferInfo bi{b.buffer, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = set_;
  w.dstBinding = binding;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  w.pBufferInfo = &bi;
  vkUpdateDescriptorSets(ctx_->device, 1, &w, 0, nullptr);
}

bool BrushComputeDispatch::createWhiteTexture()
{
  VkDevice d = ctx_->device;
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R8G8B8A8_UNORM;
  ici.extent = {1, 1, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_LINEAR;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
  if (vkCreateImage(d, &ici, nullptr, &whiteImage_) != VK_SUCCESS) return false;

  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(d, whiteImage_, &mr);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = ctx_->findMemoryType(
      mr.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (mai.memoryTypeIndex == ~0u ||
      vkAllocateMemory(d, &mai, nullptr, &whiteMem_) != VK_SUCCESS) {
    return false;
  }
  vkBindImageMemory(d, whiteImage_, whiteMem_, 0);

  void *p = nullptr;
  vkMapMemory(d, whiteMem_, 0, VK_WHOLE_SIZE, 0, &p);
  uint32_t white = 0xFFFFFFFFu;
  std::memcpy(p, &white, 4);
  vkUnmapMemory(d, whiteMem_);

  // PREINITIALIZED -> GENERAL, preserving the host-written texel.
  ctx_->runOneShot([&](VkCommandBuffer cb) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.image = whiteImage_;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  });

  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = whiteImage_;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = VK_FORMAT_R8G8B8A8_UNORM;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(d, &vci, nullptr, &whiteView_) != VK_SUCCESS) return false;

  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = VK_FILTER_LINEAR;
  sci.minFilter = VK_FILTER_LINEAR;
  sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  return vkCreateSampler(d, &sci, nullptr, &sampler_) == VK_SUCCESS;
}

bool BrushComputeDispatch::loadSpirv(const char *path)
{
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    std::fprintf(stderr, "BrushComputeDispatch: cannot open '%s'\n", path);
    return false;
  }
  std::streamsize n = f.tellg();
  f.seekg(0);
  size_t len = size_t(n);
  std::vector<char> bytes(len);
  if (!f.read(bytes.data(), n) || (n % 4) != 0) {
    std::fprintf(stderr, "BrushComputeDispatch: bad spv '%s'\n", path);
    return false;
  }

  VkDevice d = ctx_->device;
  VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smi.codeSize = size_t(n);
  smi.pCode = reinterpret_cast<const uint32_t *>(bytes.data());
  if (vkCreateShaderModule(d, &smi, nullptr, &module_) != VK_SUCCESS) return false;

  // 14 group-0 bindings, all visible to the compute stage. Bindings 11-13
  // (co_prev + neighbor CSR) are only referenced by for_neighbor kernels, but
  // the layout always declares them so one bind-group setup serves every
  // brush; non-neighbor shaders simply don't use them.
  VkDescriptorSetLayoutBinding lb[14]{};
  auto set = [&](int i, VkDescriptorType t) {
    lb[i].binding = uint32_t(i);
    lb[i].descriptorType = t;
    lb[i].descriptorCount = 1;
    lb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  };
  set(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  set(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  set(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  set(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  set(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  set(5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
  set(6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
  set(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  set(8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
  set(9, VK_DESCRIPTOR_TYPE_SAMPLER);
  set(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  set(11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  set(12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  set(13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

  VkDescriptorSetLayoutCreateInfo lci{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  lci.bindingCount = 14;
  lci.pBindings = lb;
  if (vkCreateDescriptorSetLayout(d, &lci, nullptr, &setLayout_) != VK_SUCCESS)
    return false;

  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &setLayout_;
  if (vkCreatePipelineLayout(d, &pli, nullptr, &pipeLayout_) != VK_SUCCESS)
    return false;

  VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cpi.stage.module = module_;
  cpi.stage.pName = "main";
  cpi.layout = pipeLayout_;
  if (vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline_) !=
      VK_SUCCESS)
    return false;

  VkDescriptorPoolSize ps[4]{};
  ps[0] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 10};
  ps[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2};
  ps[2] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1};
  ps[3] = {VK_DESCRIPTOR_TYPE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 1;
  dpi.poolSizeCount = 4;
  dpi.pPoolSizes = ps;
  if (vkCreateDescriptorPool(d, &dpi, nullptr, &pool_) != VK_SUCCESS) return false;

  VkDescriptorSetAllocateInfo dsi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsi.descriptorPool = pool_;
  dsi.descriptorSetCount = 1;
  dsi.pSetLayouts = &setLayout_;
  if (vkAllocateDescriptorSets(d, &dsi, &set_) != VK_SUCCESS) return false;

  if (!createWhiteTexture()) return false;

  // Bind the (constant) texture + sampler once.
  VkDescriptorImageInfo ii{};
  ii.imageView = whiteView_;
  ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo si{};
  si.sampler = sampler_;
  VkWriteDescriptorSet w[2]{};
  w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w[0].dstSet = set_;
  w[0].dstBinding = 8;
  w[0].descriptorCount = 1;
  w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  w[0].pImageInfo = &ii;
  w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w[1].dstSet = set_;
  w[1].dstBinding = 9;
  w[1].descriptorCount = 1;
  w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  w[1].pImageInfo = &si;
  vkUpdateDescriptorSets(d, 2, w, 0, nullptr);
  return true;
}

bool BrushComputeDispatch::beginStroke(const float *co, const float *no,
                                       const float *mask, int vertCount)
{
  vertCount_ = vertCount;
  hasNeighbors_ = false;
  const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (!ensureBuf(co_, VkDeviceSize(vertCount) * kVec3Stride, storage) ||
      !ensureBuf(no_, VkDeviceSize(vertCount) * kVec3Stride, storage) ||
      !ensureBuf(mask_, VkDeviceSize(vertCount) * sizeof(float), storage) ||
      !ensureBuf(coPrev_, VkDeviceSize(vertCount) * kVec3Stride, storage) ||
      !ensureBuf(nbrMeta_, 0, storage) || !ensureBuf(nbrVerts_, 0, storage)) {
    return false;
  }
  // Expand packed xyz into 16-byte std430 vec3 slots.
  auto *coDst = static_cast<float *>(co_.mapped);
  auto *noDst = static_cast<float *>(no_.mapped);
  for (int i = 0; i < vertCount; i++) {
    coDst[i * 4 + 0] = co[i * 3 + 0];
    coDst[i * 4 + 1] = co[i * 3 + 1];
    coDst[i * 4 + 2] = co[i * 3 + 2];
    coDst[i * 4 + 3] = 0.0f;
    noDst[i * 4 + 0] = no[i * 3 + 0];
    noDst[i * 4 + 1] = no[i * 3 + 1];
    noDst[i * 4 + 2] = no[i * 3 + 2];
    noDst[i * 4 + 3] = 0.0f;
  }
  std::memcpy(mask_.mapped, mask, size_t(vertCount) * sizeof(float));
  writeStorage(0, co_);
  writeStorage(1, no_);
  writeStorage(2, mask_);
  // co_prev / neighbor CSR are always bound so the descriptor set is valid
  // even for non-neighbor kernels; setNeighbors overwrites 12/13 with real
  // data. co_prev is filled per dab from the previous result.
  writeStorage(11, coPrev_);
  writeStorage(12, nbrMeta_);
  writeStorage(13, nbrVerts_);
  return true;
}

bool BrushComputeDispatch::setNeighbors(const ComputeVertNbr *meta,
                                        int vertCount, const uint32_t *nbrVerts,
                                        int nbrCount)
{
  const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (!ensureBuf(nbrMeta_, VkDeviceSize(vertCount) * sizeof(ComputeVertNbr), storage) ||
      !ensureBuf(nbrVerts_, VkDeviceSize(nbrCount < 1 ? 1 : nbrCount) * sizeof(uint32_t),
                 storage)) {
    return false;
  }
  std::memcpy(nbrMeta_.mapped, meta, size_t(vertCount) * sizeof(ComputeVertNbr));
  if (nbrCount > 0) {
    std::memcpy(nbrVerts_.mapped, nbrVerts, size_t(nbrCount) * sizeof(uint32_t));
  }
  writeStorage(12, nbrMeta_);
  writeStorage(13, nbrVerts_);
  hasNeighbors_ = true;
  return true;
}

bool BrushComputeDispatch::dab(const ComputeBrushUniforms &brushU,
                               const ComputeCtxUniforms &ctxU,
                               const uint32_t *uniqueVerts, int uniqueVertCount,
                               const ComputeNodeMeta *nodes, int nodeCount,
                               const float *falloffLut,
                               const ComputeStrokeSample *strokePath,
                               int strokeCount)
{
  if (nodeCount == 0) return true;
  const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  const VkBufferUsageFlags uniform = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

  if (!ensureBuf(unique_, VkDeviceSize(uniqueVertCount) * sizeof(uint32_t), storage) ||
      !ensureBuf(nodes_, VkDeviceSize(nodeCount) * sizeof(ComputeNodeMeta), storage) ||
      !ensureBuf(brushU_, sizeof(ComputeBrushUniforms), uniform) ||
      !ensureBuf(ctxU_, sizeof(ComputeCtxUniforms), uniform) ||
      !ensureBuf(falloff_, 256 * sizeof(float), storage) ||
      !ensureBuf(stroke_, VkDeviceSize(strokeCount < 1 ? 1 : strokeCount) *
                              sizeof(ComputeStrokeSample),
                 storage)) {
    return false;
  }

  std::memcpy(unique_.mapped, uniqueVerts,
              size_t(uniqueVertCount) * sizeof(uint32_t));
  std::memcpy(nodes_.mapped, nodes, size_t(nodeCount) * sizeof(ComputeNodeMeta));
  std::memcpy(brushU_.mapped, &brushU, sizeof(ComputeBrushUniforms));
  std::memcpy(ctxU_.mapped, &ctxU, sizeof(ComputeCtxUniforms));
  std::memcpy(falloff_.mapped, falloffLut, 256 * sizeof(float));
  if (strokeCount > 0) {
    std::memcpy(stroke_.mapped, strokePath,
                size_t(strokeCount) * sizeof(ComputeStrokeSample));
  }

  writeStorage(3, unique_);
  writeStorage(4, nodes_);
  writeUniform(5, brushU_);
  writeUniform(6, ctxU_);
  writeStorage(7, falloff_);
  writeStorage(10, stroke_);

  // Jacobi snapshot: capture the pre-dab positions so for_neighbor reads a
  // consistent state (co_buf is written in place by this dispatch). Cheap
  // host copy of mapped, coherent memory; matches the C++ executor's snapshot.
  if (hasNeighbors_) {
    std::memcpy(coPrev_.mapped, co_.mapped, size_t(vertCount_) * kVec3Stride);
  }

  return ctx_->runOneShot([&](VkCommandBuffer cb) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1,
                            &set_, 0, nullptr);
    vkCmdDispatch(cb, uint32_t(nodeCount), 1, 1);
  });
}

bool BrushComputeDispatch::endStroke(float *coOut, float *noOut, float *maskOut)
{
  if (coOut) {
    auto *src = static_cast<const float *>(co_.mapped);
    for (int i = 0; i < vertCount_; i++) {
      coOut[i * 3 + 0] = src[i * 4 + 0];
      coOut[i * 3 + 1] = src[i * 4 + 1];
      coOut[i * 3 + 2] = src[i * 4 + 2];
    }
  }
  if (noOut) {
    auto *src = static_cast<const float *>(no_.mapped);
    for (int i = 0; i < vertCount_; i++) {
      noOut[i * 3 + 0] = src[i * 4 + 0];
      noOut[i * 3 + 1] = src[i * 4 + 1];
      noOut[i * 3 + 2] = src[i * 4 + 2];
    }
  }
  if (maskOut) {
    std::memcpy(maskOut, mask_.mapped, size_t(vertCount_) * sizeof(float));
  }
  return true;
}

} // namespace sculptcore::vulkan
