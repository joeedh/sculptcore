#pragma once

#include "glew/GL/glew.h"

namespace sculptcore::opengl {

/** Offscreen render target (RGBA8 color + depth) for headless screenshots
 *  and tests. Created in the current GL context; bind() makes the FBO the
 *  active draw target, unbind() restores the default framebuffer. */
struct OffscreenTarget {
  int width = 0;
  int height = 0;
  GLuint fbo = 0;
  GLuint color = 0;
  GLuint depth = 0;

  OffscreenTarget() = default;
  OffscreenTarget(const OffscreenTarget &) = delete;
  ~OffscreenTarget() { release(); }

  bool create(int w, int h);
  void release();

  void bind();
  static void unbind();
};

} // namespace sculptcore::opengl
