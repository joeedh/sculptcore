#pragma once

#include "camera.h"

#include "brush/brush.h"
#include "gpu/manager.h"
#include "mesh/mesh.h"
#include "meshlog/meshlog_base.h"
#include "opengl/gl_backend.h"
#include "opengl/gl_context.h"
#include "opengl/gl_overlay.h"
#include "spatial/spatial.h"
#include "window/window.h"

#include "litestl/util/string.h"

namespace sculptcore::debug_app {

enum class ViewPreset { Front, Top, Side, Persp, Free };

struct LastStroke {
  bool valid = false;
  litestl::math::float3 origin{0, 0, 0};
  litestl::math::float3 normal{0, 0, 1};
  float radius = 0.0f;
};

/** Owns one full debug-app scene: mesh + spatial accelerator + brush +
 *  GPU manager + GL backend + window. All optional pieces are lazily
 *  created so a script that never asks for screenshots never opens a
 *  GL context. */
struct Scene {
  Scene(int width, int height, bool headless);
  Scene(const Scene &) = delete;
  ~Scene();

  bool ensureGL();

  mesh::Mesh *mesh = nullptr;
  spatial::SpatialTree *tree = nullptr;
  brush::Brush brush;
  meshlog::MeshLog meshLog;
  gpu::GPUManager gpu;
  Camera camera;
  ViewPreset view = ViewPreset::Persp;
  LastStroke lastStroke;
  bool showLeafBounds = false;
  bool showAxes = true;
  bool showCursor = true;

  /* Owned GL bits (created on first ensureGL()). */
  window::Window *window = nullptr;
  opengl::GLBackend *backend = nullptr;
  opengl::OffscreenTarget offscreen;
  opengl::Overlay overlay;

  int width;
  int height;
  bool headless;

  void setMesh(mesh::Mesh *m);
  void buildSpatial(int leafLimit, int depthLimit);

  /** Center camera + set view direction from preset; uses mesh AABB. */
  void applyView(ViewPreset preset);

  /** Render into the offscreen target. */
  void renderHeadless();

  /** Render into the visible window (interactive smoke). */
  void renderWindow();

  /** Helper: write current framebuffer to PNG. Caller must have rendered. */
  bool screenshot(const char *path);
};

} // namespace sculptcore::debug_app
