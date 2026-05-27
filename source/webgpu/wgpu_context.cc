#include "wgpu_context.h"

#include <cstdio>
#include <cstring>

/* emscripten_webgpu_get_device() is declared by emdawnwebgpu's webgpu.h
 * itself (the deprecated html5_webgpu.h interop header is gone). */

namespace sculptcore::webgpu {

[[maybe_unused]] static WGPUStringView strView(const char *s)
{
  WGPUStringView v{};
  v.data = s;
  v.length = s ? std::strlen(s) : 0;
  return v;
}

WgpuContext::~WgpuContext()
{
  if (surface) wgpuSurfaceRelease(surface);
  if (queue) wgpuQueueRelease(queue);
  if (device) wgpuDeviceRelease(device);
  if (instance) wgpuInstanceRelease(instance);
}

bool WgpuContext::initFromEmscripten()
{
#ifdef __EMSCRIPTEN__
  device = emscripten_webgpu_get_device();
  if (!device) {
    fprintf(stderr, "WgpuContext: no preinitialized WebGPU device\n");
    return false;
  }
  queue = wgpuDeviceGetQueue(device);
  instance = wgpuCreateInstance(nullptr);
  return queue != nullptr;
#else
  fprintf(stderr, "WgpuContext::initFromEmscripten: native path not wired\n");
  return false;
#endif
}

#ifndef __EMSCRIPTEN__
namespace {

/* wgpu-native fires request callbacks either spontaneously or inside
 * wgpuInstanceProcessEvents (mode = AllowProcessEvents). We block on a small
 * pump loop until the callback has run. */
struct AdapterReq {
  WGPUAdapter adapter = nullptr;
  bool done = false;
};
struct DeviceReq {
  WGPUDevice device = nullptr;
  bool done = false;
};

void onAdapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
               WGPUStringView message, void *ud1, void *)
{
  auto *r = static_cast<AdapterReq *>(ud1);
  if (status == WGPURequestAdapterStatus_Success) {
    r->adapter = adapter;
  } else {
    fprintf(stderr, "requestAdapter failed: %.*s\n",
            int(message.length), message.data ? message.data : "");
  }
  r->done = true;
}

void onDevice(WGPURequestDeviceStatus status, WGPUDevice device,
              WGPUStringView message, void *ud1, void *)
{
  auto *r = static_cast<DeviceReq *>(ud1);
  if (status == WGPURequestDeviceStatus_Success) {
    r->device = device;
  } else {
    fprintf(stderr, "requestDevice failed: %.*s\n",
            int(message.length), message.data ? message.data : "");
  }
  r->done = true;
}

void onUncapturedError(const WGPUDevice *, WGPUErrorType type,
                       WGPUStringView message, void *, void *)
{
  fprintf(stderr, "WebGPU uncaptured error (%d): %.*s\n", int(type),
          int(message.length), message.data ? message.data : "");
}

} // namespace
#endif

bool WgpuContext::initNative()
{
#ifdef __EMSCRIPTEN__
  fprintf(stderr, "WgpuContext::initNative: not available under WASM\n");
  return false;
#else
  instance = wgpuCreateInstance(nullptr);
  if (!instance) {
    fprintf(stderr, "WgpuContext::initNative: wgpuCreateInstance failed\n");
    return false;
  }

  AdapterReq areq;
  WGPURequestAdapterCallbackInfo aci = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
  aci.mode = WGPUCallbackMode_AllowProcessEvents;
  aci.callback = onAdapter;
  aci.userdata1 = &areq;
  WGPURequestAdapterOptions aopts = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
  aopts.powerPreference = WGPUPowerPreference_HighPerformance;
  wgpuInstanceRequestAdapter(instance, &aopts, aci);
  while (!areq.done) {
    wgpuInstanceProcessEvents(instance);
  }
  if (!areq.adapter) return false;

  // Request the adapter's max storage-buffers-per-stage: the for_neighbor
  // compute kernels (e.g. smooth) bind 10 storage buffers, over the default
  // limit of 8. Other limits stay at default (WGPU_LIMIT_*_UNDEFINED).
  WGPULimits adapterLimits = WGPU_LIMITS_INIT;
  wgpuAdapterGetLimits(areq.adapter, &adapterLimits);
  WGPULimits requiredLimits = WGPU_LIMITS_INIT;
  requiredLimits.maxStorageBuffersPerShaderStage =
      adapterLimits.maxStorageBuffersPerShaderStage;

  DeviceReq dreq;
  WGPURequestDeviceCallbackInfo dci = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
  dci.mode = WGPUCallbackMode_AllowProcessEvents;
  dci.callback = onDevice;
  dci.userdata1 = &dreq;
  WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
  dd.requiredLimits = &requiredLimits;
  dd.uncapturedErrorCallbackInfo.callback = onUncapturedError;
  wgpuAdapterRequestDevice(areq.adapter, &dd, dci);
  while (!dreq.done) {
    wgpuInstanceProcessEvents(instance);
  }
  wgpuAdapterRelease(areq.adapter);
  if (!dreq.device) return false;

  device = dreq.device;
  queue = wgpuDeviceGetQueue(device);
  return queue != nullptr;
#endif
}

