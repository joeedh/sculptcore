#include "wgpu_screenshot.h"
#include "wgpu_context.h"

#ifndef __EMSCRIPTEN__

#include <webgpu/wgpu.h> // wgpu-native extensions: wgpuDevicePoll

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sculptcore::webgpu {

namespace {
constexpr uint32_t kRowAlign = 256; // WGPU copyTextureToBuffer bytesPerRow req.

struct MapReq {
  bool done = false;
  bool ok = false;
};

void onMap(WGPUMapAsyncStatus status, WGPUStringView message, void *ud1, void *)
{
  auto *r = static_cast<MapReq *>(ud1);
  r->ok = status == WGPUMapAsyncStatus_Success;
  if (!r->ok) {
    fprintf(stderr, "buffer mapAsync failed: %.*s\n",
            int(message.length), message.data ? message.data : "");
  }
  r->done = true;
}
} // namespace

bool captureTargetToPNG(const char *path, WgpuContext &ctx, WgpuTarget &target,
                        int *outNonUniformPixels)
{
  if (outNonUniformPixels) *outNonUniformPixels = 0;
  if (!ctx.device || target.width <= 0 || target.height <= 0 ||
      !target.colorTexture) {
    return false;
  }

  const uint32_t w = uint32_t(target.width);
  const uint32_t h = uint32_t(target.height);
  const uint32_t unpaddedRow = w * 4;
  const uint32_t paddedRow = (unpaddedRow + kRowAlign - 1) / kRowAlign * kRowAlign;
  const uint64_t bufSize = uint64_t(paddedRow) * h;

  WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
  bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
  bd.size = bufSize;
  WGPUBuffer staging = wgpuDeviceCreateBuffer(ctx.device, &bd);
  if (!staging) return false;

  WGPUCommandEncoderDescriptor ced = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(ctx.device, &ced);

  WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
  src.texture = target.colorTexture;

  WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
  dst.buffer = staging;
  dst.layout.offset = 0;
  dst.layout.bytesPerRow = paddedRow;
  dst.layout.rowsPerImage = h;

  WGPUExtent3D extent{w, h, 1};
  wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &extent);

  WGPUCommandBufferDescriptor cbd = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, &cbd);
  wgpuQueueSubmit(ctx.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);

  // Drain the submit so the copy lands before we map.
  wgpuDevicePoll(ctx.device, /*wait=*/true, nullptr);

  MapReq mreq;
  WGPUBufferMapCallbackInfo mci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
  mci.mode = WGPUCallbackMode_AllowProcessEvents;
  mci.callback = onMap;
  mci.userdata1 = &mreq;
  wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, bufSize, mci);
  while (!mreq.done) {
    wgpuDevicePoll(ctx.device, /*wait=*/true, nullptr);
    wgpuInstanceProcessEvents(ctx.instance);
  }

  bool ok = false;
  if (mreq.ok) {
    const auto *mapped =
        static_cast<const unsigned char *>(wgpuBufferGetConstMappedRange(staging, 0, bufSize));
    if (mapped) {
      auto *pixels = static_cast<unsigned char *>(std::malloc(size_t(unpaddedRow) * h));
      if (pixels) {
        for (uint32_t y = 0; y < h; y++) {
          std::memcpy(pixels + size_t(y) * unpaddedRow,
                      mapped + size_t(y) * paddedRow, unpaddedRow);
        }
        if (outNonUniformPixels) {
          int count = 0;
          const unsigned char *bg = pixels; // top-left = background
          for (uint32_t i = 0; i < w * h; i++) {
            const unsigned char *px = pixels + size_t(i) * 4;
            if (px[0] != bg[0] || px[1] != bg[1] || px[2] != bg[2]) count++;
          }
          *outNonUniformPixels = count;
        }
        ok = stbi_write_png(path, int(w), int(h), 4, pixels, int(unpaddedRow)) != 0;
        if (!ok) fprintf(stderr, "captureTargetToPNG: stbi_write_png failed for '%s'\n", path);
        std::free(pixels);
      }
    }
    wgpuBufferUnmap(staging);
  }

  wgpuBufferRelease(staging);
  return ok;
}

} // namespace sculptcore::webgpu

#endif // !__EMSCRIPTEN__
