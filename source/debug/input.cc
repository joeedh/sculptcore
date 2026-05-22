#include "input.h"

#include "GLFW/glfw3.h"

namespace sculptcore::debug_app {

namespace {

unsigned int convertMods(int glfwMods)
{
  unsigned int out = 0;
  if (glfwMods & GLFW_MOD_SHIFT)    out |= 0x1;
  if (glfwMods & GLFW_MOD_CONTROL)  out |= 0x2;
  if (glfwMods & GLFW_MOD_ALT)      out |= 0x4;
  if (glfwMods & GLFW_MOD_SUPER)    out |= 0x8;
  return out;
}

InputDispatcher *get(GLFWwindow *w)
{
  return static_cast<InputDispatcher *>(glfwGetWindowUserPointer(w));
}

void cb_cursorPos(GLFWwindow *w, double x, double y)
{
  if (auto *d = get(w)) d->onCursorPos(x, y);
}
void cb_mouseButton(GLFWwindow *w, int b, int a, int m)
{
  if (auto *d = get(w)) d->onMouseButton(b, a, m);
}
void cb_scroll(GLFWwindow *w, double dx, double dy)
{
  if (auto *d = get(w)) d->onScroll(dx, dy);
}
void cb_key(GLFWwindow *w, int k, int s, int a, int m)
{
  if (auto *d = get(w)) d->onKey(k, s, a, m);
}
void cb_char(GLFWwindow *w, unsigned int c)
{
  if (auto *d = get(w)) d->onChar(c);
}
void cb_fbSize(GLFWwindow *w, int width, int height)
{
  if (auto *d = get(w)) d->onFramebufferSize(width, height);
}

} // namespace

void InputDispatcher::attach(GLFWwindow *win)
{
  win_ = win;
  glfwSetWindowUserPointer(win, this);
  glfwSetCursorPosCallback(win, cb_cursorPos);
  glfwSetMouseButtonCallback(win, cb_mouseButton);
  glfwSetScrollCallback(win, cb_scroll);
  glfwSetKeyCallback(win, cb_key);
  glfwSetCharCallback(win, cb_char);
  glfwSetFramebufferSizeCallback(win, cb_fbSize);
}

void InputDispatcher::detach()
{
  if (!win_) return;
  glfwSetCursorPosCallback(win_, nullptr);
  glfwSetMouseButtonCallback(win_, nullptr);
  glfwSetScrollCallback(win_, nullptr);
  glfwSetKeyCallback(win_, nullptr);
  glfwSetCharCallback(win_, nullptr);
  glfwSetFramebufferSizeCallback(win_, nullptr);
  glfwSetWindowUserPointer(win_, nullptr);
  win_ = nullptr;
}

void InputDispatcher::dispatch(const InputEvent &e)
{
  for (InputHandler *h : handlers_) {
    if (h->handle(e)) {
      break;
    }
  }
}

void InputDispatcher::onCursorPos(double x, double y)
{
  lastX = float(x);
  lastY = float(y);
  InputEvent e;
  e.kind = InputKind::CursorPos;
  e.u.cursor.x = lastX;
  e.u.cursor.y = lastY;
  dispatch(e);
}

void InputDispatcher::onMouseButton(int button, int action, int mods)
{
  lastMods = convertMods(mods);
  InputEvent e;
  e.kind = InputKind::MouseButton;
  switch (button) {
  case GLFW_MOUSE_BUTTON_LEFT:   e.u.mb.button = MouseButton::Left;   break;
  case GLFW_MOUSE_BUTTON_RIGHT:  e.u.mb.button = MouseButton::Right;  break;
  case GLFW_MOUSE_BUTTON_MIDDLE: e.u.mb.button = MouseButton::Middle; break;
  default: return;
  }
  e.u.mb.action = ButtonAction(action);
  e.u.mb.mods.bits = lastMods;
  e.u.mb.x = lastX;
  e.u.mb.y = lastY;
  dispatch(e);
}

void InputDispatcher::onScroll(double dx, double dy)
{
  InputEvent e;
  e.kind = InputKind::Scroll;
  e.u.scroll.dx = float(dx);
  e.u.scroll.dy = float(dy);
  dispatch(e);
}

void InputDispatcher::onKey(int key, int scancode, int action, int mods)
{
  lastMods = convertMods(mods);
  InputEvent e;
  e.kind = InputKind::Key;
  e.u.key.key = key;
  e.u.key.scancode = scancode;
  e.u.key.action = ButtonAction(action);
  e.u.key.mods.bits = lastMods;
  dispatch(e);
}

void InputDispatcher::onChar(unsigned int codepoint)
{
  InputEvent e;
  e.kind = InputKind::Char;
  e.u.ch.codepoint = codepoint;
  dispatch(e);
}

void InputDispatcher::onFramebufferSize(int w, int h)
{
  InputEvent e;
  e.kind = InputKind::FramebufferSize;
  e.u.fb.width = w;
  e.u.fb.height = h;
  dispatch(e);
}

} // namespace sculptcore::debug_app
