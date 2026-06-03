#include "vk_normals.h"
#include "vk_context.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifndef VK_COMPUTE_SPV_DIR
#define VK_COMPUTE_SPV_DIR
#endif

// VK_COMPUTE_SPV_DIR is passed unquoted by CMake (a quoted value can lose its
// quotes through the compiler-launcher/response-file plumbing on Windows), so
// stringize it here into a real C string literal.
#define VK_SPV_STRINGIZE_(x) #x
#define VK_SPV_STRINGIZE(x) VK_SPV_STRINGIZE_(x)

namespace sculptcore::vulkan {

static constexpr VkDeviceSize kVec3Stride = 16; // std430 array<vec3<f32>>

GpuNormalPass::~GpuNormalPass()
{
  if (!ctx_ || ctx_->device == VK_NULL_HANDLE) return;
  VkDevice d = ctx_->device;
  destroyBuf(triVerts_);
  destroyBuf(triNo_);
  destroyBuf(vtriMeta_);
  destroyBuf(vtriList_);
  destroyBuf(workTris_);
  destroyBuf(workVerts_);
  for (Pipe *p : {&face_, &vert_, &scatter_}) {
    if (p->pipeline) vkDestroyPipeline(d, p->pipeline, nullptr);
    if (p->layout) vkDestroyPipelineLayout(d, p->layout, nullptr);
    if (p->dsLayout) vkDestroyDescriptorSetLayout(d, p->dsLayout, nullptr);
  }
  if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
  if (scatterPool_) vkDestroyDescriptorPool(d, scatterPool_, nullptr);
}

bool GpuNormalPass::createBuf(Buf &b, VkDeviceSize size)
{
  VkDevice d = ctx_->device;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
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

void GpuNormalPass::destroyBuf(Buf &b)
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

bool GpuNormalPass::ensureBuf(Buf &b, VkDeviceSize size)
{
  if (size == 0) size = 16;
  if (b.buffer != VK_NULL_HANDLE && b.size >= size) return true;
  destroyBuf(b);
  VkDeviceSize rounded = 256;
  while (rounded < size) rounded *= 2;
  return createBuf(b, rounded);
}

bool GpuNormalPass::loadModule(const char *name, VkShaderModule &out)
{
  std::string path = std::string(VK_SPV_STRINGIZE(VK_COMPUTE_SPV_DIR)) + "/" + name;
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    std::fprintf(stderr, "GpuNormalPass: cannot open '%s'\n", path.c_str());
    return false;
  }
  std::streamsize n = f.tellg();
  f.seekg(0);
  std::vector<char> bytes(static_cast<size_t>(n));
  if (!f.read(bytes.data(), n) || (n % 4) != 0) {
    std::fprintf(stderr, "GpuNormalPass: bad spv '%s'\n", path.c_str());
    return false;
  }
  VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smi.codeSize = size_t(n);
  smi.pCode = reinterpret_cast<const uint32_t *>(bytes.data());
  return vkCreateShaderModule(ctx_->device, &smi, nullptr, &out) == VK_SUCCESS;
}

bool GpuNormalPass::buildPipe(Pipe &p, VkShaderModule module, int bindingCount)
{
  VkDevice d = ctx_->device;
  p.bindings = bindingCount;

  std::vector<VkDescriptorSetLayoutBinding> lb(static_cast<size_t>(bindingCount));
  for (int i = 0; i < bindingCount; i++) {
    lb[i] = {};
    lb[i].binding = uint32_t(i);
    lb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    lb[i].descriptorCount = 1;
    lb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo lci{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  lci.bindingCount = uint32_t(bindingCount);
  lci.pBindings = lb.data();
  if (vkCreateDescriptorSetLayout(d, &lci, nullptr, &p.dsLayout) != VK_SUCCESS)
    return false;

  VkPushConstantRange pcr{};
  pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pcr.offset = 0;
  pcr.size = sizeof(uint32_t);
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &p.dsLayout;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pcr;
  if (vkCreatePipelineLayout(d, &pli, nullptr, &p.layout) != VK_SUCCESS)
    return false;

  VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cpi.stage.module = module;
  cpi.stage.pName = "main";
  cpi.layout = p.layout;
  if (vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cpi, nullptr, &p.pipeline) !=
      VK_SUCCESS)
    return false;

  VkDescriptorSetAllocateInfo dsi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsi.descriptorPool = pool_;
  dsi.descriptorSetCount = 1;
  dsi.pSetLayouts = &p.dsLayout;
  return vkAllocateDescriptorSets(d, &dsi, &p.set) == VK_SUCCESS;
}

bool GpuNormalPass::init()
{
  VkDevice d = ctx_->device;

  // 4 (face) + 5 (vert) + 5 (scatter) storage descriptors across 3 sets.
  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 14};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 3;
  dpi.poolSizeCount = 1;
  dpi.pPoolSizes = &ps;
  if (vkCreateDescriptorPool(d, &dpi, nullptr, &pool_) != VK_SUCCESS) return false;

  VkShaderModule mFace, mVert, mScatter;
  if (!loadModule("normal_face.spv", mFace) ||
      !loadModule("normal_vert.spv", mVert) ||
      !loadModule("scatter.spv", mScatter)) {
    return false;
  }
  bool ok = buildPipe(face_, mFace, 4) && buildPipe(vert_, mVert, 5) &&
            buildPipe(scatter_, mScatter, 5);
  vkDestroyShaderModule(d, mFace, nullptr);
  vkDestroyShaderModule(d, mVert, nullptr);
  vkDestroyShaderModule(d, mScatter, nullptr);
  if (!ok) return false;

  // Dedicated pool for recordScatter()'s per-node sets. Sized for many nodes
  // per dab; sets are allocated lazily and reused round-robin across submits.
  constexpr uint32_t kMaxScatterSets = 256;
  VkDescriptorPoolSize sps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxScatterSets * 5};
  VkDescriptorPoolCreateInfo sdpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  sdpi.maxSets = kMaxScatterSets;
  sdpi.poolSizeCount = 1;
  sdpi.pPoolSizes = &sps;
  if (vkCreateDescriptorPool(d, &sdpi, nullptr, &scatterPool_) != VK_SUCCESS)
    return false;
  return true;
}

