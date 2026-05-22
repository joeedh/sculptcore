#include "input.h"
#include "interactive.h"
#include "scene.h"
#include "script.h"
#include "ui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace sculptcore::debug_app;

namespace {

void usage()
{
  std::fprintf(stderr,
               "debug_app --script PATH [--out DIR] [--headless] [--width N] [--height N]\n"
               "          [--no-headless] [--interactive]\n");
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

  Scene scene(width, height, headless);

  auto r = script::runFile(scene, scriptPath, outDir);
  if (!r.ok) {
    std::fprintf(stderr, "script error at line %d: %s\n",
                 r.line_no, r.error.c_str());
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
      ui.beginFrame();
      scene.renderWindow();
    }
    ui.shutdown();
    dispatcher.detach();
  }
  return 0;
}
