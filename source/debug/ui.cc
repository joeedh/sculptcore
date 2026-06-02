#include "ui.h"

#include "scene.h"

#include "brush/brush_executor.h"
#include "mesh/utils/triangulate.h"
#include "vulkan/vk_context.h"
#include "vulkan/vk_swapchain.h"
#include "window/window.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include <cstdio>

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

} // namespace

Ui::~Ui()
{
  shutdown();
}

bool Ui::init()
{
  if (initialized_) {
    return true;
  }
  if (!scene_ || !scene_->context || !scene_->window ||
      scene_->swapchain.renderPass == VK_NULL_HANDLE) {
    std::fprintf(stderr, "Ui::init: scene/GPU not ready\n");
    return false;
  }
  auto *ctx = scene_->context;

  descriptorPool_ = createImGuiDescriptorPool(ctx->device);
  if (descriptorPool_ == VK_NULL_HANDLE) {
    std::fprintf(stderr, "Ui::init: descriptor pool failed\n");
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
    std::fprintf(stderr, "Ui::init: ImGui_ImplVulkan_Init failed\n");
    shutdown();
    return false;
  }

  scene_->setPostDrawHook(&Ui::recordHook, this);
  initialized_ = true;
  return true;
}

void Ui::shutdown()
{
  if (scene_) {
    scene_->setPostDrawHook(nullptr, nullptr);
  }
  if (initialized_) {
    /* Drain the device so any in-flight ImGui resources are safe to free. */
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

void Ui::beginFrame()
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

void Ui::drawPanel()
{
  if (!scene_) {
    return;
  }
  auto &brush = scene_->brush;
  ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(280, 280), ImGuiCond_FirstUseEver);
  ImGui::Begin("Brush");

  bool changed = false;
  changed |= ImGui::SliderFloat("radius", &brush.radius, 0.01f, 2.0f, "%.3f");
  changed |= ImGui::SliderFloat("strength", &brush.strength, 0.0f, 2.0f, "%.3f");
  changed |= ImGui::SliderFloat("spacing", &brush.spacing, 0.05f, 1.0f, "%.3f");
  changed |= ImGui::Checkbox("invert", &brush.invert);
  if (changed) {
    brush.writeProps();
  }

  ImGui::Separator();

  /* Brush type — combo index maps 1:1 to SculptBrushes enum order. */
  static const brush::SculptBrushes kTools[] = {
      brush::SculptBrushes::DRAW,      brush::SculptBrushes::INFLATE,
      brush::SculptBrushes::CLAY,      brush::SculptBrushes::PINCH,
      brush::SculptBrushes::SHARP,     brush::SculptBrushes::MASK,
      brush::SculptBrushes::SMOOTH,    brush::SculptBrushes::KELVINLET,
      brush::SculptBrushes::POSE,      brush::SculptBrushes::TEXDRAW,
  };
  static const char *kToolNames[] = {"Draw",  "Inflate",   "Clay", "Pinch",
                                     "Sharp", "Mask",      "Smooth",
                                     "Kelvinlet", "Pose",  "TexDraw"};
  int toolIdx = 0;
  for (int i = 0; i < int(sizeof(kTools) / sizeof(kTools[0])); i++) {
    if (kTools[i] == scene_->currentTool) {
      toolIdx = i;
      break;
    }
  }
  if (ImGui::Combo("tool", &toolIdx, kToolNames,
                   int(sizeof(kToolNames) / sizeof(kToolNames[0])))) {
    scene_->currentTool = kTools[toolIdx];
  }

  /* Backend — WGSL drives the real Vulkan compute path, only available when
   * the GPU-dispatch path was compiled in (native + spirv backend). WebGPU
   * (wgpu-native) is added when SBRUSH_WEBGPU_COMPUTE was on at configure. */
#ifdef SBRUSH_WEBGPU_COMPUTE
  static const char *kBackendNames[] = {"C++", "WGSL", "WebGPU"};
  const int kBackendCount = 3;
  const BrushBackend kBackends[] = {BrushBackend::Cpp, BrushBackend::Wgsl,
                                    BrushBackend::WgpuNative};
#else
  static const char *kBackendNames[] = {"C++", "WGSL"};
  const int kBackendCount = 2;
  const BrushBackend kBackends[] = {BrushBackend::Cpp, BrushBackend::Wgsl};
#endif
  int backendIdx = 0;
  for (int i = 0; i < kBackendCount; i++) {
    if (kBackends[i] == scene_->currentBackend) {
      backendIdx = i;
      break;
    }
  }
#ifndef SBRUSH_GPU_DISPATCH
  ImGui::BeginDisabled(true);
#endif
  if (ImGui::Combo("backend", &backendIdx, kBackendNames, kBackendCount)) {
    scene_->currentBackend = kBackends[backendIdx];
  }
#ifndef SBRUSH_GPU_DISPATCH
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("GPU dispatch not compiled (needs the spirv backend)");
  }
#endif

#ifdef SBRUSH_GPU_DISPATCH
  /* How the GPU-resident WGSL stroke finalizes normals on release. CPU
   * (default) recomputes them with the exact per-node algorithm (byte-identical
   * to the C++ backend); GPU reads back the compute pass's global-sum normals
   * (cheaper, not bit-identical). Mid-stroke shading always uses GPU normals. */
  static const char *kEndNormalNames[] = {"CPU (exact)", "GPU (fast)"};
  int endNormalIdx = scene_->strokeEndNormals == StrokeEndNormals::Gpu ? 1 : 0;
  if (ImGui::Combo("stroke-end normals", &endNormalIdx, kEndNormalNames, 2)) {
    scene_->strokeEndNormals =
        endNormalIdx == 1 ? StrokeEndNormals::Gpu : StrokeEndNormals::Cpu;
  }
#endif

  ImGui::Separator();
  /* Dynamic topology: remesh under each dab toward a goal edge length. The
   * operators are triangle-only, so offer a one-click triangulate (make_cube
   * builds quads). */
  ImGui::Checkbox("dyntopo", &scene_->dyntopoEnabled);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Remesh under the brush toward the goal edge length.\n"
                      "Mesh must be triangles — use 'triangulate' first.");
  }
  {
    float detail = scene_->dyntopoParams.l_max;
    if (ImGui::SliderFloat("goal edge len", &detail, 0.005f, 0.5f, "%.4f",
                           ImGuiSliderFlags_Logarithmic)) {
      scene_->dyntopoParams.l_max = detail;
      scene_->dyntopoParams.l_min = detail * 0.4f; /* collapse below 0.4x */
    }
    ImGui::SliderFloat("grade (rim relax)", &scene_->dyntopoParams.grade, 0.0f,
                       6.0f, "%.1f");
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Relax the goal edge length outward from the brush "
                        "center (sizing field).\n0 = uniform; higher = finer "
                        "center, coarser rim, far fewer triangles.");
    }
    static const char *kModes[] = {"Subdivide", "Collapse", "Both"};
    int modeIdx = int(scene_->dyntopoParams.mode);
    if (ImGui::Combo("dyntopo mode", &modeIdx, kModes, 3)) {
      scene_->dyntopoParams.mode = dyntopo::DynTopoMode(modeIdx);
    }
    if (ImGui::Button("triangulate") && scene_->mesh && scene_->tree) {
      scene_->mesh->thawTopo();
      mesh::triangulateMesh(*scene_->mesh);
      scene_->tree->rebuild();
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Triangulate the whole mesh in place + rebuild the tree.");
    }
  }

  ImGui::Separator();
  ImGui::Checkbox("show axes", &scene_->showAxes);
  ImGui::Checkbox("show cursor", &scene_->showCursor);
  ImGui::Checkbox("show leaf bounds", &scene_->showLeafBounds);

  ImGui::Separator();
  if (ImGui::Button("undo") && scene_->mesh && scene_->tree) {
    scene_->meshLog.undo(scene_->mesh, scene_->tree);
  }
  ImGui::SameLine();
  if (ImGui::Button("redo") && scene_->mesh && scene_->tree) {
    scene_->meshLog.redo(scene_->mesh, scene_->tree);
  }

  /* Cap on retained undo steps (-1 = unbounded). Lets us measure how much of
   * the observed memory growth comes from unbounded undo history. */
  int maxUndo = scene_->meshLog.maxUndoSteps();
  if (ImGui::InputInt("max undo", &maxUndo)) {
    if (maxUndo < -1) {
      maxUndo = -1;
    }
    scene_->meshLog.setMaxUndoSteps(maxUndo);
  }

  ImGui::Separator();
  if (ImGui::Button("reorder for locality") && scene_->tree) {
    scene_->reorderForLocality();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Permute mesh elements local to their spatial nodes "
                      "(undoable). Rebuilds the tree.");
  }

  ImGui::Separator();
  int vcount = scene_->mesh ? scene_->mesh->v.count : 0;
  int fcount = scene_->mesh ? scene_->mesh->f.count : 0;
  ImGui::Text("verts: %d  faces: %d", vcount, fcount);
  ImGui::Text("%.1f fps", double(ImGui::GetIO().Framerate));

  ImGui::End();
}

void Ui::recordHook(void *user, VkCommandBuffer cb)
{
  Ui *self = static_cast<Ui *>(user);
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

bool Ui::handle(const InputEvent &e)
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

bool Ui::wantCaptureMouse() const
{
  if (!initialized_) {
    return false;
  }
  return ImGui::GetIO().WantCaptureMouse;
}

bool Ui::wantCaptureKeyboard() const
{
  if (!initialized_) {
    return false;
  }
  return ImGui::GetIO().WantCaptureKeyboard;
}

} // namespace sculptcore::debug_app
