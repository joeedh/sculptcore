#pragma once

#include "camera.h"
#include "profile.h"

#include "brush/brush.h"
#include "brush/brushes/all.h"
#include "dyntopo/dyntopo.h"
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
// Brush backend selector.
//   Cpp        — reference C++ executor (default).
//   Wgsl       — real GPU compute through the Vulkan dispatcher (SPIR-V
//                kernels). Supports the GPU-resident live-render path.
//   WgpuNative — real GPU compute through webgpu.h / wgpu-native (the .wgsl
//                kernels). Batch/readback only: it can't share buffers with
//                the Vulkan renderer, so dabs read back to the CPU mesh and the
//                Vulkan path redraws. Compiled in only with SBRUSH_WEBGPU_COMPUTE.
enum class BrushBackend { Cpp, Wgsl, WgpuNative };

// How the GPU-resident WGSL stroke path (debug app, interactive) finalizes
// vertex normals on stroke release. Cpu (default) recomputes them with the
// exact per-node CPU algorithm — byte-identical to the C++ backend, so
// sbrush-verify is unaffected. Gpu reads back the compute pass's global-sum
// normals (cheaper, skips the CPU 1-ring gather) at the cost of not being
// bit-identical. Mid-stroke shading always uses the GPU normals regardless.
enum class StrokeEndNormals { Cpu, Gpu };

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
  /* When set, the C++ executor enumerates 1-ring neighbors from the cached CSR
   * (MeshTopoCache) rather than the live disk walk — selects the CsrNbr kernel
   * instantiation. Used by the A/B harness to verify the two sources agree. */
  bool useCsrNeighbors = false;
  StrokeEndNormals strokeEndNormals = StrokeEndNormals::Cpu;
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
  /* Wall-clock stroke/dab timing, enabled by --profile. No-op when disabled. */
  StrokeProfiler profiler;

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

  /* When set, buildSpatial reorders mesh elements for node locality right
   * after the tree is built (debug --reorder / the UI button). */
  bool reorderOnBuild = false;

  /* Dynamic-topology config (the `dyntopo` script verb). When enabled, the
   * C++ stroke path remeshes the mesh under each dab before brushing. */
  bool dyntopoEnabled = false;
  dyntopo::DynTopoParams dyntopoParams;
  uint32_t dyntopoSeed = 1;

  void setMesh(mesh::Mesh *m);
  void buildSpatial(int leafLimit, int depthLimit, int gpu_tri_target);

  /* Remesh the mesh under one dab (sphere center/radius) and rebuild the
   * spatial tree with the last buildSpatial settings. Returns split+collapse
   * count; no-op when dyntopo is disabled or there is no mesh. This is the
   * debug-app integration; the in-executor incremental-spatial path is a
   * follow-up (plan M2 integration #5). */
  int applyDynTopoDab(litestl::math::float3 center, float radius, uint32_t seed);

  /* Reorder all mesh element domains to be local to their owning spatial
   * nodes, rebuild the tree, and record an undoable reorder step. No-op
   * without a tree. */
  void reorderForLocality();

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
  /* Last buildSpatial args, replayed by applyDynTopoDab's tree rebuild. */
  int spatialLeaf_ = 0;
  int spatialDepth_ = 16;
  int spatialGpuTri_ = 0;
};

} // namespace sculptcore::debug_app
