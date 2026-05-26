#include "vk_context.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstring>

namespace sculptcore::vulkan {

static const char *vk_result_str(VkResult r)
{
  switch (r) {
  case VK_SUCCESS: return "VK_SUCCESS";
  case VK_NOT_READY: return "VK_NOT_READY";
  case VK_TIMEOUT: return "VK_TIMEOUT";
  case VK_EVENT_SET: return "VK_EVENT_SET";
  case VK_EVENT_RESET: return "VK_EVENT_RESET";
  case VK_INCOMPLETE: return "VK_INCOMPLETE";
  case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
  case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
  case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
  case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
  case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
  case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
  case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
  case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
  case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
  default: return "VK_ERROR_<other>";
  }
}

#define VK_CHECK(expr)                                                                  \
  do {                                                                                  \
    VkResult _r = (expr);                                                               \
    if (_r != VK_SUCCESS) {                                                             \
      fprintf(stderr, "%s:%d: %s -> %s\n", __FILE__, __LINE__, #expr, vk_result_str(_r)); \
      return false;                                                                     \
    }                                                                                   \
  } while (0)

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT *data,
    void * /*userData*/)
{
  if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    fprintf(stderr, "[vk] %s\n", data->pMessage);
  }
  return VK_FALSE;
}

static bool layerAvailable(const char *name)
{
  uint32_t count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  litestl::util::Vector<VkLayerProperties> layers;
  layers.resize(count);
  vkEnumerateInstanceLayerProperties(&count, layers.data());
  for (const auto &l : layers) {
    if (strcmp(l.layerName, name) == 0) return true;
  }
  return false;
}

VkContext::~VkContext()
{
  /* The device must be idle before any of its objects are destroyed (Vulkan
   * spec). runOneShot waits per-submit, but make the teardown invariant
   * explicit rather than relying on every caller having drained the queue. */
  if (device) vkDeviceWaitIdle(device);
  if (oneShotFence) vkDestroyFence(device, oneShotFence, nullptr);
  if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
  if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
  if (device) vkDestroyDevice(device, nullptr);
  if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
  if (debugMessenger) {
    auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance, "vkDestroyDebugUtilsMessengerEXT");
    if (fn) fn(instance, debugMessenger, nullptr);
  }
  if (instance) vkDestroyInstance(instance, nullptr);
}

uint32_t VkContext::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const
{
  for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
    if ((typeBits & (1u << i)) &&
        (memoryProperties.memoryTypes[i].propertyFlags & props) == props) {
      return i;
    }
  }
  return ~0u;
}

