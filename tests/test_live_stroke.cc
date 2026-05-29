// Regression: a WGSL GPU stroke that runs after a C++-backend stroke must not
// crash. The C++ executor leaves the mesh in frozen-topology mode (it drops
// f.l/l.c/c.next/l.size, keeping only .corner.v, and re-freezes on its next dab
// rather than thawing at stroke end). The GPU live-render path's begin() then
// triangulates the whole mesh (buildNormalTopology) and walks the 1-ring for
// neighbor kernels — both read those dropped link columns and segfault unless
// begin() thaws first. This drives InteractiveController headlessly so
// liveBackend_ binds the offscreen backend (the path that calls
// buildNormalTopology); a script `stroke` verb uses the batch path and would
// not exercise it. GPU-dependent: self-skips if no Vulkan device is available.
#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"
#include "debug/interactive.h"
#include "debug/input.h"

#include "mesh/mesh.h"

#include <cstdio>

test_init;

using namespace sculptcore::debug_app;

static InputEvent mouseBtn(MouseButton b, ButtonAction a, float x, float y)
{
  InputEvent e;
  e.kind = InputKind::MouseButton;
  e.u.mb.button = b; e.u.mb.action = a; e.u.mb.mods.bits = 0; e.u.mb.x = x; e.u.mb.y = y;
  return e;
}
static InputEvent mouseMove(float x, float y)
{
  InputEvent e;
  e.kind = InputKind::CursorPos; e.u.cursor.x = x; e.u.cursor.y = y;
  return e;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Scene scene(256, 256, /*headless=*/true);
  const char *src =
      "make_cube subdivs=12 size=0.5\n"
      "build_spatial leaf_limit=256 depth_limit=8\n"
      "set_brush_tool tool=draw\n"
      "set_brush radius=0.25 strength=0.5\n"
      "set_backend backend=cpp\n"
      "stroke origin=0,0,0.25 normal=0,0,1\n"  // freezes topology
      "set_backend backend=wgsl\n"
      "view preset=persp\n";
  auto r = script::run(scene, src, ".");
  test_assert(r.ok);
  if (!r.ok) {
    fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    return 1;
  }

  if (!scene.ensureGPU()) {
    fprintf(stderr, "test_live_stroke: no GPU device; skipping.\n");
    return 0;
  }

  /* Precondition: the C++ stroke must have left the mesh frozen, else the test
   * isn't exercising the regression. */
  test_assert(scene.mesh->topo_frozen);

  /* WGSL stroke through the interactive live path: begin() -> buildNormalTopology
   * triangulates the frozen mesh. Pre-fix this segfaults in triangulateFace. */
  InteractiveController ctrl(&scene);
  const float cx = 128.0f, cy = 128.0f;
  scene.renderHeadless();
  ctrl.handle(mouseBtn(MouseButton::Left, ButtonAction::Press, cx, cy));
  scene.renderHeadless();
  for (int i = 1; i <= 8; i++) {
    ctrl.handle(mouseMove(cx + i * 6.0f, cy + i * 2.0f));
    scene.renderHeadless();
  }
  ctrl.handle(mouseBtn(MouseButton::Left, ButtonAction::Release, cx + 48.0f, cy + 16.0f));
  scene.renderHeadless();

  /* Thaw must have happened so the live path could triangulate. */
  test_assert(!scene.mesh->topo_frozen);
  return 0;
}
