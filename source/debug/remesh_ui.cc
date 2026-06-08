#include "remesh_ui.h"

#include "remesh_app.h"
#include "scene.h"

#include "vulkan/vk_context.h"
#include "vulkan/vk_swapchain.h"
#include "window/window.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h> // GetOpenFileNameW / OPENFILENAMEW (excluded by LEAN_AND_MEAN)

#include <cstdio>
#include <string>
#include <vector>

namespace sculptcore::debug_app {

namespace {

VkDescriptorPool createImGuiDescriptorPool(VkDevice device)
{
  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_SAMPLER, 64},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64},
  };
  VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  ci.maxSets = 64;
  ci.poolSizeCount = uint32_t(sizeof(sizes) / sizeof(sizes[0]));
  ci.pPoolSizes = sizes;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  if (vkCreateDescriptorPool(device, &ci, nullptr, &pool) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return pool;
}

std::string narrow(const std::wstring &w)
{
  if (w.empty()) {
    return std::string();
  }
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                              nullptr, nullptr);
  std::string s(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr,
                      nullptr);
  return s;
}

} // namespace

RemeshUi::~RemeshUi()
{
  shutdown();
}

bool RemeshUi::init()
{
  if (initialized_) {
    return true;
  }
  if (!scene_ || !scene_->context || !scene_->window ||
      scene_->swapchain.renderPass == VK_NULL_HANDLE) {
    std::fprintf(stderr, "RemeshUi::init: scene/GPU not ready\n");
    return false;
  }
  auto *ctx = scene_->context;

  descriptorPool_ = createImGuiDescriptorPool(ctx->device);
  if (descriptorPool_ == VK_NULL_HANDLE) {
    std::fprintf(stderr, "RemeshUi::init: descriptor pool failed\n");
    return false;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForVulkan(scene_->window->handle(), /*install_callbacks=*/true);

  ImGui_ImplVulkan_InitInfo info{};
  info.Instance = ctx->instance;
  info.PhysicalDevice = ctx->physicalDevice;
  info.Device = ctx->device;
  info.QueueFamily = ctx->graphicsQueueFamily;
  info.Queue = ctx->graphicsQueue;
  info.DescriptorPool = descriptorPool_;
  info.PipelineInfoMain.RenderPass = scene_->swapchain.renderPass;
  info.PipelineInfoMain.Subpass = 0;
  info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  info.MinImageCount = 2;
  info.ImageCount = uint32_t(scene_->swapchain.images.size());
  if (info.ImageCount < 2) {
    info.ImageCount = 2;
  }
  if (!ImGui_ImplVulkan_Init(&info)) {
    std::fprintf(stderr, "RemeshUi::init: ImGui_ImplVulkan_Init failed\n");
    shutdown();
    return false;
  }

  scene_->setPostDrawHook(&RemeshUi::recordHook, this);
  initialized_ = true;
  return true;
}

void RemeshUi::shutdown()
{
  if (scene_) {
    scene_->setPostDrawHook(nullptr, nullptr);
  }
  if (initialized_) {
    if (scene_ && scene_->context && scene_->context->device != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(scene_->context->device);
    }
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
  }
  if (descriptorPool_ != VK_NULL_HANDLE && scene_ && scene_->context &&
      scene_->context->device != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(scene_->context->device, descriptorPool_, nullptr);
    descriptorPool_ = VK_NULL_HANDLE;
  }
  frameOpen_ = false;
}

void RemeshUi::beginFrame()
{
  if (!initialized_) {
    return;
  }
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
  frameOpen_ = true;
  drawPanel();
}

void RemeshUi::doImport()
{
  wchar_t file[MAX_PATH] = {0};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = nullptr; // modal to the desktop; fine for a debug tool
  ofn.lpstrFilter = L"Meshes (*.obj)\0*.obj\0All files\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (GetOpenFileNameW(&ofn)) {
    std::string err;
    if (!app_->importAsset(narrow(file), err)) {
      app_->status = "ERROR " + err;
    }
  }
}

