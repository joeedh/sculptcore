#include "vk_screenshot.h"
#include "vk_context.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sculptcore::vulkan {

bool captureToPNG(const char *path, VkContext &ctx, OffscreenTarget &target)
{
  if (target.width <= 0 || target.height <= 0) return false;

  /* Linear, host-visible image we can vkCmdCopyImage into and map. */
  VkImage dstImage = VK_NULL_HANDLE;
  VkDeviceMemory dstMem = VK_NULL_HANDLE;

  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = target.colorFormat;
  ici.extent = {uint32_t(target.width), uint32_t(target.height), 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_LINEAR;
  ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(ctx.device, &ici, nullptr, &dstImage) != VK_SUCCESS) return false;

  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(ctx.device, dstImage, &mr);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = ctx.findMemoryType(
      mr.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (mai.memoryTypeIndex == ~0u ||
      vkAllocateMemory(ctx.device, &mai, nullptr, &dstMem) != VK_SUCCESS) {
    vkDestroyImage(ctx.device, dstImage, nullptr);
    return false;
  }
  vkBindImageMemory(ctx.device, dstImage, dstMem, 0);

  ctx.runOneShot([&](VkCommandBuffer cb) {
    /* dst: UNDEFINED -> TRANSFER_DST_OPTIMAL. */
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.image = dstImage;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,
                         0, nullptr, 0, nullptr, 1, &b);

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {uint32_t(target.width), uint32_t(target.height), 1};
    vkCmdCopyImage(cb,
                   target.colorImage,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dstImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1,
                   &region);

    /* dst: TRANSFER_DST -> GENERAL so we can map and read. */
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0,
                         0, nullptr, 0, nullptr, 1, &b);
  });

  /* Walk the host-visible image and copy rows into a packed buffer. */
  VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
  VkSubresourceLayout layout;
  vkGetImageSubresourceLayout(ctx.device, dstImage, &sub, &layout);
  void *p = nullptr;
  vkMapMemory(ctx.device, dstMem, 0, VK_WHOLE_SIZE, 0, &p);
  size_t row_bytes = size_t(target.width) * 4;
  unsigned char *pixels = static_cast<unsigned char *>(std::malloc(row_bytes * size_t(target.height)));
  bool ok = false;
  if (pixels) {
    const unsigned char *src = static_cast<const unsigned char *>(p) + layout.offset;
    for (int y = 0; y < target.height; y++) {
      std::memcpy(pixels + size_t(y) * row_bytes,
                  src + size_t(y) * layout.rowPitch,
                  row_bytes);
    }
    ok = stbi_write_png(path, target.width, target.height, 4, pixels, int(row_bytes)) != 0;
    if (!ok) fprintf(stderr, "captureToPNG: stbi_write_png failed for '%s'\n", path);
    std::free(pixels);
  }
  vkUnmapMemory(ctx.device, dstMem);
  vkDestroyImage(ctx.device, dstImage, nullptr);
  vkFreeMemory(ctx.device, dstMem, nullptr);
  return ok;
}

} // namespace sculptcore::vulkan
