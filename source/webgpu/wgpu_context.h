#pragma once

#include <webgpu/webgpu.h>

#include "litestl/util/string.h"

namespace sculptcore::webgpu {

/** Owns the WebGPU instance + device + queue (+ optional canvas surface) that
 *  the WebGpuBackend borrows. Single-device, single-queue — the analogue of
 *  vulkan::VkContext.
 *
 *  On WASM the device is created JS-side (navigator.gpu) and handed to us via
 *  emscripten_webgpu_get_device(); we only adopt it. On native (Phase 2) the
 *  device would be created through Dawn — not wired yet. */
struct WgpuContext {
  WgpuContext() = default;
  WgpuContext(const WgpuContext &) = delete;
  ~WgpuContext();

  /** WASM: adopt the JS-created device (Module.preinitializedWebGPUDevice),
   *  fetch its default queue, and create the implicit instance. Returns false
   *  if no device was preinitialized. */
  bool initFromEmscripten();

  /** Native: create an instance, request an adapter + device through
   *  wgpu-native (pumping wgpuInstanceProcessEvents until the async callbacks
   *  fire), and fetch the default queue. No surface — the native path renders
   *  offscreen (WgpuTarget) for screenshot parity. Returns false on failure. */
  bool initNative();

  /** Create a surface bound to the canvas matching `cssSelector` (e.g.
   *  "#canvas"). Must be called after init*. Stores it on `surface`. */
  bool createCanvasSurface(const char *cssSelector);

  /** Configure (or reconfigure) the surface swap-chain at w×h with `format`.
   *  No-op if there is no surface. */
  void configureSurface(int w, int h, WGPUTextureFormat format);

  WGPUInstance instance = nullptr;
  WGPUDevice device = nullptr;
  WGPUQueue queue = nullptr;

  WGPUSurface surface = nullptr;
  WGPUTextureFormat surfaceFormat = WGPUTextureFormat_BGRA8Unorm;
  int surfaceWidth = 0;
  int surfaceHeight = 0;
};

/** Renderable offscreen target: color + depth textures and their views. The
 *  surface (swap-chain) render path doesn't use this — it acquires the color
 *  view per-frame and the backend owns a matching depth texture. Mirrors
 *  vulkan::OffscreenTarget. */
struct WgpuTarget {
  WgpuTarget() = default;
  WgpuTarget(const WgpuTarget &) = delete;
  ~WgpuTarget() { release(); }

  /** Allocate color (`colorFormat`, RenderAttachment|CopySrc) + depth
   *  (depth24plus, RenderAttachment) at w×h. */
  bool create(WgpuContext *ctx, int w, int h,
              WGPUTextureFormat colorFormat = WGPUTextureFormat_RGBA8Unorm);
  void release();

  WgpuContext *ctx = nullptr;
  int width = 0;
  int height = 0;

  WGPUTextureFormat colorFormat = WGPUTextureFormat_RGBA8Unorm;
  WGPUTextureFormat depthFormat = WGPUTextureFormat_Depth24Plus;

  WGPUTexture colorTexture = nullptr;
  WGPUTextureView colorView = nullptr;
  WGPUTexture depthTexture = nullptr;
  WGPUTextureView depthView = nullptr;
};

} // namespace sculptcore::webgpu
