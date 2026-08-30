// M1 gate for the app-facing GpuBrush_* seam (plans/gpuGlobalBrushes.md):
// begin → marshal one dab → end with the unchanged begin-co blob must be a
// byte-exact no-op on the mesh, and the stroke's MeshLog step must survive an
// undo → redo round trip without drift. Also sanity-checks the marshaled blob
// sizes/contents against compute_layout.h so a layout regression fails here
// before it ever reaches a GPU.
#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"

#include "brush/brush_executor.h"
#include "brush/gpu_brush_session.h"
#include "mesh/mesh.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>
#include <cstring>

test_init;

using namespace sculptcore::debug_app;
using namespace sculptcore::mesh;
using namespace litestl::math;
namespace brush = sculptcore::brush;

// Body lives in a scope so the Scene destructs before test_end()'s leak check.
static void runTest()
{
  Scene scene(64, 64, /*headless=*/true);
  auto r = script::run(scene,
                       "make_cube subdivs=16 size=0.5\n"
                       "build_spatial leaf_limit=256 depth_limit=8\n",
                       ".");
  test_assert(r.ok);

  Mesh *m = scene.mesh;
  litestl::util::Vector<float3> start;
  start.resize(m->v.count);
  for (int i = 0; i < m->v.count; i++) {
    start[i] = m->v.co[i];
  }

  scene.currentTool = sculptcore::brush::SculptBrushes::KELVINLET;
  scene.brush.radius = 0.2f;
  scene.brush.strength = 1.0f;
  scene.brush.mu = 1.0f;
  scene.brush.nu = 0.6f; // deliberately out of range: host clamp must cap it
  scene.brush.grabFrom = float3{0, 0, 0.25f};
  scene.brush.grabTo = float3{0, 0.1f, 0};
  scene.brush.writeProps();

  sculptcore::brush::CommandExecutor exec(scene.tree, &scene.brush);
  exec.meshLog = &scene.meshLog;
  exec.setStrokeGen(1);
  exec.beginStep(false);

  // Abort path: a session freed before any dab must leave no trace.
  {
    void *aborted = GpuBrush_beginStroke(
        m, scene.tree, &scene.brush, &scene.meshLog, int(scene.currentTool));
    test_assert(aborted != nullptr);
    GpuBrush_free(aborted);
  }

  void *s = GpuBrush_beginStroke(
      m, scene.tree, &scene.brush, &scene.meshLog, int(scene.currentTool));
  test_assert(s != nullptr);
  test_assert(std::strcmp(GpuBrush_kernelName(s), "kelvinlet") == 0);
  test_assert(GpuBrush_info(s, brush::GPUBRUSH_INFO_ELEM_COUNT) == m->v.count);
  // Anchored field: never non-accumulate-eligible (was IS_GLOBAL, now retired).
  test_assert(GpuBrush_info(s, brush::GPUBRUSH_INFO_ACCUMULABLE) == 0);
  test_assert(GpuBrush_info(s, brush::GPUBRUSH_INFO_NEEDS_NEIGHBORS) == 0);

  // Begin blobs: packed-xyz co must equal the live mesh.
  test_assert(GpuBrush_dataSize(s, brush::GPUBRUSH_DATA_CO) ==
              m->v.count * 3 * int(sizeof(float)));
  const float *beginCo =
      static_cast<const float *>(GpuBrush_dataPtr(s, brush::GPUBRUSH_DATA_CO));
  test_assert(beginCo != nullptr);
  for (int i = 0; i < m->v.count; i++) {
    test_assert(beginCo[i * 3 + 0] == m->v.co[i][0]);
    test_assert(beginCo[i * 3 + 1] == m->v.co[i][1]);
    test_assert(beginCo[i * 3 + 2] == m->v.co[i][2]);
  }

  // Marshal one dab at the +Z pole.
  int chunkCount = GpuBrush_marshalDab(s,
                                       0,
                                       0,
                                       0.25f,
                                       0,
                                       0,
                                       1,
                                       0.2f,
                                       0.3f,
                                       /*mirrorIdx=*/0,
                                       /*nonaccum=*/0);
  test_assert(chunkCount > 0);
  test_assert(GpuBrush_info(s, brush::GPUBRUSH_INFO_NODE_COUNT) == chunkCount);
  test_assert(GpuBrush_info(s, brush::GPUBRUSH_INFO_UNIQUE_COUNT) > 0);
  test_assert(GpuBrush_info(s, brush::GPUBRUSH_INFO_UVERTS_CHANGED) == 1);
  test_assert(GpuBrush_info(s, brush::GPUBRUSH_INFO_DAB_GEN) == 1);

  // Layout pins (compute_layout.h): 112-byte brush uniforms with kelvinlet's
  // mu/nu at 72/76 (nu host-clamped) and color's mixMode tail at 96, 256-byte
  // ctx uniforms (128-byte base incl. the view-normal params at 96) with
  // grabFrom at 128, 256-float falloff LUT, 32-byte stroke samples.
  test_assert(GpuBrush_dataSize(s, brush::GPUBRUSH_DATA_BRUSH_UNIFORMS) == 112);
  test_assert(GpuBrush_dataSize(s, brush::GPUBRUSH_DATA_CTX_UNIFORMS) == 256);
  test_assert(GpuBrush_dataSize(s, brush::GPUBRUSH_DATA_FALLOFF_LUT) ==
              256 * int(sizeof(float)));
  test_assert(GpuBrush_dataSize(s, brush::GPUBRUSH_DATA_STROKE_PATH) ==
              GpuBrush_info(s, brush::GPUBRUSH_INFO_STROKE_SAMPLE_COUNT) * 32);
  const float *bu = static_cast<const float *>(
      GpuBrush_dataPtr(s, brush::GPUBRUSH_DATA_BRUSH_UNIFORMS));
  test_assert(bu[0] == 1.0f);                          // strength
  test_assert(bu[1] == 0.2f);                          // radius
  test_assert(bu[72 / 4] == 1.0f);                     // mu
  test_assert(std::fabs(bu[76 / 4] - 0.499f) < 1e-6f); // nu, clamped from 0.6
  const float *cu =
      static_cast<const float *>(GpuBrush_dataPtr(s, brush::GPUBRUSH_DATA_CTX_UNIFORMS));
  test_assert(cu[0] == 0.0f && cu[2] == 0.25f); // surfacePos
  test_assert(cu[128 / 4 + 2] == 0.25f);        // grabFrom.z at ctx tail
  test_assert(cu[144 / 4 + 1] == 0.1f);         // grabTo.y

  // A second identical image must not re-flag the index arrays.
  int chunkCount2 = GpuBrush_marshalDab(s,
                                        0,
                                        0,
                                        0.25f,
                                        0,
                                        0,
                                        1,
                                        0.2f,
                                        0.3f,
                                        /*mirrorIdx=*/1,
                                        /*nonaccum=*/0);
  test_assert(chunkCount2 == chunkCount);
  test_assert(GpuBrush_info(s, brush::GPUBRUSH_INFO_UVERTS_CHANGED) == 0);
  test_assert(GpuBrush_info(s, brush::GPUBRUSH_INFO_DAB_GEN) == 1); // mirror: no bump

  // End with the unchanged begin co — a no-op stroke.
  litestl::util::Vector<float> coCopy;
  coCopy.resize(size_t(m->v.count) * 3);
  std::memcpy(coCopy.data(), beginCo, coCopy.size() * sizeof(float));
  GpuBrush_endStroke(s, coCopy.data(), nullptr, m->v.count);
  exec.endStep();

  auto maxErr = [&]() {
    float e = 0.0f;
    for (int i = 0; i < m->v.count; i++) {
      for (int j = 0; j < 3; j++) {
        e = std::fmax(e, std::fabs(m->v.co[i][j] - start[i][j]));
      }
    }
    return e;
  };
  fprintf(stderr, "after endStroke: maxErr=%g\n", maxErr());
  test_assert(maxErr() == 0.0f);

  // The step must also survive undo → redo without drift.
  scene.meshLog.undo(m, scene.tree);
  fprintf(stderr, "after undo: maxErr=%g\n", maxErr());
  test_assert(maxErr() == 0.0f);
  scene.meshLog.redo(m, scene.tree);
  fprintf(stderr, "after redo: maxErr=%g\n", maxErr());
  test_assert(maxErr() == 0.0f);
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  runTest();
  return test_end();
}
