#include "remesh_app.h"

#include "scene.h"

#include "mesh/mesh.h"
#include "mesh/utils/obj_io.h"

#include "litestl/util/alloc.h"

#include "remesh_debug_config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX // keep windows.h min/max macros from shadowing std::min/std::max
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <sstream>

namespace sculptcore::debug_app {

namespace fs = std::filesystem;
using litestl::math::float3;

namespace {

std::wstring widen(const std::string &s)
{
  if (s.empty()) {
    return std::wstring();
  }
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::wstring wf(float v)
{
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  return widen(buf);
}

std::wstring wb(bool b) { return b ? L"1" : L"0"; }

// First whitespace-delimited token + the (trimmed) remainder.
void splitFirst(const std::string &line, std::string &cmd, std::string &rest)
{
  size_t i = 0;
  while (i < line.size() && std::isspace((unsigned char)line[i])) {
    i++;
  }
  size_t j = i;
  while (j < line.size() && !std::isspace((unsigned char)line[j])) {
    j++;
  }
  cmd = line.substr(i, j - i);
  while (j < line.size() && std::isspace((unsigned char)line[j])) {
    j++;
  }
  rest = line.substr(j);
  while (!rest.empty() && (rest.back() == '\r' || rest.back() == '\n' ||
                           rest.back() == ' ')) {
    rest.pop_back();
  }
}

} // namespace

std::wstring RemeshApp::remeshCliPath()
{
  // Prefer the cli's own build output (picks up a `remesh_cli`-only rebuild
  // while the app runs); fall back to the copy staged next to the app exe
  // (a deployed/moved app, where the build path no longer exists).
  std::error_code ec;
  if (fs::exists(REMESH_DBG_CLI_PATH, ec)) {
    return widen(REMESH_DBG_CLI_PATH);
  }
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring exe(buf, n);
  size_t slash = exe.find_last_of(L"\\/");
  std::wstring dir = slash == std::wstring::npos ? L"." : exe.substr(0, slash);
  return dir + L"\\remesh_cli.exe";
}

std::string RemeshApp::assetPath(const std::string &name) const
{
  return (fs::path(REMESH_DBG_ASSETS_DIR) / (name + ".obj")).generic_string();
}

void RemeshApp::rescanAssets()
{
  std::string keep = (selected >= 0 && selected < (int)assets.size())
                         ? assets[selected]
                         : std::string();
  assets.clear();
  std::error_code ec;
  for (auto &de : fs::directory_iterator(REMESH_DBG_ASSETS_DIR, ec)) {
    if (!de.is_regular_file()) {
      continue;
    }
    auto p = de.path();
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".obj") {
      assets.push_back(p.stem().string());
    }
  }
  std::sort(assets.begin(), assets.end());
  selected = -1;
  for (int i = 0; i < (int)assets.size(); i++) {
    if (assets[i] == keep) {
      selected = i;
      break;
    }
  }
  if (selected < 0 && !assets.empty()) {
    selected = 0;
  }
}

bool RemeshApp::loadObjFile(const std::string &path, std::string &err)
{
  mesh::Mesh *m = mesh::loadObj(path.c_str(), /*keepNgons=*/true);
  if (!m) {
    err = "could not load " + path;
    return false;
  }
  scene_.setMesh(m);
  scene_.buildSpatial(0, 0, 0);
  cameraFit();
  return true;
}

bool RemeshApp::loadAsset(const std::string &name, std::string &err)
{
  std::string path = assetPath(name);
  if (!fs::exists(path)) {
    err = "no such asset: " + name;
    return false;
  }
  if (!loadObjFile(path, err)) {
    return false;
  }
  currentAssetName_ = name;
  rescanAssets();
  for (int i = 0; i < (int)assets.size(); i++) {
    if (assets[i] == name) {
      selected = i;
      break;
    }
  }
  status = "loaded " + name;
  return true;
}

bool RemeshApp::importAsset(const std::string &srcPath, std::string &err)
{
  if (!fs::exists(srcPath)) {
    err = "no such file: " + srcPath;
    return false;
  }
  fs::path src(srcPath);
  fs::path dst = fs::path(REMESH_DBG_ASSETS_DIR) / src.filename();
  std::error_code ec;
  fs::create_directories(REMESH_DBG_ASSETS_DIR, ec);
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    err = "copy failed: " + ec.message();
    return false;
  }
  rescanAssets();
  std::string name = src.stem().string();
  for (int i = 0; i < (int)assets.size(); i++) {
    if (assets[i] == name) {
      selected = i;
      break;
    }
  }
  status = "imported " + name;
  return loadAsset(name, err);
}

