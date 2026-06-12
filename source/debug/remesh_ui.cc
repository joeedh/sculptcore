#include "remesh_ui.h"

#include "remesh_app.h"
#include "scene.h"

#include "remesh/remesh.h"

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

// Wrapped hover tooltip for the immediately-preceding widget. BeginItemTooltip
// uses the ForTooltip hover flags (stationary + short delay + allow-when-disabled),
// so the conditionally-disabled sliders still explain themselves on hover.
void tip(const char *text)
{
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
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
  tip("Pick an OBJ discovered in the assets dir. Use Rescan if you dropped a new "
      "file in while the app was open.");
  ImGui::BeginDisabled(app_->busy());
  if (ImGui::Button("Load") && app_->selected >= 0) {
    std::string err;
    if (!app_->loadAsset(app_->assets[app_->selected], err)) {
      app_->status = "ERROR " + err;
    }
  }
  tip("Load the selected asset as the working mesh (replaces the current one).");
  ImGui::SameLine();
  if (ImGui::Button("Import...")) {
    doImport();
  }
  tip("Browse for an .obj anywhere on disk and load it as the working mesh.");
  ImGui::SameLine();
  if (ImGui::Button("Rescan")) {
    app_->rescanAssets();
  }
  tip("Re-scan the assets dir for .obj files and refresh the asset list.");
  ImGui::EndDisabled();

  // --- Meshy generation ---
  ImGui::SeparatorText("Generate (Meshy text-to-3D)");
  static char prompt[256] = "";
  ImGui::InputText("prompt", prompt, sizeof(prompt));
  tip("Text prompt describing the mesh to generate, e.g. \"an anime girl\".");
  ImGui::BeginDisabled(app_->busy());
  if (ImGui::Button("Generate")) {
    std::string err;
    if (!app_->meshyGen(prompt, err)) {
      app_->status = "ERROR " + err;
    }
  }
  tip("Generate a mesh from the prompt via the Meshy text-to-3D API, then load "
      "it. Requires an API key in keys/meshy.txt.");
  ImGui::EndDisabled();

  // --- Params ---
  ImGui::SeparatorText("Params");
  {
    static int presetSel = 0;
    const char *presetName = remesh::remeshPresetName(presetSel);
    ImGui::SetNextItemWidth(160);
    if (ImGui::BeginCombo("##preset", presetName ? presetName : "(none)")) {
      for (int i = 0; const char *nm = remesh::remeshPresetName(i); i++) {
        if (ImGui::Selectable(nm, i == presetSel)) {
          presetSel = i;
        }
      }
      ImGui::EndCombo();
    }
    tip("Tier 8 preset bundles (the CLI's --preset). Apply overwrites every knob "
        "below with the preset's values, preserving the sizing fields and seed; "
        "edit freely afterwards.");
    ImGui::SameLine();
    if (ImGui::Button("Apply preset") && presetName) {
      remesh::applyRemeshPreset(P, presetName);
      app_->status = std::string("applied preset ") + presetName;
    }
    tip("Pre-fill the knob vector from the selected preset (sliders update to "
        "show the applied values).");
  }
  ImGui::InputInt("target quads", &P.target_quad_count, 100, 1000);
  if (P.target_quad_count < 1) {
    P.target_quad_count = 1;
  }
  tip("Target output quad count: the pipeline derives the quad edge length from "
      "it (L = sqrt(area/N), density-weighted) and re-quantizes once if the "
      "extracted count misses by >20%. Ignored when an explicit edge length is "
      "set below. Loading an asset applies its quad-counts.txt entry.");
  ImGui::BeginDisabled(app_->busy() || app_->selected < 0);
  if (ImGui::Button("Save asset quad count")) {
    std::string err;
    if (!app_->saveAssetQuadCount(err)) {
      app_->status = "ERROR " + err;
    }
  }
  tip("Record the target quad count as the selected asset's default in the "
      "assets dir's quad-counts.txt — read back by Load and by the CLI when no "
      "explicit sizing is given.");
  ImGui::EndDisabled();
  ImGui::SliderFloat("target edge len (0=auto)", &P.target_edge_length, 0.0f,
                     1.0f, "%.4f", ImGuiSliderFlags_Logarithmic);
  tip("Explicit output quad edge length, in the mesh's world units; overrides "
      "the quad count above and disables the count correction. 0 = derive from "
      "target quads. This is the size auto density modulates around.");
  ImGui::Checkbox("use curvature", &P.use_curvature);
  tip("Align the cross field to principal-curvature directions so quad rows "
      "follow surface flow. Off = a smoothness-only field (boundaries/sharp "
      "edges still constrain it).");
  ImGui::Checkbox("use sharp features", &P.use_sharp_features);
  tip("Pin the field to sharp edges and open boundaries so quad edges run along "
      "creases instead of crossing them.");
  ImGui::SliderFloat("sharp angle (rad)", &P.sharp_angle, 0.0f, 3.14159f, "%.3f");
  tip("Dihedral angle (radians) above which an edge is treated as sharp. Lower = "
      "more edges count as creases. Default 0.785 = 45 degrees.");
  ImGui::BeginDisabled(!P.use_sharp_features);
  ImGui::SliderFloat("feature hysteresis (rad)", &P.feature_hysteresis, 0.0f,
                     0.785f, "%.3f");
  tip("Tier 7: weak-tag band below the sharp angle. An edge in "
      "[sharp - hysteresis, sharp] is tagged only when vertex-connected to a "
      "strong sharp edge, so a crease oscillating around the threshold stays "
      "whole. 0 = off; mainly useful together with a min chain.");
  ImGui::SliderInt("feature min chain", &P.feature_min_chain, 0, 10);
  tip("Tier 7: drop tagged sharp chains shorter than this many edges unless both "
      "ends anchor at a junction or boundary (noise spurs force spurious "
      "singularities). 0 = off; 3 is a good starting point.");
  ImGui::EndDisabled();
  ImGui::Checkbox("use density", &P.use_density);
  tip("Honor a per-vertex .remesh.v.density map for local sizing "
      "(quad size is proportional to 1/sqrt(density)). 'auto density' below "
      "turns this on implicitly and fills the map from curvature.");
  ImGui::Checkbox("direct rounding", &P.quantize_direct_rounding);
  tip("Quantize with one-shot DIRECT rounding (round every cut translation at "
      "once off the seamless solve, one re-solve) instead of greedy "
      "most-confident-first batches. Faster, but can hand the extractor an "
      "infeasible map — an A/B oracle, not a quality default.");
  {
    float dev_deg = P.untangle_field_max_dev * (180.0f / 3.14159265f);
    if (ImGui::SliderFloat("untangle max dev (deg)", &dev_deg, 0.0f, 45.0f,
                           "%.1f")) {
      P.untangle_field_max_dev = dev_deg * (3.14159265f / 180.0f);
    }
  }
  tip("Max angle the ARAP fold-untangle may leave quads rotated off the cross "
      "field. 45 = legacy unclamped (quads can sit diagonal to the field); "
      "lower realigns the map as the untangle finishes. Default 10.");
  ImGui::Checkbox("reproject", &P.reproject);
  tip("Snap the extracted quad mesh back onto the input surface so it matches "
      "the original shape. Off leaves it on the (smoother) solve surface.");
  ImGui::Checkbox("cap odd holes", &P.cap_odd_holes);
  tip("Close boundary loops with an odd edge count using one triangle each, so "
      "the rest of the mesh can stay all-quad.");
  ImGui::SliderInt("smooth iters", &P.smooth_iterations, 0, 20);
  tip("Number of post-reprojection smoothing passes that relax vertices along "
      "the surface to even out quad shapes.");
  ImGui::SliderFloat("smooth strength", &P.smooth_strength, 0.0f, 1.0f, "%.2f");
  tip("Per-iteration smoothing step (0..1). Higher relaxes faster but can pull "
      "the mesh off sharp detail.");
  {
    int seed = int(P.seed);
    if (ImGui::InputInt("seed", &seed)) {
      P.seed = uint32_t(seed < 0 ? 0 : seed);
    }
  }
  tip("Seed for the randomized stages (independent-set ordering, etc.). Fixed "
      "seed = deterministic output; change it to sample a different result.");
  ImGui::Checkbox("triage", &P.triage);
  tip("Run input cleanup before solving: weld near-duplicate verts, drop tiny "
      "components, and fix inconsistent winding. Recommended for scanned / "
      "messy meshes.");
  ImGui::BeginDisabled(!P.triage);
  ImGui::SliderFloat("triage weld rel", &P.triage_weld_rel, 0.0f, 1e-3f, "%.6f");
  tip("Weld tolerance as a fraction of the bounding-box diagonal. Verts closer "
      "than this are merged.");
  ImGui::SliderFloat("triage min comp frac", &P.triage_min_component_frac, 0.0f,
                     0.5f, "%.3f");
  tip("Drop connected components with fewer than this fraction of the total "
      "verts (removes specks / floaters). 0 = keep every component.");
  ImGui::EndDisabled();
  ImGui::SliderFloat("hole fill max frac", &P.input_hole_fill_max_frac, 0.0f,
                     0.5f, "%.3f");
  tip("Tier 6: pre-solve hole policy. Triangulate-fill input boundary loops "
      "whose rim length is under this fraction of the total boundary length, so "
      "tiny punctures don't seed spurious boundary constraints; larger "
      "boundaries stay open. 0 = fill nothing.");
  ImGui::Checkbox("per component", &P.per_component);
  tip("Tier 6: remesh disconnected components independently so one component's "
      "field/singularities can't perturb another's solve. All components share "
      "the resolved quad edge length; a component whose sub-run fails is "
      "dropped from the merged output.");
  ImGui::SliderInt("curv smooth iters", &P.curvature_smooth_iters, 0, 20);
  tip("Tier 2a: Jacobi sweeps that smooth the curvature tensor field before "
      "directions are extracted, reducing noisy field alignment. 0 = off (raw "
      "per-vertex curvature).");
  ImGui::BeginDisabled(P.curvature_smooth_iters <= 0);
  ImGui::SliderFloat("curv smooth lambda", &P.curvature_smooth_lambda, 0.0f, 1.0f,
                     "%.2f");
  tip("Per-sweep blend toward the neighbor-averaged tensor (0..1). Higher = more "
      "smoothing per iteration.");
  ImGui::EndDisabled();
  ImGui::SliderFloat("field smoothness", &P.field_smoothness, 0.1f, 8.0f, "%.2f",
                     ImGuiSliderFlags_Logarithmic);
  tip("Tier 4: per-edge smoothness weight of the cross-field solve. Higher = "
      "globally smoother field with fewer noise-born singularities, at the cost "
      "of curvature tracking. Regularization ~ smoothness / curvature weight.");
  ImGui::SliderFloat("curvature weight", &P.curvature_weight, 0.0f, 8.0f, "%.2f");
  tip("Tier 4: soft curvature-alignment scale (x local anisotropy) — the other "
      "half of the smoothness/alignment tradeoff.");
  ImGui::Checkbox("singularity cancel", &P.singularity_cancel);
  tip("Tier 5: cancel +1/-1 pole pairs closer than the gate below by flipping "
      "edge periods along the geodesic path between them, then re-solving the "
      "phase field. Targets noise-born pairs on scan/messy inputs.");
  ImGui::BeginDisabled(!P.singularity_cancel);
  ImGui::SliderFloat("cancel max sep", &P.singularity_cancel_max_sep, 0.5f, 4.0f,
                     "%.2f");
  tip("Pair-separation gate in quad-edge-length units: only pole pairs within "
      "this geodesic distance are cancelled. Larger merges farther pairs but "
      "distorts the field over a wider area.");
  ImGui::EndDisabled();
  ImGui::Checkbox("auto density", &P.auto_density);
  tip("Tier 3: derive the density map from curvature (s = curvature x target, "
      "density = clamp(s^2, min, max)) so curved regions get finer quads and "
      "flat regions coarser. Implies 'use density'.");
  ImGui::BeginDisabled(!P.auto_density);
  ImGui::SliderFloat("density min", &P.density_min, 0.05f, 1.0f, "%.2f");
  tip("Lower clamp on auto density: the coarsest sizing (largest quads) allowed "
      "on flat regions.");
  ImGui::SliderFloat("density max", &P.density_max, 1.0f, 16.0f, "%.2f");
  tip("Upper clamp on auto density: the finest sizing (smallest quads) allowed "
      "on high-curvature regions; also acts as the minimum-feature-size floor.");
  ImGui::EndDisabled();
  ImGui::SliderFloat("density gradation", &P.density_gradation, 0.0f, 2.0f, "%.2f");
  tip("Tier 3: bound how fast quad size may change between neighboring verts "
      "(Alauzet limiter), smearing sharp size jumps over several rings. 0 = off; "
      "~0.5 = gentle; larger allows sharper transitions.");
  ImGui::BeginDisabled(P.density_gradation <= 0.0f);
  ImGui::SliderInt("density grad iters", &P.density_gradation_iters, 1, 30);
  tip("Maximum Gauss-Seidel sweeps for the gradation limiter (it stops early "
      "once the size field converges).");
  ImGui::EndDisabled();
  ImGui::Checkbox("pre-remesh in pipeline", &P.pre_remesh);
  tip("Tier 9d: run the field-aligned input pre-pass inside the quad pipeline "
      "(after triage, before the field solve), cleaning the working "
      "triangulation's flow. The standalone section below is for inspecting the "
      "pre-pass alone; this gate is what 'Run Remesh' uses.");
  ImGui::BeginDisabled(!P.pre_remesh);
  ImGui::SliderFloat("pl target (0=auto)", &P.pre_remesh_target, 0.0f, 1.0f,
                     "%.4f", ImGuiSliderFlags_Logarithmic);
  tip("Pipeline pre-pass edge length. 0 = auto: 0.7x the resolved quad edge, "
      "floored at half the median input edge.");
  ImGui::SliderInt("pl iters (0=auto)", &P.pre_remesh_iters, 0, 20);
  tip("Outer convergence iterations. 0 = auto from the measured input "
      "(resolution travel + fold/irregularity level, clamped to 3..6).");
  ImGui::SliderInt("pl bootstrap (-1=auto)", &P.pre_remesh_bootstrap_iters, -1, 8);
  tip("Isotropic denoise sweeps before the field is trusted. -1 = auto from "
      "input noise (1 clean / 2 default / 4 noisy); 0 = none.");
  ImGui::Checkbox("pl density", &P.pre_remesh_density);
  tip("Grade the pipeline pre-pass band by the curvature size field. Note: "
      "regenerating it overwrites a painted density map on the working copy.");
  ImGui::SliderFloat("pl gradation (0=off)", &P.pre_remesh_gradation, 0.0f, 2.0f,
                     "%.2f");
  tip("Per-edge-hop growth cap on the pipeline pre-pass size field.");
  ImGui::BeginDisabled(P.pre_remesh_gradation <= 0.0f);
  ImGui::SliderInt("pl gradation iters", &P.pre_remesh_gradation_iters, 1, 30);
  tip("Work cap for the pipeline pre-pass gradation limiter (pops per vertex).");
  ImGui::EndDisabled();
  ImGui::SliderFloat("pl align (iso<->field)", &P.pre_remesh_align, 0.0f, 1.0f,
                     "%.2f");
  tip("Pipeline pre-pass smooth blend: isotropic (0) to field-aligned (1).");
  ImGui::SliderInt("pl field cadence", &P.pre_remesh_field_cadence, 1, 8);
  tip("Recompute the rough cross field every N outer iters.");
  ImGui::SliderInt("pl smooth iters", &P.pre_remesh_smooth_iters, 0, 20);
  tip("Inner field-aligned smooth sweeps per outer iter.");
  ImGui::SliderFloat("pl smooth lambda", &P.pre_remesh_smooth_lambda, 0.0f, 1.0f,
                     "%.2f");
  tip("Per-sweep relaxation factor for the pipeline pre-pass smooth.");
  ImGui::SliderFloat("pl converge eps (0=off)", &P.pre_remesh_converge_eps, 0.0f,
                     0.2f, "%.3f");
  tip("Early-out: stop once an outer iter moves every vertex less than "
      "eps x target. Clean inputs settle in 2-3 iters.");
  ImGui::Checkbox("pl preserve features", &P.pre_remesh_preserve_features);
  tip("Pin boundaries and dihedral-sharp creases through the pipeline pre-pass.");
  ImGui::BeginDisabled(!P.pre_remesh_preserve_features);
  ImGui::SliderFloat("pl sharp angle (rad)", &P.pre_remesh_sharp_angle, 0.0f,
                     3.14159f, "%.3f");
  tip("Crease dihedral threshold for the pipeline pre-pass feature pinning.");
  ImGui::EndDisabled();
  ImGui::Checkbox("pl convergence trace", &P.pre_remesh_trace);
  tip("Print the pre-pass per-iter convergence summary + oscillation verdict to "
      "the CLI job's stderr.");
  ImGui::Checkbox("pl reproject anchors", &P.pre_remesh_anchors);
  tip("Maintain per-vertex source-face anchors through the pre-pass and "
      "transport them into the reproject snap (local sheet-correct walks "
      "instead of global closest-point queries).");
  ImGui::EndDisabled();
  ImGui::Checkbox("auto retry", &P.auto_retry);
  tip("Tier 8: metric-driven retry. Re-run the pipeline from the original input "
      "with one knob escalated per attempt — driven by the run report's folds / "
      "pole count / size cliffs / odd-hole residue — keeping the best-scoring "
      "result.");
  ImGui::BeginDisabled(!P.auto_retry);
  ImGui::SliderInt("max attempts", &P.max_attempts, 1, 8);
  tip("Total attempt cap, first run included (clamped to the report's trail "
      "capacity of 8).");
  ImGui::EndDisabled();

  // --- Pre-remesh (Tier 9 input pre-pass) ---
  ImGui::SeparatorText("Pre-remesh (input pre-pass)");
  auto &PR = app_->preParams;
  ImGui::SliderFloat("pre align (iso<->field)", &PR.align, 0.0f, 1.0f, "%.2f");
  tip("Blend the pre-pass tangential smooth between isotropic relaxation (0) and "
      "cross-field-aligned (1). Field-aligned straightens quad rows along the "
      "rough field; isotropic just evens out triangle sizes.");
  ImGui::SliderInt("pre iters", &PR.iters, 1, 20);
  tip("Outer convergence iterations of the pre-pass (cross field -> Botsch-Kobbelt "
      "remesh -> field-aligned smooth). Also the step count for the Step button.");
  ImGui::SliderFloat("pre target (0=auto)", &PR.target, 0.0f, 1.0f, "%.4f",
                     ImGuiSliderFlags_Logarithmic);
  tip("Base pre-pass edge length. 0 = the pipeline's auto resolution (explicit "
      "edge len, else 0.7x the count-derived quad edge with a median floor). "
      "With 'pre density' on it is scaled per-vertex by 1/sqrt(density).");
  ImGui::Checkbox("pre density", &PR.density);
  tip("Grade the pre-pass split/collapse band by a per-vertex curvature size field "
      "(finer where curved) instead of one global length.");
  ImGui::BeginDisabled(!PR.density);
  ImGui::SliderFloat("pre density min", &PR.density_min, 0.05f, 1.0f, "%.2f");
  tip("Lower clamp on the pre-pass size field (coarsest sizing on flat regions).");
  ImGui::SliderFloat("pre density max", &PR.density_max, 1.0f, 16.0f, "%.2f");
  tip("Upper clamp on the pre-pass size field (finest sizing on curved regions).");
  ImGui::SliderFloat("pre gradation (0=off)", &PR.gradation, 0.0f, 2.0f, "%.2f");
  tip("Per-edge-hop growth cap on the pre-pass size field (Alauzet limiter): the "
      "goal length may grow by at most (1 + gradation) per hop, so BK doesn't "
      "thrash at sharp size cliffs. 0 = off (A/B only).");
  ImGui::BeginDisabled(PR.gradation <= 0.0f);
  ImGui::SliderInt("pre gradation iters", &PR.gradation_iters, 1, 30);
  tip("Work cap for the gradation limiter (pops per vertex; it stops early once "
      "the size field converges).");
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  ImGui::SliderInt("pre field cadence", &PR.field_cadence, 1, 8);
  tip("Recompute the rough cross field every N outer iters (it is stable once the "
      "geometry settles, so it need not be re-solved every iteration).");
  ImGui::SliderInt("pre bootstrap iters", &PR.bootstrap_iters, 0, 8);
  tip("Isotropic denoise sweeps before the field-aligned smooth begins, so a noisy "
      "input isn't over-regularized to its noise (and 45-degree noise isn't mistaken "
      "for a sharp feature). 0 = keep crisp features from the start.");
  ImGui::SliderInt("pre smooth iters", &PR.smooth_iters, 0, 20);
  tip("Inner field-aligned smooth sweeps per outer iter.");
  ImGui::SliderFloat("pre smooth lambda", &PR.smooth_lambda, 0.0f, 1.0f, "%.2f");
  tip("Per-sweep relaxation factor for the pre-pass smooth.");
  ImGui::SliderFloat("pre converge eps (0=off)", &PR.converge_eps, 0.0f, 0.2f,
                     "%.3f");
  tip("Early-out: stop once an outer iter's smooth moves every vertex less than "
      "eps x target. 0 = always run all iters.");
  {
    int s = int(PR.seed);
    if (ImGui::InputInt("pre seed", &s)) {
      PR.seed = uint32_t(s < 0 ? 0 : s);
    }
  }
  tip("Determinism seed for the pre-pass BK rounds (bumped per outer iter by "
      "the Step driver).");
  ImGui::Checkbox("pre preserve features", &PR.preserve_features);
  tip("Pin open boundaries and dihedral-sharp creases so the iterated flow follows "
      "features instead of eroding them.");
  ImGui::BeginDisabled(!PR.preserve_features);
  ImGui::SliderFloat("pre sharp angle (rad)", &PR.sharp_angle, 0.0f, 3.14159f, "%.3f");
  tip("Dihedral angle (radians) above which a pre-pass edge counts as a sharp "
      "feature to pin. Default 0.785 = 45 degrees.");
  ImGui::EndDisabled();
  ImGui::Checkbox("pre convergence trace", &app_->preTrace);
  tip("Print the per-iter convergence summary + oscillation verdict to stderr "
      "after Run pre-pass.");
  ImGui::Checkbox("show rough field after run", &app_->preShowField);
  tip("After a pre-pass run, turn on the cross-field overlay so the rough field it "
      "wrote into .remesh.f.theta is visible (field-aligned runs only).");
  ImGui::Checkbox("reproject onto input", &app_->preReproject);
  tip("Snap the pre-pass result back onto the original input surface (reloaded "
      "from disk) so tangential smoothing can't drift verts off the surface into "
      "spikes. The full quad pipeline reprojects too; toggle off to see raw drift.");

  ImGui::BeginDisabled(app_->busy() || !scene_->mesh);
  if (ImGui::Button("Run pre-pass")) {
    std::string err;
    if (!app_->runPreRemesh(err)) {
      app_->status = "ERROR " + err;
    }
  }
  tip("Run the whole input pre-pass in-process on the loaded mesh and show the "
      "cleaned triangle result. Does not run the full quad pipeline.");
  ImGui::SameLine();
  if (!app_->preStepping) {
    if (ImGui::Button("Step >")) {
      app_->preStepStart();
    }
    tip("Animate convergence: apply one outer pre-pass iter per frame so the mesh "
        "and rough field update live. Runs 'pre iters' steps.");
  } else {
    if (ImGui::Button("Stop")) {
      app_->preStepping = false;
    }
    tip("Stop the per-iteration stepping animation.");
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset")) {
    std::string err;
    if (!app_->preStepReset(err)) {
      app_->status = "ERROR " + err;
    }
  }
  tip("Reload the current asset from disk (undo the pre-pass to inspect again).");
  ImGui::SeparatorText(app_->stats.c_str());
  if (ImGui::Button("Laplacian Smooth")) {
    app_->scene().smoothMesh();
  }
  tip("Apply one plain Laplacian smooth pass to the loaded mesh (quick visual "
      "comparison against the pre-pass's tangential smooth).");
  ImGui::EndDisabled();
  if (app_->preStepping) {
    ImGui::SameLine();
    ImGui::Text("step %d/%d", app_->preStepIter, PR.iters);
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
  tip("Run the full quad-remesh pipeline on the loaded mesh with the params "
      "above. Progress and result stats appear below.");
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::Checkbox("quad wireframe", &app_->showWireframe);
  tip("Overlay the output quad edges on the rendered mesh.");

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
  tip("Frame the camera to the bounds of the current mesh.");
  ImGui::SameLine();
  ImGui::Checkbox("axes", &scene_->showAxes);
  tip("Show the world-axis gizmo at the origin.");
  ImGui::Checkbox("curvature field", &app_->showCurvature);
  tip("Draw per-vertex principal-curvature direction lines (blue = minimum "
      "curvature kmin, red = maximum kmax). The field 'use curvature' aligns to.");
  ImGui::SameLine();
  ImGui::TextDisabled("(blue=kmin, red=kmax)");
  ImGui::SliderFloat("field line scale", &app_->curvatureScale, 0.0f, 0.2f, "%.3f");
  tip("Length of the drawn curvature direction lines (display only).");

  ImGui::Checkbox("cross field", &app_->showCrossField);
  tip("Draw the solved 4-RoSy cross field and its singularities (the quad-flow "
      "directions the parametrization integrates).");
  ImGui::SameLine();
  ImGui::TextDisabled("(4-RoSy + singularities)");
  if (app_->showCrossField) {
    ImGui::SliderFloat("cross scale", &app_->crossScale, 0.0f, 0.2f, "%.3f");
    tip("Length of the drawn cross-field crosses (display only).");
    ImGui::Checkbox("anisotropy weighting", &app_->crossAnisotropy);
    tip("Scale each cross arm by the local field anisotropy when drawing, so "
        "stretched regions read as stretched.");
  }

  ImGui::Checkbox("field edges", &app_->showFieldEdges);
  tip("Color mesh edges by a per-edge cross-field diagnostic (see edge mode).");
  if (app_->showFieldEdges) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("edge mode", &app_->fieldEdgeMode, "period\0curl\0");
    tip("period = integer period (matching) jump across the edge; "
        "curl = field curl residual across the edge.");
  }

  ImGui::Checkbox("streamlines", &app_->showStreamlines);
  tip("Trace integral curves of the cross field across the surface to preview "
      "the quad flow.");
  if (app_->showStreamlines) {
    ImGui::SliderFloat("stream step", &app_->streamlineScale, 0.001f, 0.05f, "%.3f");
    tip("Integration step length for streamline tracing. Smaller = smoother "
        "but slower curves.");
    ImGui::SliderInt("stream seeds", &app_->streamlineSeeds, 10, 2000);
    tip("Number of seed points streamlines are traced from.");
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