VkDescriptorSet GpuNormalPass::nextScatterSet()
{
  if (scatterCursor_ < scatterSets_.size()) {
    return scatterSets_[scatterCursor_++];
  }
  VkDescriptorSetAllocateInfo dsi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsi.descriptorPool = scatterPool_;
  dsi.descriptorSetCount = 1;
  dsi.pSetLayouts = &scatter_.dsLayout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(ctx_->device, &dsi, &set) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  scatterSets_.push_back(set);
  scatterCursor_++;
  return set;
}

void GpuNormalPass::bindStorage(VkDescriptorSet set, uint32_t binding,
                                VkBuffer buf)
{
  VkDescriptorBufferInfo bi{buf, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = set;
  w.dstBinding = binding;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w.pBufferInfo = &bi;
  vkUpdateDescriptorSets(ctx_->device, 1, &w, 0, nullptr);
}

bool GpuNormalPass::dispatch(const Pipe &p, uint32_t count)
{
  uint32_t groups = (count + 63u) / 64u;
  if (groups == 0) return true;
  return ctx_->runOneShot([&](VkCommandBuffer cb) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1,
                            &p.set, 0, nullptr);
    vkCmdPushConstants(cb, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(uint32_t), &count);
    vkCmdDispatch(cb, groups, 1, 1);
  });
}

bool GpuNormalPass::setTopology(const uint32_t *triVerts, int triCount,
                                const uint32_t *vtriMeta, int vertCount,
                                const uint32_t *vtriList, int listCount)
{
  triCount_ = triCount;
  vertCount_ = vertCount;
  if (!ensureBuf(triVerts_, VkDeviceSize(triCount) * 3 * sizeof(uint32_t)) ||
      !ensureBuf(triNo_, VkDeviceSize(triCount) * kVec3Stride) ||
      !ensureBuf(vtriMeta_, VkDeviceSize(vertCount) * 2 * sizeof(uint32_t)) ||
      !ensureBuf(vtriList_,
                 VkDeviceSize(listCount < 1 ? 1 : listCount) * sizeof(uint32_t))) {
    return false;
  }
  std::memcpy(triVerts_.mapped, triVerts,
              size_t(triCount) * 3 * sizeof(uint32_t));
  std::memcpy(vtriMeta_.mapped, vtriMeta,
              size_t(vertCount) * 2 * sizeof(uint32_t));
  if (listCount > 0) {
    std::memcpy(vtriList_.mapped, vtriList,
                size_t(listCount) * sizeof(uint32_t));
  }
  return true;
}

