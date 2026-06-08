// Interactive quad-remesh debug app. Loads/imports/generates assets, runs the
// standalone remesh_cli as a subprocess (so the remesher can be rebuilt without
// restarting this app), and shows the shaded result with a quad-edge wireframe.
// Orbit/pan/zoom with the mouse; an agent can drive it over the named pipe
// \\.\pipe\sculpt-remesh-debug (see tools/remesh_dbg.mjs, or send `help`).

#include "input.h"
#include "remesh_app.h"
#include "remesh_pipe_server.h"
#include "remesh_ui.h"
#include "scene.h"

#include "mesh/mesh.h"
#include "window/window.h"

#include "litestl/util/alloc.h"

#include "imgui.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace sculptcore::debug_app;
using litestl::math::float2;
using litestl::math::float3;
using litestl::math::mat4;

namespace {

constexpr int kMaxWireEdges = 250000; // keep the per-frame ImGui overlay bounded

// Orbit/pan/zoom controller for the read-only remesh viewer: plain LMB or RMB
// orbits, Shift+LMB or MMB pans, scroll zooms. No sculpting (unlike debug_app's
// InteractiveController). Camera math mirrors interactive.cc.
struct OrbitController : InputHandler {
  OrbitController(Scene *scene) : scene_(scene) {}

  bool handle(const InputEvent &e) override
  {
    switch (e.kind) {
    case InputKind::CursorPos: {
      float2 cur(e.u.cursor.x, e.u.cursor.y);
      float2 d(cur[0] - cursor_[0], cur[1] - cursor_[1]);
      cursor_ = cur;
      if (orbit_) {
        doOrbit(d);
      } else if (pan_) {
        doPan(d);
      }
      return false;
    }
    case InputKind::MouseButton: {
      bool press = e.u.mb.action == ButtonAction::Press;
      cursor_ = float2(e.u.mb.x, e.u.mb.y);
      bool shift = (e.u.mb.mods.bits & 0x1) != 0;
      if (e.u.mb.button == MouseButton::Left) {
        lmb_ = press;
        orbit_ = press && !shift;
        pan_ = press && shift;
      } else if (e.u.mb.button == MouseButton::Right) {
        orbit_ = press;
      } else if (e.u.mb.button == MouseButton::Middle) {
        pan_ = press;
      }
      if (!press) {
        // clear whichever modes the released button could have set
        if (e.u.mb.button == MouseButton::Left) {
          orbit_ = pan_ = false;
        } else if (e.u.mb.button == MouseButton::Right) {
          orbit_ = false;
        } else if (e.u.mb.button == MouseButton::Middle) {
          pan_ = false;
        }
      }
      return false;
    }
    case InputKind::Scroll:
      doZoom(e.u.scroll.dy);
      return true;
    case InputKind::FramebufferSize:
      scene_->handleResize();
      return false;
    default:
      return false;
    }
  }

private:
  void fbSize(int &w, int &h) const
  {
    w = scene_->swapchain.width > 0 ? scene_->swapchain.width : scene_->width;
    h = scene_->swapchain.height > 0 ? scene_->swapchain.height : scene_->height;
  }

  void doOrbit(float2 delta)
  {
    constexpr float kRadPerPx = 0.005f;
    float3 worldUp(0, 0, 1);
    auto rot = [](float3 v, float3 a, float ang) -> float3 {
      float c = std::cos(ang), s = std::sin(ang);
      return v * c + a.cross(v) * s + a * (a.dot(v) * (1.0f - c));
    };
    float3 rel = scene_->camera.eye - scene_->camera.target;
    rel = rot(rel, worldUp, -delta[0] * kRadPerPx);
    float3 fwd = rel * -1.0f;
    fwd.normalize();
    float3 right = fwd.cross(worldUp);
    if (right.lengthSqr() < 1e-8f) {
      right = float3(1, 0, 0);
    } else {
      right.normalize();
    }
    rel = rot(rel, right, -delta[1] * kRadPerPx);
    scene_->camera.eye = scene_->camera.target + rel;
  }

  void doPan(float2 delta)
  {
    float3 fwd = scene_->camera.target - scene_->camera.eye;
    float dist = fwd.length();
    if (dist < 1e-6f) {
      return;
    }
    fwd.normalize();
    float3 right = fwd.cross(scene_->camera.up);
    right.normalize();
    float3 up = right.cross(fwd);
    up.normalize();
    int w = 0, h = 0;
    fbSize(w, h);
    if (h <= 0) {
      return;
    }
    float worldPerPx =
        (2.0f * dist * std::tan(scene_->camera.fovy * 0.5f)) / float(h);
    float3 ofs = right * (-delta[0] * worldPerPx) + up * (delta[1] * worldPerPx);
    scene_->camera.eye = scene_->camera.eye + ofs;
    scene_->camera.target = scene_->camera.target + ofs;
  }

  void doZoom(float dy)
  {
    float3 rel = scene_->camera.eye - scene_->camera.target;
    float dist = rel.length();
    if (dist < 1e-6f) {
      return;
    }
    float newDist = dist * std::pow(1.1f, -dy);
    if (newDist < 0.05f) {
      newDist = 0.05f;
    }
    scene_->camera.eye = scene_->camera.target + rel * (newDist / dist);
  }