bool VkContext::init(GLFWwindow *glfwWindow, bool validation)
{
  /* --- instance --- */
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "sculptcore";
  app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
  app.pEngineName = "sculptcore";
  app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
  app.apiVersion = VK_API_VERSION_1_2;

  litestl::util::Vector<const char *> instExts;
  if (glfwWindow) {
    uint32_t n = 0;
    const char **glfwExts = glfwGetRequiredInstanceExtensions(&n);
    for (uint32_t i = 0; i < n; i++) instExts.append(glfwExts[i]);
  }
  if (validation && layerAvailable("VK_LAYER_KHRONOS_validation")) {
    instExts.append(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    validationEnabled = true;
  }
  const char *valLayers[] = {"VK_LAYER_KHRONOS_validation"};

  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = uint32_t(instExts.size());
  ici.ppEnabledExtensionNames = instExts.data();
  if (validationEnabled) {
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = valLayers;
  }
  VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

  if (validationEnabled) {
    auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance, "vkCreateDebugUtilsMessengerEXT");
    if (create) {
      VkDebugUtilsMessengerCreateInfoEXT dci{
          VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
      dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      dci.pfnUserCallback = debug_callback;
      create(instance, &dci, nullptr, &debugMessenger);
    }
  }

  /* --- surface (optional) --- */
  if (glfwWindow) {
    VK_CHECK(glfwCreateWindowSurface(instance, glfwWindow, nullptr, &surface));
  }

  /* --- physical device --- */
  uint32_t nDev = 0;
  vkEnumeratePhysicalDevices(instance, &nDev, nullptr);
  if (nDev == 0) {
    fprintf(stderr, "VkContext: no Vulkan-capable GPU\n");
    return false;
  }
  litestl::util::Vector<VkPhysicalDevice> devs;
  devs.resize(nDev);
  vkEnumeratePhysicalDevices(instance, &nDev, devs.data());

  /* Prefer discrete GPUs. */
  for (auto d : devs) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(d, &p);
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      physicalDevice = d;
      break;
    }
  }
  if (!physicalDevice) physicalDevice = devs[0];
  vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

  /* --- queue families --- */
  uint32_t nq = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &nq, nullptr);
  litestl::util::Vector<VkQueueFamilyProperties> qfs;
  qfs.resize(nq);
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &nq, qfs.data());

  for (uint32_t i = 0; i < nq; i++) {
    if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      if (graphicsQueueFamily == ~0u) graphicsQueueFamily = i;
    }
    if (surface) {
      VkBool32 supports = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface, &supports);
      if (supports && presentQueueFamily == ~0u) presentQueueFamily = i;
    }
  }
  if (graphicsQueueFamily == ~0u) {
    fprintf(stderr, "VkContext: no graphics queue family\n");
    return false;
  }
  if (surface && presentQueueFamily == ~0u) {
    fprintf(stderr, "VkContext: no queue family supports present\n");
    return false;
  }

  /* --- logical device --- */
  float prio = 1.0f;
  litestl::util::Vector<VkDeviceQueueCreateInfo> qci;
  {
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = graphicsQueueFamily;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;
    qci.append(q);
  }
  if (surface && presentQueueFamily != graphicsQueueFamily) {
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = presentQueueFamily;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;
    qci.append(q);
  }

  litestl::util::Vector<const char *> devExts;
  if (surface) devExts.append(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = uint32_t(qci.size());
  dci.pQueueCreateInfos = qci.data();
  dci.enabledExtensionCount = uint32_t(devExts.size());
  dci.ppEnabledExtensionNames = devExts.data();
  VK_CHECK(vkCreateDevice(physicalDevice, &dci, nullptr, &device));

  vkGetDeviceQueue(device, graphicsQueueFamily, 0, &graphicsQueue);
  if (surface) vkGetDeviceQueue(device, presentQueueFamily, 0, &presentQueue);

  /* --- pools --- */
  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.queueFamilyIndex = graphicsQueueFamily;
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VK_CHECK(vkCreateCommandPool(device, &cpi, nullptr, &commandPool));

  /* Persistent one-shot command buffer + fence (see runOneShot). */
  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = commandPool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VK_CHECK(vkAllocateCommandBuffers(device, &cbai, &oneShotCmd));
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VK_CHECK(vkCreateFence(device, &fci, nullptr, &oneShotFence));

  VkDescriptorPoolSize ps{};
  ps.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  ps.descriptorCount = 64;
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 64;
  dpi.poolSizeCount = 1;
  dpi.pPoolSizes = &ps;
  VK_CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptorPool));

  return true;
}

/* ----------------------- OffscreenTarget ----------------------- */

static bool createImage(VkContext *ctx,
                        int w,
                        int h,
                        VkFormat fmt,
                        VkImageUsageFlags usage,
                        VkImageAspectFlags aspect,
                        VkImage *image,
                        VkDeviceMemory *memory,
                        VkImageView *view)
{
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = fmt;
  ici.extent = {uint32_t(w), uint32_t(h), 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = usage;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(ctx->device, &ici, nullptr, image) != VK_SUCCESS) return false;

  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(ctx->device, *image, &mr);

  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex =
      ctx->findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mai.memoryTypeIndex == ~0u) return false;
  if (vkAllocateMemory(ctx->device, &mai, nullptr, memory) != VK_SUCCESS) return false;
  vkBindImageMemory(ctx->device, *image, *memory, 0);

  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = *image;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = fmt;
  vci.subresourceRange.aspectMask = aspect;
  vci.subresourceRange.levelCount = 1;
  vci.subresourceRange.layerCount = 1;
  return vkCreateImageView(ctx->device, &vci, nullptr, view) == VK_SUCCESS;
}

