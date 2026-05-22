#include "vk_swapchain.h"

#include "vk_context.h"

#include <algorithm>
#include <cstdio>

namespace sculptcore::vulkan {

namespace {

bool createDepth(VkContext *ctx, int w, int h, VkFormat fmt,
                 VkImage &image, VkDeviceMemory &memory, VkImageView &view)
{
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = fmt;
  ici.extent = {uint32_t(w), uint32_t(h), 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(ctx->device, &ici, nullptr, &image) != VK_SUCCESS) {
    return false;
  }

  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(ctx->device, image, &mr);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex =
      ctx->findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mai.memoryTypeIndex == ~0u) {
    return false;
  }
  if (vkAllocateMemory(ctx->device, &mai, nullptr, &memory) != VK_SUCCESS) {
    return false;
  }
  vkBindImageMemory(ctx->device, image, memory, 0);

  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = image;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = fmt;
  vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  vci.subresourceRange.levelCount = 1;
  vci.subresourceRange.layerCount = 1;
  return vkCreateImageView(ctx->device, &vci, nullptr, &view) == VK_SUCCESS;
}

} // namespace

bool Swapchain::create(VkContext *c, int desiredW, int desiredH)
{
  release();
  ctx = c;
  if (ctx->surface == VK_NULL_HANDLE) {
    fprintf(stderr, "Swapchain::create: VkContext has no surface\n");
    return false;
  }

  VkSurfaceCapabilitiesKHR caps{};
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->physicalDevice, ctx->surface, &caps);

  /* Format selection. */
  uint32_t nf = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(ctx->physicalDevice, ctx->surface, &nf, nullptr);
  litestl::util::Vector<VkSurfaceFormatKHR> fmts;
  fmts.resize(nf);
  vkGetPhysicalDeviceSurfaceFormatsKHR(ctx->physicalDevice, ctx->surface, &nf, fmts.data());
  VkSurfaceFormatKHR chosen = fmts[0];
  for (auto &f : fmts) {
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      chosen = f;
      break;
    }
  }
  colorFormat = chosen.format;
  colorSpace = chosen.colorSpace;

  /* Extent: surface chooses if currentExtent == 0xFFFFFFFF, otherwise
   * we must use it. */
  VkExtent2D extent;
  if (caps.currentExtent.width != 0xFFFFFFFFu) {
    extent = caps.currentExtent;
  } else {
    extent.width = std::clamp(uint32_t(desiredW),
                              caps.minImageExtent.width,
                              caps.maxImageExtent.width);
    extent.height = std::clamp(uint32_t(desiredH),
                               caps.minImageExtent.height,
                               caps.maxImageExtent.height);
  }
  if (extent.width == 0 || extent.height == 0) {
    /* Window minimized — caller will recreate later. */
    fprintf(stderr, "Swapchain::create: zero extent (minimized?)\n");
    return false;
  }
  width = int(extent.width);
  height = int(extent.height);

  uint32_t imageCount = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
    imageCount = caps.maxImageCount;
  }

  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = ctx->surface;
  sci.minImageCount = imageCount;
  sci.imageFormat = colorFormat;
  sci.imageColorSpace = colorSpace;
  sci.imageExtent = extent;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  uint32_t qf[2] = {ctx->graphicsQueueFamily, ctx->presentQueueFamily};
  if (ctx->graphicsQueueFamily != ctx->presentQueueFamily) {
    sci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    sci.queueFamilyIndexCount = 2;
    sci.pQueueFamilyIndices = qf;
  } else {
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; /* vsync, always supported */
  sci.clipped = VK_TRUE;
  if (vkCreateSwapchainKHR(ctx->device, &sci, nullptr, &swapchain) != VK_SUCCESS) {
    fprintf(stderr, "Swapchain: vkCreateSwapchainKHR failed\n");
    release();
    return false;
  }

  uint32_t ni = 0;
  vkGetSwapchainImagesKHR(ctx->device, swapchain, &ni, nullptr);
  images.resize(ni);
  vkGetSwapchainImagesKHR(ctx->device, swapchain, &ni, images.data());

  views.resize(ni);
  for (uint32_t i = 0; i < ni; i++) {
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = images[i];
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = colorFormat;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(ctx->device, &vci, nullptr, &views[i]) != VK_SUCCESS) {
      release();
      return false;
    }
  }

  if (!createDepth(ctx, width, height, depthFormat, depthImage, depthMemory, depthView)) {
    release();
    return false;
  }

  /* Render pass: color (clear→present) + depth (clear→don't care). */
  VkAttachmentDescription atts[2]{};
  atts[0].format = colorFormat;
  atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
  atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  atts[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  atts[1].format = depthFormat;
  atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
  atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &colorRef;
  sub.pDepthStencilAttachment = &depthRef;

  /* Dependency so the present transition is properly synchronised. */
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.srcAccessMask = 0;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpi.attachmentCount = 2;
  rpi.pAttachments = atts;
  rpi.subpassCount = 1;
  rpi.pSubpasses = &sub;
  rpi.dependencyCount = 1;
  rpi.pDependencies = &dep;
  if (vkCreateRenderPass(ctx->device, &rpi, nullptr, &renderPass) != VK_SUCCESS) {
    release();
    return false;
  }

  framebuffers.resize(ni);
  for (uint32_t i = 0; i < ni; i++) {
    VkImageView fbv[2] = {views[i], depthView};
    VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbi.renderPass = renderPass;
    fbi.attachmentCount = 2;
    fbi.pAttachments = fbv;
    fbi.width = uint32_t(width);
    fbi.height = uint32_t(height);
    fbi.layers = 1;
    if (vkCreateFramebuffer(ctx->device, &fbi, nullptr, &framebuffers[i]) != VK_SUCCESS) {
      release();
      return false;
    }
  }

  VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  vkCreateSemaphore(ctx->device, &semi, nullptr, &imageAvailable);
  vkCreateSemaphore(ctx->device, &semi, nullptr, &renderComplete);
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  vkCreateFence(ctx->device, &fci, nullptr, &inFlightFence);

  return true;
}

