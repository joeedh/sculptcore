#pragma once

#include "camera.h"

#include "brush/brush.h"
#include "brush/brushes/all.h"
#include "gpu/manager.h"
#include "mesh/mesh.h"
#include "meshlog/meshlog_base.h"
#include "spatial/spatial.h"
#include "vulkan/vk_backend.h"
#include "vulkan/vk_context.h"
#include "vulkan/vk_overlay.h"
#include "vulkan/vk_swapchain.h"
#include "window/window.h"

#include "litestl/util/string.h"

#include <string>

namespace sculptcore::debug_app {

enum class ViewPreset { Front, Top, Side, Persp, Free };

struct LastStroke {
  bool valid = false;
  litestl::math::float3 origin{0, 0, 0};
  litestl::math::float3 normal{0, 0, 1};
  float radius = 0.0f;
};

/** Owns one full debug-app scene: mesh + spatial accelerator + brush +
 *  GPU manager + Vulkan backend + window. All optional pieces are lazily
 *  created so a script that never asks for screenshots never opens a
 *  Vulkan device. Interactive mode currently shares the offscreen target
 *  with the headless path — a swapchain-presented window is a follow-up. */
// Brush backend selector. Wave 3 implements WGSL emit + tint validation
// but does NOT yet have a WebGPU runtime native — selecting Wgsl is a
// gate that asserts the .wgsl artifacts exist (i.e. SBRUSH_BACKEND_WGSL
// was ON at configure time); the actual sculpting still runs through the
// C++ executor. Later waves replace that with real GPU dispatch.
enum class BrushBackend { Cpp, Wgsl };

struct Scene {
  Scene(int width, int height, bool headless);
  Scene(const Scene &) = delete;
  ~Scene();

  /** Bring up window (if needed), VkContext, OffscreenTarget, and backend. */
  bool ensureGPU();

  mesh::Mesh *mesh = nullptr;
  spatial::SpatialTree *tree = nullptr;
  brush::Brush brush;
  brush::SculptBrushes currentTool = brush::SculptBrushes::DRAW;
  BrushBackend currentBackend = BrushBackend::Cpp;
  /* Texture coord-space matrix for VIEWPLANE/VIEWREPEAT (set_render_matrix).
   * Identity'd in the ctor; threaded into both the C++ ctx.renderMatrix and the
   * GPU ctx uniform so the matrix-driven coord spaces are deterministic. */
  mat4 renderMatrix;
  /* When non-empty, the GPU dispatch path (runBrushStrokeGPU) writes a JSON
   * fixture per wgsl stroke capturing the exact per-binding buffer bytes and
   * the final readback, for the Dawn/WebGPU replay harness (--gpu-capture). */
  std::string gpuCapturePrefix;
  meshlog::MeshLog meshLog;
  gpu::GPUManager gpu;
  Camera camera;
  ViewPreset view = ViewPreset::Persp;
  LastStroke lastStroke;
  bool showLeafBounds = false;
  bool showAxes = true;
  bool showCursor = true;

  /* Owned GPU bits (created on first ensureGPU()). */
  window::Window *window = nullptr;
  vulkan::VkContext *context = nullptr;
  vulkan::VulkanBackend *backend = nullptr;        /* offscreen render pass */
  vulkan::VulkanBackend *backendWindow = nullptr;  /* swapchain render pass; only when !headless */
  vulkan::OffscreenTarget offscreen;
  vulkan::Swapchain swapchain;
  vulkan::Overlay overlay;

  int width;
  int height;
  bool headless;

  void setMesh(mesh::Mesh *m);
  void buildSpatial(int leafLimit, int depthLimit, int gpu_tri_target);

  /** Center camera + set view direction from preset; uses mesh AABB. */
  void applyView(ViewPreset preset);

  /** Render into the offscreen target. */
  void renderHeadless();

  /** Acquire a swapchain image, draw the scene + overlay (+ optional
   *  pre-pass and ImGui hook), and present. Returns false if the
   *  swapchain went out-of-date and was recreated. */
  void renderWindow();

  /** Recreate the swapchain to match the GLFW framebuffer size. Called
   *  from the resize callback and on present-out-of-date. */
  void handleResize();

  /** Optional hook recorded inside the swapchain render pass, after the
   *  scene+overlay draw and before vkCmdEndRenderPass. Used to plug in
   *  ImGui draw data. Pass nullptr to clear. */
  using PostDrawHook = void (*)(void *user, VkCommandBuffer cb);
  void setPostDrawHook(PostDrawHook hook, void *user)
  {
    postDrawHook_ = hook;
    postDrawUser_ = user;
  }

  /** Helper: write current offscreen color attachment to PNG. */
  bool screenshot(const char *path);

private:
  PostDrawHook postDrawHook_ = nullptr;
  void *postDrawUser_ = nullptr;
};

} // namespace sculptcore::debug_app