bool WgpuContext::createCanvasSurface(const char *cssSelector)
{
#ifndef __EMSCRIPTEN__
  (void)cssSelector;
  fprintf(stderr, "WgpuContext::createCanvasSurface: WASM-only\n");
  return false;
#else
  if (!instance) {
    fprintf(stderr, "WgpuContext::createCanvasSurface: no instance\n");
    return false;
  }
  WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasSrc =
      WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
  canvasSrc.selector = strView(cssSelector);

  WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;
  sd.nextInChain = &canvasSrc.chain;

  surface = wgpuInstanceCreateSurface(instance, &sd);
  if (!surface) {
    fprintf(stderr, "WgpuContext::createCanvasSurface: failed for '%s'\n", cssSelector);
    return false;
  }
  return true;
#endif
}

void WgpuContext::configureSurface(int w, int h, WGPUTextureFormat format)
{
  if (!surface || w <= 0 || h <= 0) return;
  surfaceFormat = format;
  surfaceWidth = w;
  surfaceHeight = h;

  WGPUSurfaceConfiguration cfg = WGPU_SURFACE_CONFIGURATION_INIT;
  cfg.device = device;
  cfg.format = format;
  cfg.usage = WGPUTextureUsage_RenderAttachment;
  cfg.width = uint32_t(w);
  cfg.height = uint32_t(h);
  cfg.alphaMode = WGPUCompositeAlphaMode_Opaque;
  wgpuSurfaceConfigure(surface, &cfg);
}

bool WgpuTarget::create(WgpuContext *ctx_, int w, int h, WGPUTextureFormat fmt)
{
  release();
  ctx = ctx_;
  width = w;
  height = h;
  colorFormat = fmt;
  if (!ctx || !ctx->device || w <= 0 || h <= 0) return false;

  WGPUExtent3D extent{uint32_t(w), uint32_t(h), 1};

  WGPUTextureDescriptor ctd = WGPU_TEXTURE_DESCRIPTOR_INIT;
  ctd.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
  ctd.dimension = WGPUTextureDimension_2D;
  ctd.size = extent;
  ctd.format = colorFormat;
  colorTexture = wgpuDeviceCreateTexture(ctx->device, &ctd);

  WGPUTextureDescriptor dtd = WGPU_TEXTURE_DESCRIPTOR_INIT;
  dtd.usage = WGPUTextureUsage_RenderAttachment;
  dtd.dimension = WGPUTextureDimension_2D;
  dtd.size = extent;
  dtd.format = depthFormat;
  depthTexture = wgpuDeviceCreateTexture(ctx->device, &dtd);

  if (!colorTexture || !depthTexture) {
    release();
    return false;
  }
  colorView = wgpuTextureCreateView(colorTexture, nullptr);
  depthView = wgpuTextureCreateView(depthTexture, nullptr);
  return colorView && depthView;
}

void WgpuTarget::release()
{
  if (colorView) { wgpuTextureViewRelease(colorView); colorView = nullptr; }
  if (depthView) { wgpuTextureViewRelease(depthView); depthView = nullptr; }
  if (colorTexture) { wgpuTextureRelease(colorTexture); colorTexture = nullptr; }
  if (depthTexture) { wgpuTextureRelease(depthTexture); depthTexture = nullptr; }
  width = height = 0;
}

} // namespace sculptcore::webgpu
