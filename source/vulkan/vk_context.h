#pragma once

#include <vulkan/vulkan.h>

#include "litestl/util/vector.h"

struct GLFWwindow;

namespace sculptcore::vulkan {

/** Owns the Vulkan instance + device + queue + command pool that all other
 *  vulkan/* helpers borrow. Single-queue, single-physical-device — enough for
 *  the debug app. Validation layers are enabled by default in Debug builds;
 *  set `validation = false` to suppress. */
struct VkContext {
  VkContext() = default;
  VkContext(const VkContext &) = delete;
  ~VkContext();

  /** Bring up instance + device + queue + command pool + descriptor pool.
   *  If `glfwWindow` is non-null, also creates the VkSurfaceKHR and selects
   *  a queue family with present support. Returns false on any failure;
   *  partial state is released. */
  bool init(GLFWwindow *glfwWindow = nullptr, bool validation = true);

  /** Allocate a primary command buffer, begin it, run `record(cb)`, end,
   *  submit to the graphics queue, and `vkQueueWaitIdle`. Convenience for
   *  one-shot work (uploads, layout transitions, the debug-app's single-
   *  frame submission). */
  template <typename F> bool runOneShot(F &&record);

  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;

  uint32_t graphicsQueueFamily = ~0u;
  uint32_t presentQueueFamily = ~0u;
  VkQueue graphicsQueue = VK_NULL_HANDLE;
  VkQueue presentQueue = VK_NULL_HANDLE;

  VkCommandPool commandPool = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

  /* Optional, only set if a window was passed to init(). */
  VkSurfaceKHR surface = VK_NULL_HANDLE;

  VkPhysicalDeviceMemoryProperties memoryProperties{};

  bool validationEnabled = false;

  /** Look up a memory type index satisfying `typeBits` (from
   *  VkMemoryRequirements) and `props` (VkMemoryPropertyFlags).
   *  Returns ~0u if none. */
  uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
};

/** Renderable offscreen target: color (RGBA8 unorm) + depth (D32_SFLOAT) +
 *  matching render pass and framebuffer. Sized once at create(); recreate by
 *  calling release() + create() with new dimensions. */
struct OffscreenTarget {
  OffscreenTarget() = default;
  OffscreenTarget(const OffscreenTarget &) = delete;
  ~OffscreenTarget() { release(); }

  bool create(VkContext *ctx, int w, int h);
  void release();

  /** Begin the render pass on the active command buffer, setting viewport
   *  and scissor to the full target. Caller must vkCmdEndRenderPass()
   *  before submitting. */
  void beginRenderPass(VkCommandBuffer cb, float r, float g, float b, float a) const;

  VkContext *ctx = nullptr;
  int width = 0;
  int height = 0;

  VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
  VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

  VkImage colorImage = VK_NULL_HANDLE;
  VkDeviceMemory colorMemory = VK_NULL_HANDLE;
  VkImageView colorView = VK_NULL_HANDLE;

  VkImage depthImage = VK_NULL_HANDLE;
  VkDeviceMemory depthMemory = VK_NULL_HANDLE;
  VkImageView depthView = VK_NULL_HANDLE;

  VkRenderPass renderPass = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
};

template <typename F> bool VkContext::runOneShot(F &&record)
{
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = commandPool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(device, &ai, &cb) != VK_SUCCESS) {
    return false;
  }
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &bi);
  record(cb);
  vkEndCommandBuffer(cb);

  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  VkResult sr = vkQueueSubmit(graphicsQueue, 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(graphicsQueue);
  vkFreeCommandBuffers(device, commandPool, 1, &cb);
  return sr == VK_SUCCESS;
}

} // namespace sculptcore::vulkan
