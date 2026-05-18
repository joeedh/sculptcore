#pragma once

#include "glew/GL/glew.h"

#include "litestl/math/matrix.h"
#include "litestl/math/vector.h"

namespace sculptcore::opengl {

/** Self-contained immediate-style overlay helpers (axes, brush cursor).
 *  Owns its own shader program + small dynamic VBO; bypasses GPUManager so
 *  it can be drawn outside the engine's normal batch lifecycle. */
struct Overlay {
  Overlay() = default;
  Overlay(const Overlay &) = delete;
  ~Overlay() { release(); }

  bool ensure();
  void release();

  /** Draw the X/Y/Z axis tri-color gizmo at the world origin. */
  void drawAxes(const litestl::math::mat4 &drawMatrix, float scale = 1.0f);

  /** Draw a brush cursor (circle) at `center` lying in the plane defined
   *  by `normal`, with the given world-space `radius`. */
  void drawBrushCursor(const litestl::math::mat4 &drawMatrix,
                       litestl::math::float3 center,
                       litestl::math::float3 normal,
                       float radius,
                       litestl::math::float4 color = {1.0f, 1.0f, 0.0f, 1.0f});

private:
  GLuint prog_ = 0;
  GLuint vbo_ = 0;
  GLint loc_pos_ = -1;
  GLint loc_color_ = -1;
  GLint loc_uMat_ = -1;
  GLint loc_uColor_ = -1;
};

} // namespace sculptcore::opengl
