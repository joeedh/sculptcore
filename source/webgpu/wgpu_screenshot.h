#pragma once

namespace sculptcore::webgpu {

struct WgpuContext;
struct WgpuTarget;

/** Copy `target`'s color texture (must be RGBA8Unorm with CopySrc usage —
 *  WgpuTarget::create sets this) into a host-mappable buffer, un-pad the
 *  256-byte-aligned rows, and write a top-down PNG to `path`. Native only
 *  (drains via wgpuDevicePoll / wgpuInstanceProcessEvents). Returns true on
 *  success. Mirrors vulkan::captureToPNG.
 *
 *  If `outNonUniformPixels` is non-null it receives the count of pixels that
 *  differ from the top-left (background) pixel — a cheap "did anything draw?"
 *  signal for the parity test, avoiding a PNG re-read. */
bool captureTargetToPNG(const char *path, WgpuContext &ctx, WgpuTarget &target,
                        int *outNonUniformPixels = nullptr);

} // namespace sculptcore::webgpu
