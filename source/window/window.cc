#include "window.h"

namespace sculptcore::window {

static bool glfw_inited = false;

static void ensure_glfw()
{
  if (glfw_inited) {
    return;
  }
  if (!glfwInit()) {
    fprintf(stderr, "Failed to initialize glfw.\n");
    fflush(stderr);
    abort();
  }
  glfw_inited = true;
}

bool Window::init()
{
  ensure_glfw();

  /* Vulkan-backed: ask GLFW not to create a GL context. */
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_VISIBLE, opts_.visible ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_RESIZABLE, opts_.resizable ? GLFW_TRUE : GLFW_FALSE);

  handle_ =
      glfwCreateWindow(int(size_[0]), int(size_[1]), "SculptCore", nullptr, nullptr);
  if (!handle_) {
    fprintf(stderr, "Failed to create GLFW window.\n");
    fflush(stderr);
    return false;
  }
  return true;
}

void Window::start()
{
  if (!init()) {
    return;
  }
  while (!glfwWindowShouldClose(handle_)) {
    glfwPollEvents();
    time::sleep_ms(1);
  }
}

Window::~Window()
{
  if (handle_) {
    glfwDestroyWindow(handle_);
    handle_ = nullptr;
  }
}

} // namespace sculptcore::window