  Scene *scene_;
  float2 cursor_{0, 0};
  bool lmb_ = false, orbit_ = false, pan_ = false;
};

// Project a world point to clip space (column-major mat4). Returns false if it
// is at/behind the eye plane (w <= eps), so the caller skips that edge.
bool projectClip(const mat4 &m, float3 p, float &sx, float &sy, float W, float H)
{
  const float *d = static_cast<const float *>(m);
  float x = d[0] * p[0] + d[4] * p[1] + d[8] * p[2] + d[12];
  float y = d[1] * p[0] + d[5] * p[1] + d[9] * p[2] + d[13];
  float w = d[3] * p[0] + d[7] * p[1] + d[11] * p[2] + d[15];
  if (w <= 1e-6f) {
    return false;
  }
  float ndcX = x / w, ndcY = y / w;
  sx = (ndcX * 0.5f + 0.5f) * W;
  sy = (0.5f - ndcY * 0.5f) * H;
  return true;
}

// Draw mesh edges (real quad edges — the renderer fans quads to tris for the
// shaded pass, so this overlay is the only way to see the quad topology) into
// ImGui's background draw list. Must run between beginFrame() and renderWindow().
void drawQuadWireframe(Scene &scene, bool enabled)
{
  if (!enabled || !scene.mesh) {
    return;
  }
  ImGuiIO &io = ImGui::GetIO();
  float W = io.DisplaySize.x, H = io.DisplaySize.y;
  if (W <= 0 || H <= 0) {
    return;
  }
  int sw = scene.swapchain.width > 0 ? scene.swapchain.width : scene.width;
  int sh = scene.swapchain.height > 0 ? scene.swapchain.height : scene.height;
  float aspect = sh > 0 ? float(sw) / float(sh) : 1.0f;
  mat4 vp = scene.camera.viewProj(aspect);

  ImDrawList *dl = ImGui::GetBackgroundDrawList();
  ImU32 col = IM_COL32(20, 25, 35, 170);
  auto &m = *scene.mesh;
  int drawn = 0;
  for (int e : m.e) {
    auto ev = m.e.vs[e];
    float sx0, sy0, sx1, sy1;
    if (!projectClip(vp, m.v.co[ev[0]], sx0, sy0, W, H)) {
      continue;
    }
    if (!projectClip(vp, m.v.co[ev[1]], sx1, sy1, W, H)) {
      continue;
    }
    dl->AddLine(ImVec2(sx0, sy0), ImVec2(sx1, sy1), col, 1.0f);
    if (++drawn >= kMaxWireEdges) {
      break;
    }
  }
}

void usage()
{
  std::fprintf(stderr,
               "remesh_debug_app [--asset NAME] [--width N] [--height N]\n"
               "  --asset NAME   load this asset (OBJ stem) on startup\n"
               "  Mouse: LMB/RMB orbit, Shift+LMB/MMB pan, scroll zoom.\n"
               "  Pipe:  \\\\.\\pipe\\sculpt-remesh-debug (send 'help').\n");
}

} // namespace

int main(int argc, char **argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  int width = 1280, height = 800;
  const char *asset = nullptr;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    auto next = [&](const char *n) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", n);
        std::exit(2);
      }
      return argv[++i];
    };
    if (std::strcmp(a, "--asset") == 0) {
      asset = next("--asset");
    } else if (std::strcmp(a, "--width") == 0) {
      width = std::atoi(next("--width"));
    } else if (std::strcmp(a, "--height") == 0) {
      height = std::atoi(next("--height"));
    } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", a);
      usage();
      return 2;
    }
  }

  {
    Scene scene(width, height, /*headless=*/false);
    RemeshApp app(scene);
    app.rescanAssets();

    std::string err;
    if (asset) {
      if (!app.loadAsset(asset, err)) {
        std::fprintf(stderr, "load %s: %s\n", asset, err.c_str());
      }
    } else if (!scene.mesh && app.selected >= 0) {
      app.loadAsset(app.assets[app.selected], err);
    }

    if (!scene.ensureGPU() || !scene.window) {
      std::fprintf(stderr, "failed to bring up window/GPU\n");
      return 1;
    }

    InputDispatcher dispatcher;
    RemeshUi ui(&scene, &app);
    OrbitController controller(&scene);
    dispatcher.addHandler(&ui); // first: short-circuits the controller on ImGui focus
    dispatcher.addHandler(&controller);
    dispatcher.attach(scene.window->handle());

    if (!ui.init()) {
      std::fprintf(stderr, "Ui::init failed; continuing without panel\n");
    }

    PipeServer pipe;
    if (!pipe.start(L"\\\\.\\pipe\\sculpt-remesh-debug",
                    [&app](const std::string &line) {
                      return app.handleCommand(line);
                    })) {
      std::fprintf(stderr, "warning: pipe server failed to start\n");
    } else {
      std::printf("pipe server: \\\\.\\pipe\\sculpt-remesh-debug\n");
    }

    while (!scene.window->shouldClose()) {
      scene.window->poll();
      pipe.drainMainThreadQueue(); // run queued pipe commands on this thread
      app.update();                // parse subprocess stdout, apply results
      ui.beginFrame();
      drawQuadWireframe(scene, app.showWireframe);
      scene.renderWindow();
    }

    pipe.stop();
    ui.shutdown();
    dispatcher.detach();
  }

  if (litestl::alloc::getMemorySize() > 0) {
    std::printf("=== memory leaks: ===\n");
  }
  litestl::alloc::print_blocks(false);
  std::fflush(stdout);
  return 0;
}