void Swapchain::release()
{
  if (!ctx) return;
  VkDevice d = ctx->device;
  if (d != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(d);
  }

  if (inFlightFence)  { vkDestroyFence    (d, inFlightFence,  nullptr); inFlightFence  = VK_NULL_HANDLE; }
  if (imageAvailable) { vkDestroySemaphore(d, imageAvailable, nullptr); imageAvailable = VK_NULL_HANDLE; }
  if (renderComplete) { vkDestroySemaphore(d, renderComplete, nullptr); renderComplete = VK_NULL_HANDLE; }

  for (VkFramebuffer fb : framebuffers) {
    if (fb) vkDestroyFramebuffer(d, fb, nullptr);
  }
  framebuffers.clear();

  if (renderPass) { vkDestroyRenderPass(d, renderPass, nullptr); renderPass = VK_NULL_HANDLE; }

  if (depthView)   { vkDestroyImageView(d, depthView,   nullptr); depthView   = VK_NULL_HANDLE; }
  if (depthImage)  { vkDestroyImage    (d, depthImage,  nullptr); depthImage  = VK_NULL_HANDLE; }
  if (depthMemory) { vkFreeMemory      (d, depthMemory, nullptr); depthMemory = VK_NULL_HANDLE; }

  for (VkImageView v : views) {
    if (v) vkDestroyImageView(d, v, nullptr);
  }
  views.clear();
  images.clear(); /* owned by swapchain itself */

  if (swapchain) { vkDestroySwapchainKHR(d, swapchain, nullptr); swapchain = VK_NULL_HANDLE; }

  width = height = 0;
  ctx = nullptr;
}

bool Swapchain::recreate(int desiredW, int desiredH)
{
  VkContext *c = ctx;
  release();
  return create(c, desiredW, desiredH);
}

bool Swapchain::acquireNext(uint32_t &outIndex)
{
  vkWaitForFences(ctx->device, 1, &inFlightFence, VK_TRUE, UINT64_MAX);
  vkResetFences(ctx->device, 1, &inFlightFence);

  VkResult r = vkAcquireNextImageKHR(ctx->device, swapchain, UINT64_MAX,
                                     imageAvailable, VK_NULL_HANDLE, &outIndex);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    return false;
  }
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
    fprintf(stderr, "Swapchain::acquireNext: %d\n", int(r));
    return false;
  }
  return true;
}

void Swapchain::beginRenderPass(VkCommandBuffer cb, uint32_t imageIndex,
                                float r, float g, float b, float a) const
{
  VkClearValue clears[2]{};
  clears[0].color = {{r, g, b, a}};
  clears[1].depthStencil = {1.0f, 0};

  VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  bi.renderPass = renderPass;
  bi.framebuffer = framebuffers[imageIndex];
  bi.renderArea.extent = {uint32_t(width), uint32_t(height)};
  bi.clearValueCount = 2;
  bi.pClearValues = clears;
  vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport vp{};
  vp.width = float(width);
  vp.height = float(height);
  vp.maxDepth = 1.0f;
  vkCmdSetViewport(cb, 0, 1, &vp);

  VkRect2D sc{};
  sc.extent = {uint32_t(width), uint32_t(height)};
  vkCmdSetScissor(cb, 0, 1, &sc);
}

bool Swapchain::submitAndPresent(VkCommandBuffer cb, uint32_t imageIndex)
{
  VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &imageAvailable;
  si.pWaitDstStageMask = &waitStage;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &renderComplete;
  if (vkQueueSubmit(ctx->graphicsQueue, 1, &si, inFlightFence) != VK_SUCCESS) {
    fprintf(stderr, "Swapchain: vkQueueSubmit failed\n");
    return false;
  }

  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &renderComplete;
  pi.swapchainCount = 1;
  pi.pSwapchains = &swapchain;
  pi.pImageIndices = &imageIndex;
  VkResult r = vkQueuePresentKHR(ctx->presentQueue, &pi);
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
    return false;
  }
  return r == VK_SUCCESS;
}

} // namespace sculptcore::vulkan
