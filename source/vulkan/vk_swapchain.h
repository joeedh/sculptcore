#pragma once

#include <vulkan/vulkan.h>

#include "litestl/util/vector.h"

struct GLFWwindow;

namespace sculptcore::vulkan {

struct VkContext;

/** Presentation-ready swapchain + depth attachment + render pass +
 *  per-image framebuffers + per-frame sync primitives.
 *
 *  Single in-flight frame to keep the debug app's render loop simple:
 *  acquire → record → submit (wait for image-acquire semaphore, signal
 *  render-complete semaphore, signal in-flight fence) → present (wait
 *  for render-complete semaphore) → vkWaitForFences before the next
 *  acquire. */
struct Swapchain {
  Swapchain() = default;
  Swapchain(const Swapchain &) = delete;
  ~Swapchain()
  {
    release();
  }

  /** Build the swapchain over `ctx->surface`. Picks a B8G8R8A8_UNORM
   *  surface format (or first available), FIFO present mode, and an
   *  extent clamped to surface capabilities using `desiredW/H` as a hint
   *  when the surface reports an indeterminate extent. */
  bool create(VkContext *ctx, int desiredW, int desiredH);

  /** Tear everything down. Safe to call multiple times. */
  void release();

  /** Recreate on window resize / out-of-date. Waits device idle first. */
  bool recreate(int desiredW, int desiredH);

  /** Wait for the previous frame's fence, then acquire the next swapchain
   *  image into `outIndex`. Returns false if the swapchain is out-of-date
   *  (caller should `recreate`). */
  bool acquireNext(uint32_t &outIndex);

  /** Begin the swapchain render pass on the active command buffer. */
  void beginRenderPass(
      VkCommandBuffer cb, uint32_t imageIndex, float r, float g, float b, float a) const;

  /** Submit `cb` (waits on imageAvailable, signals renderComplete +
   *  inFlightFence), then present `imageIndex` (waits on
   *  renderComplete). Returns false if present reports out-of-date. */
  bool submitAndPresent(VkCommandBuffer cb, uint32_t imageIndex);

  VkContext *ctx = nullptr;
  int width = 0;
  int height = 0;

  VkFormat colorFormat = VK_FORMAT_B8G8R8A8_UNORM;
  VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
  VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  litestl::util::Vector<VkImage> images;
  litestl::util::Vector<VkImageView> views;
  litestl::util::Vector<VkFramebuffer> framebuffers;

  VkImage depthImage = VK_NULL_HANDLE;
  VkDeviceMemory depthMemory = VK_NULL_HANDLE;
  VkImageView depthView = VK_NULL_HANDLE;

  VkRenderPass renderPass = VK_NULL_HANDLE;

  VkSemaphore imageAvailable = VK_NULL_HANDLE;
  VkSemaphore renderComplete = VK_NULL_HANDLE;
  VkFence inFlightFence = VK_NULL_HANDLE;
};

} // namespace sculptcore::vulkan
