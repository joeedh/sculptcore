#pragma once

#include "litestl/math/vector.h"
#include "platform/time.h"
#include "litestl/util/vector.h"

#include "glew/GL/glew.h"
#include "GLFW/glfw3.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace litestl;
namespace sculptcore::window {
struct Window {
  using float2 = math::float2;

  Window(float2 size) : size_(size)
  {
  }

  void start();
private:
  float2 size_;
  GLFWwindow *handle_ = nullptr;
};
} // namespace sculptcore::window