bool RemeshApp::startJob(Job kind, const std::wstring &exe,
                         const std::vector<std::wstring> &args,
                         const std::string &label)
{
  proc_.kill(); // join + clean any prior finished job before reusing proc_
  status = label;
  progress = 0.0f;
  stage.clear();
  lastObj.clear();
  lastManifest.clear();
  lastStats.clear();
  if (!proc_.start(exe, args)) {
    status = "ERROR failed to spawn " + label;
    job_ = Job::None;
    return false;
  }
  job_ = kind;
  return true;
}

bool RemeshApp::runRemesh(std::string &err)
{
  if (busy()) {
    err = "a job is already running";
    return false;
  }
  if (selected < 0 || selected >= (int)assets.size()) {
    err = "no asset selected";
    return false;
  }
  std::string name = assets[selected];
  currentAssetName_ = name;
  std::vector<std::wstring> args = {
      L"--input",          widen(assetPath(name)),
      L"--name",           widen(name),
      L"--outdir",         widen(REMESH_DBG_RESULTS_DIR),
      L"--target",         wf(params.target_edge_length),
      L"--solve",          wf(params.solve_edge_length),
      L"--curvature",      wb(params.use_curvature),
      L"--sharp",          wb(params.use_sharp_features),
      L"--sharp-angle",    wf(params.sharp_angle),
      L"--density",        wb(params.use_density),
      L"--reproject",      wb(params.reproject),
      L"--cap-odd",        wb(params.cap_odd_holes),
      L"--smooth",         widen(std::to_string(params.smooth_iterations)),
      L"--smooth-strength", wf(params.smooth_strength),
      L"--seed",           widen(std::to_string((unsigned long)params.seed)),
      L"--triage",         wb(params.triage),
      L"--triage-weld-rel", wf(params.triage_weld_rel),
      L"--triage-min-component-frac", wf(params.triage_min_component_frac),
      L"--curvature-smooth-iters",
      widen(std::to_string(params.curvature_smooth_iters)),
      L"--curvature-smooth-lambda", wf(params.curvature_smooth_lambda),
  };
  if (!startJob(Job::Remesh, remeshCliPath(), args, "remeshing " + name)) {
    err = status;
    return false;
  }
  return true;
}

bool RemeshApp::meshyGen(const std::string &prompt, std::string &err)
{
  if (busy()) {
    err = "a job is already running";
    return false;
  }
  if (prompt.empty()) {
    err = "empty prompt";
    return false;
  }
  std::string script =
      (fs::path(REMESH_DBG_TOOLS_DIR) / "meshy_gen.mjs").generic_string();
  std::vector<std::wstring> args = {widen(script), L"--prompt", widen(prompt)};
  if (!startJob(Job::Meshy, L"node", args, "meshy: " + prompt)) {
    err = status;
    return false;
  }
  return true;
}

void RemeshApp::handleLine(const std::string &line)
{
  if (line.rfind("PROGRESS ", 0) == 0) {
    int pct = 0;
    char st[64] = {0};
    if (std::sscanf(line.c_str(), "PROGRESS %d %63s", &pct, st) >= 1) {
      progress = float(pct) / 100.0f;
      stage = st;
      status = (job_ == Job::Meshy ? "meshy: " : "remesh: ") + stage;
    }
  } else if (line.rfind("RESULT ", 0) == 0) {
    std::string path = line.substr(7);
    lastObj = path;
    std::string err;
    if (loadObjFile(path, err)) {
      // Meshy writes into the assets dir; surface it in the dropdown.
      if (job_ == Job::Meshy) {
        std::string name = fs::path(path).stem().string();
        rescanAssets();
        for (int i = 0; i < (int)assets.size(); i++) {
          if (assets[i] == name) {
            selected = i;
            currentAssetName_ = name;
            break;
          }
        }
      }
      status = "loaded result " + fs::path(path).filename().string();
    } else {
      status = "ERROR " + err;
    }
  } else if (line.rfind("MANIFEST ", 0) == 0) {
    lastManifest = line.substr(9);
  } else if (line.rfind("STATS ", 0) == 0) {
    lastStats = line.substr(6);
  } else if (line.rfind("ERROR", 0) == 0) {
    status = line;
  }
}

