#include "scene.h"

#include "gpu/batch.h"
#include "litestl/util/alloc.h"
#include "vulkan/vk_screenshot.h"

#include <cstdio>

namespace sculptcore::debug_app {

using litestl::math::float3;
using litestl::math::mat4;

Scene::Scene(int w, int h, bool hl) : width(w), height(h), headless(hl) {}

Scene::~Scene()
{
  /* Release backend BEFORE the VkContext goes away. Otherwise the cached
   * Vulkan handles would dangle. */
  if (backend) {
    backend->invalidate();
    delete backend;
    backend = nullptr;
  }
  overlay.release();
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
  window::WindowOptions opts;
  opts.visible = !headless;
  opts.resizable = !headless;
  window = new window::Window(litestl::math::float2(float(width), float(height)), opts);
  if (!window->init()) {
    fprintf(stderr, "Scene::ensureGPU: window init failed\n");
    return false;
  }
  context = new vulkan::VkContext();
  /* Pass the GLFW window only in interactive mode — surface creation is
   * required for a future swapchain. Headless skips it. */
  GLFWwindow *handle = headless ? nullptr : window->handle();
  if (!context->init(handle, true)) {
    fprintf(stderr, "Scene::ensureGPU: VkContext init failed\n");
    return false;
  }
  if (!offscreen.create(context, width, height)) {
    fprintf(stderr, "Scene::ensureGPU: OffscreenTarget create failed\n");
    return false;
  }
  backend = new vulkan::VulkanBackend(&gpu, context, offscreen.renderPass);
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

void Scene::buildSpatial(int leafLimit, int depthLimit)
{
  if (!mesh) {
    fprintf(stderr, "Scene::buildSpatial: no mesh\n");
    return;
  }
  if (tree) {
    litestl::alloc::Delete(tree);
  }
  tree = litestl::alloc::New<spatial::SpatialTree>("SpatialTree (debug)", mesh);
  tree->leaf_limit = leafLimit;
  tree->depth_limit = depthLimit;
  tree->buildAll();
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
    overlay.drawAxes(vp, 1.0f);
  }
  if (showCursor && lastStroke.valid) {
    overlay.drawBrushCursor(vp, lastStroke.origin, lastStroke.normal, lastStroke.radius);
  }
  backend->endFrame();
}

void Scene::renderWindow()
{
  /* Interactive presentation requires a swapchain; not yet wired up. Render
   * into the offscreen target so the rest of the pipeline exercises Vulkan
   * end-to-end. The visible GLFW window stays blank for now. */
  renderHeadless();
}

bool Scene::screenshot(const char *path)
{
  if (!ensureGPU()) {
    return false;
  }
  return vulkan::captureToPNG(path, *context, offscreen);
}

} // namespace sculptcore::debug_app
