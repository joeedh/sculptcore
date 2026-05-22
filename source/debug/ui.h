#pragma once

#include "input.h"

#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace sculptcore::debug_app {

struct Scene;

/** Dear ImGui wrapper for the interactive debug app. Owns its own
 *  descriptor pool and uses the GLFW + Vulkan first-party backends; the
 *  rendering happens inside the swapchain render pass via a postDrawHook
 *  installed on Scene.
 *
 *  Implements InputHandler so the dispatcher can route events through it
 *  first — when ImGui is capturing the mouse/keyboard the controller is
 *  short-circuited. ImGui receives the actual GLFW events through its
 *  own chained callbacks (InstallCallbacks=true), so the handler return
 *  value is a focus gate, not an event delivery channel. */
struct Ui : InputHandler {
  explicit Ui(Scene *scene) : scene_(scene) {}
  Ui(const Ui &) = delete;
  ~Ui() override;

  bool handle(const InputEvent &e) override;

  /** Initialize ImGui + backends. Must be called after Scene::ensureGPU()
   *  so the swapchain render pass exists. Installs a postDrawHook on
   *  `scene` so renderWindow() records the ImGui draw data each frame.
   *  Returns false on any failure. */
  bool init();

  /** Tear down ImGui state; safe to call multiple times. */
  void shutdown();

  /** Begin a new ImGui frame and emit the brush-control panel. Call once
   *  per loop iteration before Scene::renderWindow(). */
  void beginFrame();

  /** Returns true if the ImGui IO is currently capturing the mouse —
   *  the input dispatcher can short-circuit the interactive controller
   *  in that case so dragging a slider doesn't paint into the scene. */
  bool wantCaptureMouse() const;
  bool wantCaptureKeyboard() const;

private:
  static void recordHook(void *user, VkCommandBuffer cb);
  void drawPanel();

  Scene *scene_;
  VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
  bool initialized_ = false;
  bool frameOpen_ = false;
};

} // namespace sculptcore::debug_app
