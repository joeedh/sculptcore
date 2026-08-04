/* Grids-native GPU stroke path, G4 gate (brush/grid_gpu_session.h). Drives
 * the debug app's grid_stroke verb on a displaced multires cube:
 *   - CPU-grids vs GPU-grids parity: the identical dab battery through the
 *     grids executor and through the wgpu-native dispatcher must land within
 *     float tolerance (both sides share ONE lattice CSR, so the mesh path's
 *     neighbor-order caveat does not apply) — checked for draw and the
 *     for_neighbor smooth kernel;
 *   - undo fidelity through the GPU path: the shared GridStrokeLog captured
 *     leaves before any readback, so grid_undo restores the pre-stroke
 *     positions bit-exactly.
 * Skips (passes) when the wgpu-native compute backend is not built. */
#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"

#include "mesh/mesh.h"

#include <cstdio>
#include <cstring>

test_init;

using namespace sculptcore;
using namespace sculptcore::debug_app;

static bool runOk(Scene &scene, const char *text, const char *tag)
{
  auto r = script::run(scene, text, ".");
  if (!r.ok) {
    fprintf(stderr, "  %s: line %d: %s\n", tag, r.line_no, r.error.c_str());
  }
  test_assert(r.ok);
  return r.ok;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Scene scene(128, 128, /*headless=*/true);

  if (!runOk(scene,
             "make_cube subdivs=4 size=0.5\n"
             "multires_init levels=3 level=3\n"
             "set_brush radius=0.2 strength=0.5\n"
             "set_brush_tool tool=draw\n"
             "save_pos id=base\n",
             "setup"))
  {
    return 1;
  }

  /* Runtime backend probe: the verb reports when no GPU compute backend was
   * compiled in (the define is private to debug_core, so the test can't
   * check it) — treat that one error as a skip. */
  {
    auto r = script::run(scene,
                         "grid_stroke origin=0,0,0.25 normal=0,0,1 backend=gpu\n",
                         ".");
    if (!r.ok) {
      if (std::strstr(r.error.c_str(), "no GPU compute backend")) {
        fprintf(stderr, "grid gpu gates skipped (%s)\n", r.error.c_str());
        return 0;
      }
      fprintf(stderr, "  probe: %s\n", r.error.c_str());
      test_assert(r.ok);
      return 1;
    }
    // Roll the probe stroke back so the A/B below starts from `base`.
    if (!runOk(scene, "grid_undo\nassert_pos id=base eps=1e-6\n", "probe-undo")) {
      return 1;
    }
  }

  /* Draw: CPU stroke -> snapshot -> undo -> GPU stroke -> compare. */
  if (!runOk(scene,
             "grid_stroke origin=0,0,0.25 normal=0,0,1 dabs=3 step=0.05,0,0\n"
             "save_pos id=cpu\n"
             "grid_undo\n"
             "assert_pos id=base eps=1e-6\n"
             "grid_stroke origin=0,0,0.25 normal=0,0,1 dabs=3 step=0.05,0,0 "
             "backend=gpu\n"
             "assert_pos id=cpu eps=1e-5\n"
             "grid_undo\n"
             "assert_pos id=base eps=1e-6\n",
             "draw-ab"))
  {
    return 1;
  }

  /* Smooth (for_neighbor over the shared lattice CSR). */
  if (!runOk(scene,
             "set_brush_tool tool=smooth\n"
             "save_pos id=base2\n"
             "grid_stroke origin=0,0,0.25 normal=0,0,1 dabs=2 step=0.04,0,0\n"
             "save_pos id=cpu2\n"
             "grid_undo\n"
             "assert_pos id=base2 eps=1e-6\n"
             "grid_stroke origin=0,0,0.25 normal=0,0,1 dabs=2 step=0.04,0,0 "
             "backend=gpu\n"
             "assert_pos id=cpu2 eps=1e-5\n"
             "grid_undo\n"
             "assert_pos id=base2 eps=1e-6\n",
             "smooth-ab"))
  {
    return 1;
  }

  fprintf(stderr, "grid gpu gates passed\n");
  /* Skip test_end(): attr name strings stay live in the alloc tracker
   * (mirrors the other debug_core tests). */
  return retval;
}