bool GpuNormalPass::prepareNormals(VkBuffer co, VkBuffer no,
                                   const uint32_t *workTris, int triWorkCount,
                                   const uint32_t *workVerts, int vertWorkCount)
{
  triWork_ = triWorkCount;
  vertWork_ = vertWorkCount;
  if (!ensureBuf(workTris_,
                 VkDeviceSize(triWorkCount < 1 ? 1 : triWorkCount) *
                     sizeof(uint32_t)) ||
      !ensureBuf(workVerts_,
                 VkDeviceSize(vertWorkCount < 1 ? 1 : vertWorkCount) *
                     sizeof(uint32_t))) {
    return false;
  }
  if (triWorkCount > 0) {
    std::memcpy(workTris_.mapped, workTris,
                size_t(triWorkCount) * sizeof(uint32_t));
  }
  if (vertWorkCount > 0) {
    std::memcpy(workVerts_.mapped, workVerts,
                size_t(vertWorkCount) * sizeof(uint32_t));
  }

  // Face pass: co (0) + triVerts (1) -> triNo (2), work list (3).
  bindStorage(face_.set, 0, co);
  bindStorage(face_.set, 1, triVerts_.buffer);
  bindStorage(face_.set, 2, triNo_.buffer);
  bindStorage(face_.set, 3, workTris_.buffer);

  // Vert pass: triNo (0) + vtriMeta (1) + vtriList (2) -> no (3), work list (4).
  bindStorage(vert_.set, 0, triNo_.buffer);
  bindStorage(vert_.set, 1, vtriMeta_.buffer);
  bindStorage(vert_.set, 2, vtriList_.buffer);
  bindStorage(vert_.set, 3, no);
  bindStorage(vert_.set, 4, workVerts_.buffer);
  return true;
}

void GpuNormalPass::recordNormals(VkCommandBuffer cb)
{
  uint32_t faceCount = uint32_t(triWork_);
  uint32_t faceGroups = (faceCount + 63u) / 64u;
  if (faceGroups > 0) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, face_.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, face_.layout, 0,
                            1, &face_.set, 0, nullptr);
    vkCmdPushConstants(cb, face_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(uint32_t), &faceCount);
    vkCmdDispatch(cb, faceGroups, 1, 1);
  }

  // The vert pass reads the triNo the face pass just wrote.
  computeBarrier(cb);

  uint32_t vertCount = uint32_t(vertWork_);
  uint32_t vertGroups = (vertCount + 63u) / 64u;
  if (vertGroups > 0) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, vert_.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, vert_.layout, 0,
                            1, &vert_.set, 0, nullptr);
    vkCmdPushConstants(cb, vert_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(uint32_t), &vertCount);
    vkCmdDispatch(cb, vertGroups, 1, 1);
  }
}

void GpuNormalPass::computeBarrier(VkCommandBuffer cb)
{
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                       nullptr, 0, nullptr);
}

bool GpuNormalPass::computeNormals(VkBuffer co, VkBuffer no)
{
  // Full-mesh pass: identity work lists [0..triCount) / [0..vertCount).
  std::vector<uint32_t> idTris(size_t(triCount_ < 0 ? 0 : triCount_));
  std::vector<uint32_t> idVerts(size_t(vertCount_ < 0 ? 0 : vertCount_));
  for (int i = 0; i < triCount_; i++) idTris[size_t(i)] = uint32_t(i);
  for (int i = 0; i < vertCount_; i++) idVerts[size_t(i)] = uint32_t(i);
  if (!prepareNormals(co, no, idTris.data(), triCount_, idVerts.data(),
                      vertCount_)) {
    return false;
  }
  return ctx_->runOneShot([&](VkCommandBuffer cb) { recordNormals(cb); });
}

bool GpuNormalPass::scatter(VkBuffer co, VkBuffer no, VkBuffer slotVertex,
                            VkBuffer pos, VkBuffer nor, int slotCount)
{
  bindStorage(scatter_.set, 0, co);
  bindStorage(scatter_.set, 1, no);
  bindStorage(scatter_.set, 2, slotVertex);
  bindStorage(scatter_.set, 3, pos);
  bindStorage(scatter_.set, 4, nor);
  return dispatch(scatter_, uint32_t(slotCount));
}

void GpuNormalPass::beginScatterBatch()
{
  scatterCursor_ = 0;
}

void GpuNormalPass::recordScatter(VkCommandBuffer cb, VkBuffer co, VkBuffer no,
                                  VkBuffer slotVertex, VkBuffer pos,
                                  VkBuffer nor, int slotCount)
{
  uint32_t count = uint32_t(slotCount);
  uint32_t groups = (count + 63u) / 64u;
  if (groups == 0) return;

  VkDescriptorSet set = nextScatterSet();
  if (set == VK_NULL_HANDLE) return;
  bindStorage(set, 0, co);
  bindStorage(set, 1, no);
  bindStorage(set, 2, slotVertex);
  bindStorage(set, 3, pos);
  bindStorage(set, 4, nor);

  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, scatter_.pipeline);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, scatter_.layout, 0,
                          1, &set, 0, nullptr);
  vkCmdPushConstants(cb, scatter_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(uint32_t), &count);
  vkCmdDispatch(cb, groups, 1, 1);
}

} // namespace sculptcore::vulkan
