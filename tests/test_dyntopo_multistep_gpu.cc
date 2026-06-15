/* Regression: GPU upload currency across a MULTI-STEP undo/redo walk.
 *
 * The ts2.wproj field repro is several separate dyntopo strokes (each its own
 * meshlog step); undoing all the way down then redoing back up showed the
 * uploaded GPU position buffer drifting (different corner COUNT than the forward
 * pass at the same cursor) even though mesh co was bit-identical. A single-step
 * test can't surface that — the partition/GPU-node set is only re-derived when
 * topology changes, and across multiple steps the deferred split/merge cadence
 * and ownership replay interact. This drives N separate strokes through the
 * unified executor, snapshots the uploaded GPU positions at the top, then walks
 * all the way down (undo) and back up (redo) and asserts the top-of-stack GPU
 * buffer is reproduced exactly — same corner multiset AND same count. */
#include "test_util.h"

#include "brush/brush_executor.h"
#include "debug/scene.h"
#include "debug/script.h"
#include "gpu/vbo.h"
#include "mesh/mesh.h"
#include "spatial/spatial.h"

#include "litestl/math/vector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::debug_app;
using litestl::math::float3;

/* The bytes the GPU actually draws: every GPU node's pos buffer after update(). */
static void collectGpuPos(spatial::SpatialTree *tree, litestl::util::Vector<float3> &out)
{
  out.clear();
  for (spatial::SpatialNode *node : tree->gpu_nodes()) {
    if (!node->gpu_data || !node->gpu_data->pos) {
      continue;
    }
    gpu::Buffer *pos = node->gpu_data->pos;
    float3 *d = pos->get_data<float3>();
    for (int i = 0; i < pos->size; i++) {
      out.append(d[i]);
    }
  }
  std::sort(out.begin(), out.end(), [](const float3 &a, const float3 &b) {
    if (a[0] != b[0]) return a[0] < b[0];
    if (a[1] != b[1]) return a[1] < b[1];
    return a[2] < b[2];
  });
}

/* Mirror the real app's per-frame cadence: many update() calls per stroke, so
 * the slow deferred-merge pass (every mergeCadence_-th update) actually fires
 * and interacts with undo/redo — the field repro's regime, not one update/step. */
static void pump(Scene &scene)
{
  for (int i = 0; i < 10; i++) {
    scene.tree->update(&scene.gpu);
  }
}

static int cmpGpu(const litestl::util::Vector<float3> &a,
                  const litestl::util::Vector<float3> &b, float &maxw)
{
  maxw = 0.0f;
  int diff = 0;
  int n = (int)std::min(a.size(), b.size());
  for (int i = 0; i < n; i++) {
    float d = (a[i] - b[i]).length();
    if (d > 1e-5f) {
      diff++;
    }
    maxw = std::max(maxw, d);
  }
  return diff;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Scene scene(256, 256, /*headless=*/true);
  const char *src = "make_cube subdivs=24 size=0.5\n"
                    "build_spatial leaf_limit=128 depth_limit=10\n"
                    "set_brush_tool tool=draw\n"
                    "set_brush radius=0.18 strength=0.5\n";
  auto r = script::run(scene, src, ".");
  test_assert(r.ok);
  if (!r.ok) {
    fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    return 1;
  }

  mesh::Mesh *m = scene.mesh;

  scene.dyntopoEnabled = true;
  scene.dyntopoParams.l_max = 0.05f;
  scene.dyntopoParams.l_min = 0.02f;
  scene.dyntopoParams.mode = dyntopo::DynTopoMode::Both;

  const float3 normal(0, 0, 1);
  const float radius = scene.brush.radius;

  brush::CommandExecutor exec(scene.tree, &scene.brush);
  exec.meshLog = &scene.meshLog;
  exec.ctx.renderMatrix = scene.renderMatrix;

  /* N separate strokes, each its own meshlog step — like the field repro. Snapshot
   * the uploaded GPU positions after each forward step. */
  const int NSTEPS = 4;
  const int NDABS = 5;
  litestl::util::Vector<litestl::util::Vector<float3>> gpuFwd;
  gpuFwd.resize(NSTEPS);

  for (int s = 0; s < NSTEPS; s++) {
    exec.beginStep(true);
    for (int d = 0; d < NDABS; d++) {
      float t = float(d) / float(NDABS - 1);
      /* Sweep a different band of the +Z face each stroke. */
      float bx = -0.18f + 0.12f * float(s);
      float3 origin(bx + 0.1f * t, -0.15f + 0.3f * t, 0.25f);
      exec.applyDab(scene.currentTool, origin, normal, radius,
                    &scene.dyntopoParams, scene.dyntopoSeed + uint32_t(s * 16 + d));
    }
    exec.endDynTopoStroke();
    exec.endStep();
    /* Mirror per-frame tree maintenance between strokes (deferred split/merge). */
    pump(scene);
    collectGpuPos(scene.tree, gpuFwd[s]);
    printf("  step %d: v=%d f=%d gpuCorners=%d\n", s, m->v.count, m->f.count,
           (int)gpuFwd[s].size());
  }

  /* Undo all the way to the base, then redo back to the top, mirroring the field
   * repro's descent/ascent. Drive update() after each so GPU buffers refresh. */
  printf("  -- undo descent --\n");
  for (int s = NSTEPS - 1; s >= 0; s--) {
    scene.meshLog.undo(m, scene.tree);
    pump(scene);
  }

  printf("  -- redo ascent --\n");
  for (int s = 0; s < NSTEPS; s++) {
    scene.meshLog.redo(m, scene.tree);
    pump(scene);
    litestl::util::Vector<float3> gpuNow;
    collectGpuPos(scene.tree, gpuNow);
    float maxw = 0.0f;
    int diff = cmpGpu(gpuFwd[s], gpuNow, maxw);
    printf("  redo step %d: gpuCorners fwd=%d now=%d, diff=%d maxw=%.5f\n", s,
           (int)gpuFwd[s].size(), (int)gpuNow.size(), diff, maxw);
    /* Same count AND same positions as the forward pass at this cursor. */
    test_assert(gpuFwd[s].size() == gpuNow.size());
    test_assert(diff == 0);
  }

  printf("dyntopo_multistep_gpu: ok\n");
  return retval;
}
