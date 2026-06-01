#include "scene.h"

#include "gpu/batch.h"
#include "litestl/util/alloc.h"
#include "vulkan/vk_screenshot.h"

#include <cstdio>

namespace sculptcore::debug_app {

using litestl::math::float3;
using litestl::math::mat4;

Scene::Scene(int w, int h, bool hl) : width(w), height(h), headless(hl)
{
  renderMatrix.identity();
}

Scene::~Scene()
{
  /* Release backends BEFORE the VkContext goes away. Otherwise the cached
   * Vulkan handles would dangle. */
  if (backendWindow) {
    backendWindow->invalidate();
    delete backendWindow;
    backendWindow = nullptr;
  }
  if (backend) {
    backend->invalidate();
    delete backend;
    backend = nullptr;
  }
  swapchain.release();
  offscreen.release();
  if (context) {
    delete context;
    context = nullptr;
  }
  if (window) {
    delete window;
    window = nullptr;
  }
  if (tree) {
    litestl::alloc::Delete(tree);
    tree = nullptr;
  }
  if (mesh) {
    litestl::alloc::Delete(mesh);
    mesh = nullptr;
  }
}

bool Scene::ensureGPU()
{
  if (backend) {
    return true;
  }
  // Headless never touches GLFW: a displayless container aborts in glfwInit().
  // VkContext::init(nullptr) + OffscreenTarget render entirely off-screen.
  if (!headless) {
    window::WindowOptions opts;
    opts.visible = true;
    opts.resizable = true;
    window = new window::Window(litestl::math::float2(float(width), float(height)), opts);
    if (!window->init()) {
      fprintf(stderr, "Scene::ensureGPU: window init failed\n");
      return false;
    }
  }
  context = new vulkan::VkContext();
  GLFWwindow *handle = (headless || !window) ? nullptr : window->handle();
  if (!context->init(handle, true)) {
    fprintf(stderr, "Scene::ensureGPU: VkContext init failed\n");
    return false;
  }
  if (!offscreen.create(context, width, height)) {
    fprintf(stderr, "Scene::ensureGPU: OffscreenTarget create failed\n");
    return false;
  }
  backend = new vulkan::VulkanBackend(&gpu, context, offscreen.renderPass);

  if (!headless) {
    int fbw = width, fbh = height;
    window->framebufferSize(fbw, fbh);
    if (!swapchain.create(context, fbw, fbh)) {
      fprintf(stderr, "Scene::ensureGPU: Swapchain create failed\n");
      return false;
    }
    backendWindow = new vulkan::VulkanBackend(&gpu, context, swapchain.renderPass);
  }
  return true;
}

void Scene::setMesh(mesh::Mesh *m)
{
  if (tree) {
    litestl::alloc::Delete(tree);
    tree = nullptr;
  }
  if (mesh) {
    litestl::alloc::Delete(mesh);
  }
  mesh = m;
  meshLog.setActiveMesh(m);
}

void Scene::buildSpatial(int leafLimit, int depthLimit, int gpuPrimLimit)
{
  if (!mesh) {
    fprintf(stderr, "Scene::buildSpatial: no mesh\n");
    return;
  }
  if (tree) {
    litestl::alloc::Delete(tree);
  }
  spatialLeaf_ = leafLimit;
  spatialDepth_ = depthLimit;
  spatialGpuTri_ = gpuPrimLimit;
  tree = litestl::alloc::New<spatial::SpatialTree>("SpatialTree (debug)", mesh);
  /* Mesh-size-derived defaults; any positive explicit arg overrides. A <=0 arg
   * means "auto" for that knob. */
  tree->autoTuneLimits();
  if (leafLimit > 0) {
    tree->leaf_limit = leafLimit;
  }
  if (depthLimit > 0) {
    tree->depth_limit = depthLimit;
  }
  if (gpuPrimLimit > 0) {
    tree->gpu_tri_target = gpuPrimLimit;
  }
  tree->buildAll();

  if (reorderOnBuild) {
    reorderForLocality();
  }
}

int Scene::applyDynTopoDab(litestl::math::float3 center, float radius, uint32_t seed)
{
  if (!dyntopoEnabled || !mesh) {
    return 0;
  }
  /* dyntopo walks live disk/radial links; a prior stroke may have frozen the
   * topology (TOPO pages freed). Thaw first (no-op when not frozen). */
  mesh->thawTopo();
  dyntopo::DynTopoStats st =
      dyntopo::applyBrushDab(*mesh, center, radius, dyntopoParams, seed);
  /* Topology changed underneath the tree; rebuild it with the same settings
   * the last buildSpatial used so subsequent queries see fresh geometry. */
  if (tree) {
    buildSpatial(spatialLeaf_, spatialDepth_, spatialGpuTri_);
  }
  return st.splits + st.collapses;
}

void Scene::reorderForLocality()
{
  if (!tree) {
    return;
  }

  litestl::util::Vector<int> vmap, emap, cmap, lmap, fmap;
  tree->computeLocalityMaps(vmap, emap, cmap, lmap, fmap);

  meshLog.beginStep();
  meshLog.pushReorderChunk(vmap, emap, cmap, lmap, fmap);
  tree->applyReorder(vmap, emap, cmap, lmap, fmap);
  meshLog.endStep();
}

void Scene::applyView(ViewPreset preset)
{
  view = preset;
  if (!mesh) {
    return;
  }
  float3 mn, mx;
  mesh->calcAABB(mn, mx);
  float3 dir;
  switch (preset) {
  case ViewPreset::Front: dir = float3(0, -1, 0); break;
  case ViewPreset::Top:   dir = float3(0, 0, 1);  break;
  case ViewPreset::Side:  dir = float3(1, 0, 0);  break;
  case ViewPreset::Persp: dir = float3(1, 1, 1);  break;
  case ViewPreset::Free:  return; /* leave camera as-is */
  }
  camera.frame(mn, mx, dir);
}

void Scene::renderHeadless()
{
  if (!ensureGPU()) {
    return;
  }
  if (!backend->beginFrame(offscreen, 0.10f, 0.11f, 0.13f, 1.0f)) {
    return;
  }

  if (!mesh || !tree) {
    backend->endFrame();
    return;
  }

  float aspect = float(width) / float(height);
  mat4 vp = camera.viewProj(aspect);

  vulkan::DrawUniforms u;
  u.drawMatrix = vp;
  u.normalMatrix.identity();

  tree->update(&gpu);
  backend->draw(tree->getDrawBatch(), u);

  if (showLeafBounds) {
    gpu::DrawBatch *lines = tree->buildLeafBoundsBatch(gpu);
    backend->draw(lines, u);
    gpu.destroyBatch(lines, true, true);
  }
  if (showAxes) {
    overlay.drawAxes(gpu, *backend, vp, 1.0f);
  }
  if (showCursor && lastStroke.valid) {
    overlay.drawBrushCursor(gpu, *backend, vp,
                            lastStroke.origin, lastStroke.normal, lastStroke.radius);
  }
  backend->endFrame();
}

void Scene::handleResize()
{
  if (!backendWindow) {
    return;
  }
  int fbw = 0, fbh = 0;
  window->framebufferSize(fbw, fbh);
  if (fbw <= 0 || fbh <= 0) {
    return; /* minimised */
  }
  /* The render pass identity might change on recreate, so we have to
   * rebuild the swapchain backend's pipelines too. */
  backendWindow->invalidate();
  delete backendWindow;
  backendWindow = nullptr;
  if (!swapchain.recreate(fbw, fbh)) {
    fprintf(stderr, "Scene::handleResize: swapchain recreate failed\n");
    return;
  }
  backendWindow = new vulkan::VulkanBackend(&gpu, context, swapchain.renderPass);
  width = swapchain.width;
  height = swapchain.height;
}

void Scene::renderWindow()
{
  if (!ensureGPU() || !backendWindow) {
    return;
  }
  uint32_t imageIndex = 0;
  if (!swapchain.acquireNext(imageIndex)) {
    handleResize();
    return;
  }

  if (!backendWindow->beginFrameSwapchain(swapchain, imageIndex,
                                          0.10f, 0.11f, 0.13f, 1.0f)) {
    return;
  }

  if (mesh && tree) {
    float aspect = float(swapchain.width) / float(swapchain.height);
    mat4 vp = camera.viewProj(aspect);
    vulkan::DrawUniforms u;
    u.drawMatrix = vp;
    u.normalMatrix.identity();

    tree->update(&gpu);
    backendWindow->draw(tree->getDrawBatch(), u);

    if (showLeafBounds) {
      gpu::DrawBatch *lines = tree->buildLeafBoundsBatch(gpu);
      backendWindow->draw(lines, u);
      gpu.destroyBatch(lines, true, true);
    }
    if (showAxes) {
      overlay.drawAxes(gpu, *backendWindow, vp, 1.0f);
    }
    if (showCursor && lastStroke.valid) {
      overlay.drawBrushCursor(gpu, *backendWindow, vp,
                              lastStroke.origin, lastStroke.normal, lastStroke.radius);
    }
  }

  if (postDrawHook_) {
    postDrawHook_(postDrawUser_, backendWindow->activeCommandBuffer());
  }

  if (!backendWindow->endFrameSwapchain(swapchain, imageIndex)) {
    handleResize();
  }
}

bool Scene::screenshot(const char *path)
{
  if (!ensureGPU()) {
    return false;
  }
  return vulkan::captureToPNG(path, *context, offscreen);
}

} // namespace sculptcore::debug_app