bool OffscreenTarget::create(VkContext *c, int w, int h)
{
  release();
  ctx = c;
  width = w;
  height = h;

  if (!createImage(ctx,
                   w,
                   h,
                   colorFormat,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT,
                   &colorImage,
                   &colorMemory,
                   &colorView)) {
    release();
    return false;
  }
  if (!createImage(ctx,
                   w,
                   h,
                   depthFormat,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                   VK_IMAGE_ASPECT_DEPTH_BIT,
                   &depthImage,
                   &depthMemory,
                   &depthView)) {
    release();
    return false;
  }

  /* Single subpass: color + depth attachments. */
  VkAttachmentDescription atts[2]{};
  atts[0].format = colorFormat;
  atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
  atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  atts[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
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

  VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpi.attachmentCount = 2;
  rpi.pAttachments = atts;
  rpi.subpassCount = 1;
  rpi.pSubpasses = &sub;
  if (vkCreateRenderPass(ctx->device, &rpi, nullptr, &renderPass) != VK_SUCCESS) {
    release();
    return false;
  }

  VkImageView views[2] = {colorView, depthView};
  VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbi.renderPass = renderPass;
  fbi.attachmentCount = 2;
  fbi.pAttachments = views;
  fbi.width = uint32_t(w);
  fbi.height = uint32_t(h);
  fbi.layers = 1;
  if (vkCreateFramebuffer(ctx->device, &fbi, nullptr, &framebuffer) != VK_SUCCESS) {
    release();
    return false;
  }
  return true;
}

void OffscreenTarget::release()
{
  if (!ctx) return;
  VkDevice d = ctx->device;
  if (framebuffer) { vkDestroyFramebuffer(d, framebuffer, nullptr); framebuffer = VK_NULL_HANDLE; }
  if (renderPass)  { vkDestroyRenderPass (d, renderPass,  nullptr); renderPass  = VK_NULL_HANDLE; }
  if (colorView)   { vkDestroyImageView  (d, colorView,   nullptr); colorView   = VK_NULL_HANDLE; }
  if (colorImage)  { vkDestroyImage      (d, colorImage,  nullptr); colorImage  = VK_NULL_HANDLE; }
  if (colorMemory) { vkFreeMemory        (d, colorMemory, nullptr); colorMemory = VK_NULL_HANDLE; }
  if (depthView)   { vkDestroyImageView  (d, depthView,   nullptr); depthView   = VK_NULL_HANDLE; }
  if (depthImage)  { vkDestroyImage      (d, depthImage,  nullptr); depthImage  = VK_NULL_HANDLE; }
  if (depthMemory) { vkFreeMemory        (d, depthMemory, nullptr); depthMemory = VK_NULL_HANDLE; }
  width = height = 0;
  /* Null ctx so a second release() hits the early-out instead of reading
   * ctx->device through a freed pointer: Scene::~Scene calls release()
   * explicitly, deletes the VkContext, then the ~OffscreenTarget member dtor
   * calls release() again. Mirrors Swapchain::release. */
  ctx = nullptr;
}

void OffscreenTarget::beginRenderPass(VkCommandBuffer cb,
                                       float r,
                                       float g,
                                       float b,
                                       float a) const
{
  VkClearValue clears[2]{};
  clears[0].color = {{r, g, b, a}};
  clears[1].depthStencil = {1.0f, 0};

  VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  bi.renderPass = renderPass;
  bi.framebuffer = framebuffer;
  bi.renderArea.extent = {uint32_t(width), uint32_t(height)};
  bi.clearValueCount = 2;
  bi.pClearValues = clears;
  vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport vp{};
  vp.x = 0; vp.y = 0;
  vp.width = float(width);
  vp.height = float(height);
  vp.minDepth = 0; vp.maxDepth = 1;
  vkCmdSetViewport(cb, 0, 1, &vp);

  VkRect2D sc{};
  sc.extent = {uint32_t(width), uint32_t(height)};
  vkCmdSetScissor(cb, 0, 1, &sc);
}

} // namespace sculptcore::vulkan
