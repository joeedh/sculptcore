#pragma once

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"
#include "platform/time.h"

#include "glew/GL/glew.h"
#include "GLFW/glfw3.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace litestl;
namespace sculptcore::window {

struct WindowOptions {
  bool visible = true;
  /* Request a compatibility GL context that accepts the GLSL-ES style
   * shaders used by the rest of the engine (attribute/varying/gl_FragColor)
   * once a `#version 120` preamble is prepended. */
  int gl_major = 2;
  int gl_minor = 1;
  bool resizable = true;
};

struct Window {
  using float2 = math::float2;

  Window(float2 size, WindowOptions opts = {}) : size_(size), opts_(opts) {}

  /** Create the GL context. Returns false on failure. */
  bool init();

  /** Legacy fire-and-forget loop kept for the stub app entry point. */
  void start();

  bool shouldClose() const
  {
    return handle_ ? glfwWindowShouldClose(handle_) : true;
  }
  void poll() { glfwPollEvents(); }
  void swap() { glfwSwapBuffers(handle_); }
  void makeCurrent() { glfwMakeContextCurrent(handle_); }
  void close()
  {
    if (handle_) {
      glfwSetWindowShouldClose(handle_, GLFW_TRUE);
    }
  }

  GLFWwindow *handle() { return handle_; }
  float2 size() const { return size_; }

  ~Window();

private:
  float2 size_;
  WindowOptions opts_;
  GLFWwindow *handle_ = nullptr;
  bool glew_inited_ = false;
};

} // namespace sculptcore::window
