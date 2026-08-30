#pragma once

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

struct GLFWwindow;

namespace sculptcore::debug_app {

enum class InputKind {
  CursorPos,       // CursorPosEvent
  MouseButton,     // MouseButtonEvent
  Scroll,          // ScrollEvent
  Key,             // KeyEvent
  Char,            // CharEvent
  FramebufferSize, // FramebufferSizeEvent
};

enum class MouseButton { Left = 0, Right = 1, Middle = 2 };
enum class ButtonAction { Release = 0, Press = 1, Repeat = 2 };

/* Mask values match GLFW (1<<0=Shift, 1<<1=Ctrl, 1<<2=Alt). */
struct ModFlags {
  unsigned int bits = 0;
  bool shift() const
  {
    return bits & 0x1;
  }
  bool ctrl() const
  {
    return bits & 0x2;
  }
  bool alt() const
  {
    return bits & 0x4;
  }
  bool super() const
  {
    return bits & 0x8;
  }
};

struct InputEvent {
  InputKind kind = InputKind::CursorPos;
  union Payload {
    struct CursorPos {
      float x, y;
    } cursor;
    struct MouseBtn {
      MouseButton button;
      ButtonAction action;
      ModFlags mods;
      float x, y;
    } mb;
    struct Scroll {
      float dx, dy;
    } scroll;
    struct Key {
      int key;
      int scancode;
      ButtonAction action;
      ModFlags mods;
    } key;
    struct Char {
      unsigned int codepoint;
    } ch;
    struct Fb {
      int width, height;
    } fb;
    Payload() : cursor{0.0f, 0.0f}
    {
    }
  } u;
};

struct InputHandler {
  virtual ~InputHandler() = default;
  /** Return true to mark the event consumed (subsequent handlers skipped). */
  virtual bool handle(const InputEvent &e) = 0;
};

/* Installs GLFW callbacks on `win` and fans events out to handlers in
 * registration order, stopping at the first that returns true. */
struct InputDispatcher {
  void attach(GLFWwindow *win);
  void detach();

  void addHandler(InputHandler *h)
  {
    handlers_.append(h);
  }
  void clear()
  {
    handlers_.clear();
  }

  /* Last-known cursor position (window-coords, pixels). Cached so events
   * other than CursorPos can include it. */
  float lastX = 0.0f;
  float lastY = 0.0f;
  unsigned int lastMods = 0;

  /* Forwarded by GLFW trampolines; public so the trampolines can reach them. */
  void onCursorPos(double x, double y);
  void onMouseButton(int button, int action, int mods);
  void onScroll(double dx, double dy);
  void onKey(int key, int scancode, int action, int mods);
  void onChar(unsigned int codepoint);
  void onFramebufferSize(int w, int h);

private:
  void dispatch(const InputEvent &e);

  GLFWwindow *win_ = nullptr;
  litestl::util::Vector<InputHandler *> handlers_;
};

} // namespace sculptcore::debug_app
