#pragma once

// ImGui panel for the quad-remesh debug app. Mirrors debug_app's Ui (own
// descriptor pool + GLFW/Vulkan backends, draws inside the swapchain pass via a
// Scene postDrawHook) but renders remesh controls — asset dropdown, import,
// meshy generate, the RemeshParams knobs, Run, and live progress — driving
// everything through RemeshApp so a button and a pipe command do the same thing.

#include "input.h"

#include <vulkan/vulkan.h>

namespace sculptcore::debug_app {

struct Scene;
class RemeshApp;

struct RemeshUi : InputHandler {
  RemeshUi(Scene *scene, RemeshApp *app) : scene_(scene), app_(app)
  {
  }
  RemeshUi(const RemeshUi &) = delete;
  ~RemeshUi() override;

  bool handle(const InputEvent &e) override;

  /* Init ImGui + backends. Call after Scene::ensureGPU(). Installs the Scene
   * postDrawHook so renderWindow() records the panel each frame. */
  bool init();
  void shutdown();

  /* New ImGui frame + emit the panel. Call once per loop before renderWindow(). */
  void beginFrame();

  bool wantCaptureMouse() const;
  bool wantCaptureKeyboard() const;

private:
  static void recordHook(void *user, VkCommandBuffer cb);
  void drawPanel();
  void doImport();

  Scene *scene_;
  RemeshApp *app_;
  VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
  bool initialized_ = false;
  bool frameOpen_ = false;
};

} // namespace sculptcore::debug_app
