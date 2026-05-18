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

  glfwWindowHint(GLFW_VISIBLE, opts_.visible ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_RESIZABLE, opts_.resizable ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, opts_.gl_major);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, opts_.gl_minor);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);

  handle_ =
      glfwCreateWindow(int(size_[0]), int(size_[1]), "SculptCore", nullptr, nullptr);
  if (!handle_) {
    fprintf(stderr, "Failed to create GLFW window.\n");
    fflush(stderr);
    return false;
  }

  glfwMakeContextCurrent(handle_);

  glewExperimental = GL_TRUE;
  GLenum errorcode = glewInit();
  if (errorcode != GLEW_OK) {
    fprintf(stderr,
            "Failed to initialize glew; error code: %s\n",
            glewGetErrorString(errorcode));
    return false;
  }
  /* glewInit() can leave a benign GL_INVALID_ENUM in the error queue with
   * core profiles; drain it so subsequent checks aren't poisoned. */
  while (glGetError() != GL_NO_ERROR) {
  }
  glew_inited_ = true;
  return true;
}

void Window::start()
{
  if (!init()) {
    return;
  }
  while (!glfwWindowShouldClose(handle_)) {
    glClearColor(0.1f, 0.3f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glfwSwapBuffers(handle_);
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