void RemeshUi::drawPanel()
{
  auto &P = app_->params;
  ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(320, 560), ImGuiCond_FirstUseEver);
  ImGui::Begin("Quad Remesher");

  // --- Asset selection ---
  ImGui::SeparatorText("Asset");
  const char *preview =
      (app_->selected >= 0 && app_->selected < (int)app_->assets.size())
          ? app_->assets[app_->selected].c_str()
          : "(none)";
  if (ImGui::BeginCombo("asset", preview)) {
    for (int i = 0; i < (int)app_->assets.size(); i++) {
      bool sel = i == app_->selected;
      if (ImGui::Selectable(app_->assets[i].c_str(), sel)) {
        app_->selected = i;
      }
      if (sel) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::BeginDisabled(app_->busy());
  if (ImGui::Button("Load") && app_->selected >= 0) {
    std::string err;
    if (!app_->loadAsset(app_->assets[app_->selected], err)) {
      app_->status = "ERROR " + err;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Import...")) {
    doImport();
  }
  ImGui::SameLine();
  if (ImGui::Button("Rescan")) {
    app_->rescanAssets();
  }
  ImGui::EndDisabled();

  // --- Meshy generation ---
  ImGui::SeparatorText("Generate (Meshy text-to-3D)");
  static char prompt[256] = "";
  ImGui::InputText("prompt", prompt, sizeof(prompt));
  ImGui::BeginDisabled(app_->busy());
  if (ImGui::Button("Generate")) {
    std::string err;
    if (!app_->meshyGen(prompt, err)) {
      app_->status = "ERROR " + err;
    }
  }
  ImGui::EndDisabled();

  // --- Params ---
  ImGui::SeparatorText("Params");
  ImGui::SliderFloat("target edge len", &P.target_edge_length, 0.005f, 1.0f,
                     "%.4f", ImGuiSliderFlags_Logarithmic);
  ImGui::SliderFloat("solve edge len (0=off)", &P.solve_edge_length, 0.0f, 1.0f,
                     "%.4f");
  ImGui::Checkbox("use curvature", &P.use_curvature);
  ImGui::Checkbox("use sharp features", &P.use_sharp_features);
  ImGui::SliderFloat("sharp angle (rad)", &P.sharp_angle, 0.0f, 3.14159f, "%.3f");
  ImGui::Checkbox("use density", &P.use_density);
  ImGui::Checkbox("reproject", &P.reproject);
  ImGui::Checkbox("cap odd holes", &P.cap_odd_holes);
  ImGui::SliderInt("smooth iters", &P.smooth_iterations, 0, 20);
  ImGui::SliderFloat("smooth strength", &P.smooth_strength, 0.0f, 1.0f, "%.2f");
  {
    int seed = int(P.seed);
    if (ImGui::InputInt("seed", &seed)) {
      P.seed = uint32_t(seed < 0 ? 0 : seed);
    }
  }

  // --- Run ---
  ImGui::SeparatorText("Run");
  ImGui::BeginDisabled(app_->busy() || app_->selected < 0);
  if (ImGui::Button("Run Remesh")) {
    std::string err;
    if (!app_->runRemesh(err)) {
      app_->status = "ERROR " + err;
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::Checkbox("quad wireframe", &app_->showWireframe);

  if (app_->busy() || app_->progress > 0.0f) {
    ImGui::ProgressBar(app_->progress, ImVec2(-1, 0),
                       app_->stage.empty() ? nullptr : app_->stage.c_str());
  }
  ImGui::TextWrapped("%s", app_->status.c_str());

  if (!app_->lastStats.empty()) {
    ImGui::SeparatorText("Last result");
    ImGui::TextWrapped("%s", app_->lastStats.c_str());
    if (!app_->lastManifest.empty()) {
      ImGui::TextWrapped("manifest: %s", app_->lastManifest.c_str());
    }
  }

  // --- View ---
  ImGui::SeparatorText("View");
  if (ImGui::Button("Fit camera")) {
    app_->cameraFit();
  }
  ImGui::SameLine();
  ImGui::Checkbox("axes", &scene_->showAxes);
  ImGui::Checkbox("curvature field", &app_->showCurvature);
  ImGui::SameLine();
  ImGui::TextDisabled("(blue=kmin, red=kmax)");
  ImGui::SliderFloat("field line scale", &app_->curvatureScale, 0.0f, 0.2f, "%.3f");

  ImGui::Checkbox("cross field", &app_->showCrossField);
  ImGui::SameLine();
  ImGui::TextDisabled("(4-RoSy + singularities)");
  if (app_->showCrossField) {
    ImGui::SliderFloat("cross scale", &app_->crossScale, 0.0f, 0.2f, "%.3f");
    ImGui::Checkbox("anisotropy weighting", &app_->crossAnisotropy);
  }

  ImGui::Checkbox("field edges", &app_->showFieldEdges);
  if (app_->showFieldEdges) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("edge mode", &app_->fieldEdgeMode, "period\0curl\0");
  }

  ImGui::Checkbox("streamlines", &app_->showStreamlines);
  if (app_->showStreamlines) {
    ImGui::SliderFloat("stream step", &app_->streamlineScale, 0.001f, 0.05f, "%.3f");
    ImGui::SliderInt("stream seeds", &app_->streamlineSeeds, 10, 2000);
  }

  int vc = scene_->mesh ? scene_->mesh->v.count : 0;
  int fc = scene_->mesh ? scene_->mesh->f.count : 0;
  ImGui::Text("verts: %d  faces: %d", vc, fc);
  ImGui::Text("%.1f fps", double(ImGui::GetIO().Framerate));

  ImGui::End();
}

void RemeshUi::recordHook(void *user, VkCommandBuffer cb)
{
  RemeshUi *self = static_cast<RemeshUi *>(user);
  if (!self || !self->initialized_ || !self->frameOpen_) {
    return;
  }
  ImGui::Render();
  ImDrawData *data = ImGui::GetDrawData();
  if (data) {
    ImGui_ImplVulkan_RenderDrawData(data, cb);
  }
  self->frameOpen_ = false;
}

bool RemeshUi::handle(const InputEvent &e)
{
  if (!initialized_) {
    return false;
  }
  switch (e.kind) {
  case InputKind::CursorPos:
  case InputKind::MouseButton:
  case InputKind::Scroll:
    return wantCaptureMouse();
  case InputKind::Key:
  case InputKind::Char:
    return wantCaptureKeyboard();
  case InputKind::FramebufferSize:
    return false;
  }
  return false;
}

bool RemeshUi::wantCaptureMouse() const
{
  return initialized_ && ImGui::GetIO().WantCaptureMouse;
}

bool RemeshUi::wantCaptureKeyboard() const
{
  return initialized_ && ImGui::GetIO().WantCaptureKeyboard;
}

} // namespace sculptcore::debug_app