void RemeshApp::update()
{
  std::vector<std::string> lines;
  proc_.drain(lines);
  for (auto &l : lines) {
    handleLine(l);
  }
  if (job_ != Job::None && !proc_.running()) {
    // Catch any lines pushed just before the reader observed child exit.
    lines.clear();
    proc_.drain(lines);
    for (auto &l : lines) {
      handleLine(l);
    }
    int code = proc_.exitCode();
    if (code != 0 && status.rfind("ERROR", 0) != 0) {
      status = "job exited with code " + std::to_string(code);
    } else if (code == 0) {
      progress = 1.0f;
    }
    job_ = Job::None;
  }
}

void RemeshApp::cameraFit()
{
  if (!scene_.mesh) {
    return;
  }
  float3 mn(0, 0, 0), mx(0, 0, 0);
  bool have = false;
  for (int v : scene_.mesh->v) {
    float3 co = scene_.mesh->v.co[v];
    if (!have) {
      mn = mx = co;
      have = true;
      continue;
    }
    for (int i = 0; i < 3; i++) {
      mn[i] = std::min(mn[i], co[i]);
      mx[i] = std::max(mx[i], co[i]);
    }
  }
  scene_.camera.frame(mn, mx, float3(1, 1, 1));
  scene_.view = ViewPreset::Persp;
}

bool RemeshApp::screenshot(const std::string &path, std::string &err)
{
  // The live quad-wireframe overlay is an ImGui background draw list (window
  // only); the offscreen capture is the shaded mesh.
  scene_.renderHeadless();
  if (!scene_.screenshot(path.c_str())) {
    err = "capture failed";
    return false;
  }
  return true;
}

