#include "input.h"
#include "interactive.h"
#include "scene.h"
#include "script.h"
#include "ui.h"

// CLAUDENOTE: CB-M0 scaffolding (plan 2026-07-12-2141-meshlog-callback-batching)
#include "meshlog/cb_prof.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace sculptcore::debug_app;

namespace {

void usage()
{
  std::fprintf(
      stderr,
      "debug_app --script PATH [--out DIR] [--headless] [--width N] [--height N]\n"
      "          [--no-headless] [--interactive] [--backend cpp|wgsl]\n"
      "          [--gpu-capture PREFIX] [--max-undo N] [--profile] [--reorder]\n");
}

bool ensureDir(const char *path)
{
  if (!path || path[0] == 0) {
    return true;
  }
#ifdef _WIN32
  std::string cmd = std::string("if not exist \"") + path + "\" mkdir \"" + path + "\"";
  return std::system(cmd.c_str()) == 0;
#else
  std::string cmd = std::string("mkdir -p \"") + path + "\"";
  return std::system(cmd.c_str()) == 0;
#endif
}

} // namespace

int main(int argc, char **argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  const char *scriptPath = nullptr;
  const char *outDir = ".";
  bool headless = true;
  bool interactive = false;
  int width = 1024;
  int height = 768;
  const char *backendArg = nullptr;
  const char *gpuCapture = nullptr;
  int maxUndo = -1; // -1 = unbounded undo history
  bool profile = false;
  bool reorder = false;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    auto next = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (std::strcmp(a, "--script") == 0) {
      scriptPath = next("--script");
    } else if (std::strcmp(a, "--out") == 0) {
      outDir = next("--out");
    } else if (std::strcmp(a, "--width") == 0) {
      width = std::atoi(next("--width"));
    } else if (std::strcmp(a, "--height") == 0) {
      height = std::atoi(next("--height"));
    } else if (std::strcmp(a, "--headless") == 0) {
      headless = true;
    } else if (std::strcmp(a, "--no-headless") == 0) {
      headless = false;
    } else if (std::strcmp(a, "--interactive") == 0) {
      interactive = true;
      headless = false;
    } else if (std::strcmp(a, "--backend") == 0) {
      backendArg = next("--backend");
    } else if (std::strcmp(a, "--gpu-capture") == 0) {
      gpuCapture = next("--gpu-capture");
    } else if (std::strcmp(a, "--max-undo") == 0) {
      maxUndo = std::atoi(next("--max-undo"));
    } else if (std::strcmp(a, "--profile") == 0) {
      profile = true;
    } else if (std::strcmp(a, "--reorder") == 0) {
      reorder = true;
    } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", a);
      usage();
      return 2;
    }
  }

  if (!scriptPath) {
    usage();
    return 2;
  }
  ensureDir(outDir);

  // create a new scope so we get desctructors called before print leaks
  {
    Scene scene(width, height, headless);
    scene.meshLog.setMaxUndoSteps(maxUndo);
    scene.profiler.enabled = profile;
    // CLAUDENOTE: CB-M0 scaffolding (plan 2026-07-12-2141)
    sculptcore::meshlog::cbprof::get().enabled = profile;
    scene.reorderOnBuild = reorder;

    if (backendArg) {
      if (std::strcmp(backendArg, "cpp") == 0) {
        scene.currentBackend = BrushBackend::Cpp;
      } else if (std::strcmp(backendArg, "wgsl") == 0) {
#ifdef SBRUSH_BACKEND_WGSL
        scene.currentBackend = BrushBackend::Wgsl;
#else
        std::fprintf(stderr,
                     "--backend=wgsl: WGSL backend not compiled in "
                     "(configure with --backends=cpp,wgsl)\n");
        return 2;
#endif
      } else if (std::strcmp(backendArg, "webgpu") == 0) {
#ifdef SBRUSH_WEBGPU_COMPUTE
        scene.currentBackend = BrushBackend::WgpuNative;
#else
        std::fprintf(stderr,
                     "--backend=webgpu: WebGPU backend not compiled in "
                     "(configure with -DSBRUSH_WEBGPU_COMPUTE=ON "
                     "--backends=cpp,wgsl,spirv)\n");
        return 2;
#endif
      } else {
        std::fprintf(
            stderr, "--backend: unknown value '%s' (valid: cpp, wgsl, webgpu)\n", backendArg);
        return 2;
      }
    }

    if (gpuCapture) {
      /* Fixtures land alongside the JSON dumps in outDir. */
      std::string p = outDir;
      if (!p.empty() && p.back() != '/') {
        p += '/';
      }
      scene.gpuCapturePrefix = p + gpuCapture;
    }

    auto r = script::runFile(scene, scriptPath, outDir);
    if (!r.ok) {
      std::fprintf(stderr, "script error at line %d: %s\n", r.line_no, r.error.c_str());
      return 1;
    }

    if (interactive) {
      if (!scene.ensureGPU() || !scene.window) {
        std::fprintf(stderr, "interactive: failed to bring up window/GPU\n");
        return 1;
      }
      InputDispatcher dispatcher;
      Ui ui(&scene);
      InteractiveController controller(&scene);
      /* Order matters: Ui runs first and short-circuits the controller when
       * ImGui has focus. The actual GLFW event delivery to ImGui happens
       * through its chained callbacks installed inside Ui::init(). */
      dispatcher.addHandler(&ui);
      dispatcher.addHandler(&controller);
      dispatcher.attach(scene.window->handle());

      if (!ui.init()) {
        std::fprintf(stderr, "interactive: Ui::init failed; continuing without panel\n");
      }

      while (!scene.window->shouldClose()) {
        scene.window->poll();
        // Drain a frame's worth of WgpuNative dabs in a single readback (no-op
        // for other backends / when no stroke is open).
        controller.flushGpuReadback();
        ui.beginFrame();
        scene.renderWindow();
      }
      ui.shutdown();
      dispatcher.detach();
    }

    scene.profiler.printSummary();
    // CLAUDENOTE: CB-M0 scaffolding (plan 2026-07-12-2141)
    sculptcore::meshlog::cbprof::get().print("session");
  }

  if (litestl::alloc::getMemorySize() > 0) {
    printf("=== memory leaks: ===\n");
  }
  litestl::alloc::print_blocks(false);
  std::fflush(stdout);

  return 0;
}
