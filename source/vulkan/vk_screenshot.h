#pragma once

namespace sculptcore::vulkan {

struct VkContext;
struct OffscreenTarget;

/** Read the color attachment of `target` (which must be in
 *  TRANSFER_SRC_OPTIMAL — the render pass transitions it there) and write
 *  it to `path` as a top-down PNG. Returns true on success. */
bool captureToPNG(const char *path, VkContext &ctx, OffscreenTarget &target);

} // namespace sculptcore::vulkan
