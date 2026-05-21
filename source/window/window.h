#pragma once

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"
#include "platform/time.h"

#include "GLFW/glfw3.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace litestl;
namespace sculptcore::window {

struct WindowOptions {
  bool visible = true;
  bool resizable = true;
};

/** Bare GLFW window with no client API attached — Vulkan owns the surface
 *  and swapchain via `vulkan::VkContext`. The renderer is responsible for
 *  presenting frames; this struct just hands out the GLFWwindow handle. */
struct Window {
  using float2 = math::float2;

  Window(float2 size, WindowOptions opts = {}) : size_(size), opts_(opts) {}

  /** Create the window. Returns false on failure. */
  bool init();

  /** Legacy fire-and-forget loop kept for the stub app entry point. */
  void start();

  bool shouldClose() const
  {
    return handle_ ? glfwWindowShouldClose(handle_) : true;
  }
  void poll() { glfwPollEvents(); }
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
};

} // namespace sculptcore::window
