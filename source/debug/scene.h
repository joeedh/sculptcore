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
#include "vdm/vdm_store.h"
#include "vulkan/vk_backend.h"
#include "vulkan/vk_context.h"
#include "vulkan/vk_overlay.h"
#include "vulkan/vk_swapchain.h"
#include "window/window.h"

#include "litestl/util/string.h"

#include <string>

namespace sculptcore::subdiv {
struct Multires;
}

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
  /* VDM tile store (vdm_init verb). Freed AFTER meshLog history is dropped —
   * VdmLogChunk entries hold non-owning store pointers. */
  vdm::VdmStore *vdm = nullptr;
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

  /* Mechanism B auto-defrag: if > 0, after each stroke (while its undo step is
   * still open) compact the mesh layout when the vert page-spread ratio exceeds
   * this. 0 = off. Set via the `auto_defrag` verb. */
  double autoDefragRatio = 0.0;

  /* Cumulative dyntopo op counts across `stroke` verbs (printed/reset by the
   * `dyntopo_stats` verb) — to measure id-order sensitivity of total work. */
  int64_t cumSplits = 0, cumCollapses = 0, cumFlips = 0;

  /* Non-accumulate sculpt mode (plans/nonAccumMode.md): deform dabs measure from
   * each vert's stroke-start position. `strokeGen` is bumped once per stroke verb
   * (so prior strokes' `.brush.orig.*` snapshots never collide) and pushed to the
   * executor + DynTopoParams.nonAccumGen. */
  bool nonAccum = false;
  uint32_t strokeGen = 0;

  /* Multires (displacementAndSubSurf S4): when set, `mesh` / `tree` are
   * NON-OWNING views of the active level's slot (the Multires owns them) and
   * the original mesh is parked as the cage. mrUndoLevels/mrRedoLevels record
   * which level each stroke verb's meshlog step was made on, so undo/redo can
   * auto-switch back to it. */
  subdiv::Multires *multires = nullptr;
  mesh::Mesh *multiresCage = nullptr;
  litestl::util::Vector<int> mrUndoLevels, mrRedoLevels;

  /** Point mesh/tree/meshLog at the active multires level's slot. */
  void attachMultiresLevel();
  /** Tear down the multires stack + cage; mesh/tree become null. */
  void clearMultires();

  void setMesh(mesh::Mesh *m);
  void buildSpatial(int leafLimit, int depthLimit, int gpu_tri_target);
  void smoothMesh();

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

  /** Optional depth-tested overlay hook, fired inside the swapchain render
   *  pass while the scene's view-projection / GPU manager / backend are live —
   *  after the axes+cursor overlays and before the ImGui post-draw hook. Lets
   *  callers submit world-space line geometry (e.g. field overlays) that the
   *  rendered surface occludes. Pass nullptr to clear. */
  using OverlayDrawCB = void (*)(void *user, gpu::GPUManager &mgr,
                                 vulkan::VulkanBackend &backend,
                                 const litestl::math::mat4 &vp);
  void setOverlayDrawCB(OverlayDrawCB cb, void *user)
  {
    overlayCB_ = cb;
    overlayUser_ = user;
  }

  /** Helper: write current offscreen color attachment to PNG. */
  bool screenshot(const char *path);

private:
  PostDrawHook postDrawHook_ = nullptr;
  void *postDrawUser_ = nullptr;
  OverlayDrawCB overlayCB_ = nullptr;
  void *overlayUser_ = nullptr;
  /* Last buildSpatial args, retained for a tree rebuild after reload. */
  int spatialLeaf_ = 0;
  int spatialDepth_ = 16;
  int spatialGpuTri_ = 0;
};

} // namespace sculptcore::debug_app
