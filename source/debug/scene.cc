#include "scene.h"

#include "gpu/batch.h"
#include "litestl/util/alloc.h"
#include "opengl/gl_screenshot.h"

#include <cstdio>

namespace sculptcore::debug_app {

using litestl::math::float3;
using litestl::math::mat4;

Scene::Scene(int w, int h, bool hl) : width(w), height(h), headless(hl) {}

Scene::~Scene()
{
  /* Release backend BEFORE the GL context goes away (window destruction).
   * Otherwise the cached GL handles would dangle. */
  if (backend) {
    backend->invalidate();
    delete backend;
    backend = nullptr;
  }
  overlay.release();
  offscreen.release();
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

bool Scene::ensureGL()
{
  if (backend) {
    return true;
  }
  window::WindowOptions opts;
  opts.visible = !headless;
  opts.resizable = !headless;
  window = new window::Window(litestl::math::float2(float(width), float(height)), opts);
  if (!window->init()) {
    fprintf(stderr, "Scene::ensureGL: window init failed\n");
    return false;
  }
  if (headless) {
    if (!offscreen.create(width, height)) {
      return false;
    }
  }
  backend = new opengl::GLBackend(&gpu);
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
  if (!ensureGL()) {
    return;
  }
  window->makeCurrent();
  offscreen.bind();
  glEnable(GL_DEPTH_TEST);
  glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  if (!mesh || !tree) {
    return;
  }

  float aspect = float(width) / float(height);
  mat4 vp = camera.viewProj(aspect);

  opengl::DrawUniforms u;
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
  glFinish();
}

void Scene::renderWindow()
{
  if (!ensureGL()) {
    return;
  }
  window->makeCurrent();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, width, height);
  glEnable(GL_DEPTH_TEST);
  glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  if (!mesh || !tree) {
    return;
  }
  float aspect = float(width) / float(height);
  mat4 vp = camera.viewProj(aspect);
  opengl::DrawUniforms u;
  u.drawMatrix = vp;
  u.normalMatrix.identity();

  tree->update(&gpu);
  backend->draw(tree->getDrawBatch(), u);
  if (showAxes) {
    overlay.drawAxes(vp);
  }
  if (showCursor && lastStroke.valid) {
    overlay.drawBrushCursor(vp, lastStroke.origin, lastStroke.normal, lastStroke.radius);
  }
  window->swap();
}

bool Scene::screenshot(const char *path)
{
  if (!ensureGL()) {
    return false;
  }
  if (headless) {
    offscreen.bind();
  } else {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  return opengl::captureToPNG(path, width, height);
}

} // namespace sculptcore::debug_app