std::string RemeshApp::handleCommand(const std::string &line)
{
  std::string cmd, rest;
  splitFirst(line, cmd, rest);
  std::ostringstream o;

  if (cmd.empty()) {
    return "";
  }
  if (cmd == "help") {
    o << "commands:\n"
         "  help                  this list\n"
         "  list_assets           OBJ stems in the assets dir\n"
         "  load_asset <name>     load an asset into the viewport\n"
         "  import_asset <path>   copy an external file into assets + load it\n"
         "  meshy_gen <prompt>    text-to-3D a new asset (async)\n"
         "  get_params            current remesh params (name=value)\n"
         "  set_param <name> <v>  set one remesh param\n"
         "  run_remesh            remesh the selected asset (async)\n"
         "  last_result           obj/manifest/stats of the last run\n"
         "  get_state             JSON: mesh, camera, job, last result\n"
         "  camera_fit            frame the camera to the mesh AABB\n"
         "  camera_get            eye/target/up/fov\n"
         "  screenshot <path>     write a shaded PNG of the offscreen view\n"
         "  quit                  close the app";
    return o.str();
  }
  if (cmd == "list_assets") {
    for (auto &a : assets) {
      o << a << "\n";
    }
    return o.str();
  }
  if (cmd == "load_asset") {
    std::string err;
    return loadAsset(rest, err) ? ("OK " + rest) : ("ERROR " + err);
  }
  if (cmd == "import_asset") {
    std::string err;
    return importAsset(rest, err) ? ("OK imported") : ("ERROR " + err);
  }
  if (cmd == "meshy_gen") {
    std::string err;
    return meshyGen(rest, err) ? "OK started" : ("ERROR " + err);
  }
  if (cmd == "get_params") {
    o << "target_edge_length=" << params.target_edge_length << "\n"
      << "solve_edge_length=" << params.solve_edge_length << "\n"
      << "use_curvature=" << int(params.use_curvature) << "\n"
      << "use_sharp_features=" << int(params.use_sharp_features) << "\n"
      << "sharp_angle=" << params.sharp_angle << "\n"
      << "use_density=" << int(params.use_density) << "\n"
      << "reproject=" << int(params.reproject) << "\n"
      << "cap_odd_holes=" << int(params.cap_odd_holes) << "\n"
      << "smooth_iterations=" << params.smooth_iterations << "\n"
      << "smooth_strength=" << params.smooth_strength << "\n"
      << "seed=" << params.seed << "\n"
      << "triage=" << int(params.triage) << "\n"
      << "triage_weld_rel=" << params.triage_weld_rel << "\n"
      << "triage_min_component_frac=" << params.triage_min_component_frac << "\n"
      << "curvature_smooth_iters=" << params.curvature_smooth_iters << "\n"
      << "curvature_smooth_lambda=" << params.curvature_smooth_lambda;
    return o.str();
  }
  if (cmd == "set_param") {
    std::string name, val;
    splitFirst(rest, name, val);
    if (name.empty() || val.empty()) {
      return "ERROR usage: set_param <name> <value>";
    }
    double d = std::atof(val.c_str());
    int iv = std::atoi(val.c_str());
    if (name == "target_edge_length") {
      params.target_edge_length = float(d);
    } else if (name == "solve_edge_length") {
      params.solve_edge_length = float(d);
    } else if (name == "use_curvature") {
      params.use_curvature = iv != 0;
    } else if (name == "use_sharp_features") {
      params.use_sharp_features = iv != 0;
    } else if (name == "sharp_angle") {
      params.sharp_angle = float(d);
    } else if (name == "use_density") {
      params.use_density = iv != 0;
    } else if (name == "reproject") {
      params.reproject = iv != 0;
    } else if (name == "cap_odd_holes") {
      params.cap_odd_holes = iv != 0;
    } else if (name == "smooth_iterations") {
      params.smooth_iterations = iv;
    } else if (name == "smooth_strength") {
      params.smooth_strength = float(d);
    } else if (name == "seed") {
      params.seed = uint32_t(std::strtoul(val.c_str(), nullptr, 10));
    } else if (name == "triage") {
      params.triage = iv != 0;
    } else if (name == "triage_weld_rel") {
      params.triage_weld_rel = float(d);
    } else if (name == "triage_min_component_frac") {
      params.triage_min_component_frac = float(d);
    } else if (name == "curvature_smooth_iters") {
      params.curvature_smooth_iters = iv;
    } else if (name == "curvature_smooth_lambda") {
      params.curvature_smooth_lambda = float(d);
    } else {
      return "ERROR unknown param: " + name;
    }
    return "OK " + name + "=" + val;
  }
  if (cmd == "run_remesh") {
    std::string err;
    return runRemesh(err) ? "OK started" : ("ERROR " + err);
  }
  if (cmd == "last_result") {
    o << "obj=" << lastObj << "\nmanifest=" << lastManifest
      << "\nstats=" << lastStats;
    return o.str();
  }
  if (cmd == "get_state") {
    int vc = scene_.mesh ? scene_.mesh->v.count : 0;
    int fc = scene_.mesh ? scene_.mesh->f.count : 0;
    auto &c = scene_.camera;
    o << "{\n";
    o << "  \"asset\": \"" << (selected >= 0 ? assets[selected] : "") << "\",\n";
    o << "  \"mesh\": { \"verts\": " << vc << ", \"faces\": " << fc << " },\n";
    o << "  \"job\": { \"running\": " << (busy() ? "true" : "false")
      << ", \"progress\": " << progress << ", \"stage\": \"" << stage
      << "\" },\n";
    o << "  \"camera\": { \"eye\": [" << c.eye[0] << ", " << c.eye[1] << ", "
      << c.eye[2] << "], \"target\": [" << c.target[0] << ", " << c.target[1]
      << ", " << c.target[2] << "] },\n";
    o << "  \"last\": { \"obj\": \"" << lastObj << "\", \"manifest\": \""
      << lastManifest << "\", \"stats\": \"" << lastStats << "\" },\n";
    o << "  \"status\": \"" << status << "\"\n";
    o << "}";
    return o.str();
  }
  if (cmd == "camera_fit") {
    cameraFit();
    return "OK";
  }
  if (cmd == "camera_get") {
    auto &c = scene_.camera;
    o << "eye=" << c.eye[0] << "," << c.eye[1] << "," << c.eye[2] << "\n"
      << "target=" << c.target[0] << "," << c.target[1] << "," << c.target[2]
      << "\n"
      << "up=" << c.up[0] << "," << c.up[1] << "," << c.up[2] << "\n"
      << "fovy=" << c.fovy;
    return o.str();
  }
  if (cmd == "screenshot") {
    if (rest.empty()) {
      return "ERROR usage: screenshot <path>";
    }
    std::string err;
    return screenshot(rest, err) ? ("OK " + rest) : ("ERROR " + err);
  }
  if (cmd == "quit") {
    if (scene_.window) {
      scene_.window->close();
    }
    return "OK";
  }
  return "ERROR unknown command: " + cmd + " (try 'help')";
}

} // namespace sculptcore::debug_app
