#include "window.h"

namespace sculptcore::window {
void Window::start()
{
  if (!glfwInit()) {
    fprintf(stderr, "Failed to initialize glfw.\n");
    fflush(stderr);
    abort();
  }

  handle_ =
      glfwCreateWindow(int(size_[0]), int(size_[1]), "SculptCore", nullptr, nullptr);
  if (!handle_) {
    fprintf(stderr, "Failed to create window.\n");
    fflush(stderr);
    abort();
  }

  glfwMakeContextCurrent(handle_);

  while (!glfwWindowShouldClose(handle_)) {
    /* Render here */
    glClearColor(0.1, 0.3, 1.0, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);

    /* Swap front and back buffers */
    glfwSwapBuffers(handle_);

    /* Poll for and process events */
    glfwPollEvents();

    time::sleep_ms(1);
  }
}
}
