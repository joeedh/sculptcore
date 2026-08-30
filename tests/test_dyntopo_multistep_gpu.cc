/* Regression: mesh + GPU currency across a MULTI-STEP undo/redo walk.
 *
 * The ts2.wproj field repro is several separate dyntopo strokes (each its own
 * meshlog step); undoing all the way down then redoing back up drifted. Two
 * distinct failure classes are gated here:
 *
 *  1. MESH corruption: undoing a multi-dab dyntopo stroke restored some verts
 *     to MID-step values. Root cause: with one step-wide topo chunk, an
 *     Existed vert first brush-deformed (element store holds its true
 *     pre-step row) and only LATER dyntopo-touched gets a mid-step
 *     begin_body — and the topo chunk, sitting earliest in the chunk list,
 *     wins the reverse-order undo. The per-dab topo-chunk seal in
 *     CommandExecutor::applyDab (re-enabled) restores the per-dab capture
 *     chains the undo order depends on. Gated by sorted live-vert co
 *     multiset comparison at every undo/redo cursor.
 *
 *  2. GPU currency: at every checkpoint the DRAWN buffer positions must be
 *     exactly the live mesh's position set. Leaf PARTITION after undo/redo
 *     legitimately differs from the forward pass (per-leaf vert DUPLICATION
 *     shifts), so the gate compares the sorted UNIQUE position set against
 *     the mesh, checkpoint-local — never forward-vs-redo buffers. The forward
 *     pass also gates the executor's border propagation: a border leaf whose
 *     tris replicate another leaf's moved vert must refresh its slice even
 *     when none of its OWN verts moved (pre-fix this drew phantom corners
 *     holding the replica's pre-final-dab position). */
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
#include <cstring>

test_init;

using namespace sculptcore;
using namespace sculptcore::debug_app;
using litestl::math::float3;

/* The bytes the GPU actually draws: every GPU node's pos buffer after
 * update(), deduplicated (per-leaf replicas collapse; see header comment). */
static void collectGpuUnique(spatial::SpatialTree *tree,
                             litestl::util::Vector<float3> &out)
{
  out.clear();
  for (spatial::SpatialNode *node : tree->gpu_nodes()) {
    if (!node->gpu_data || !node->gpu_data->pos) {
      continue;
    }
    gpu::Buffer *pos = node->gpu_data->pos;
    float3 *d = pos->get_data<float3>();
    /* Clamp to the DRAWN count: buffers keep slack past total_verts after
     * dyntopo churn, and those undrawn slots hold stale positions. */
    int n = std::min(pos->size, node->gpu_data->total_verts);
    for (int i = 0; i < n; i++) {
      out.append(d[i]);
    }
  }
  std::sort(out.begin(), out.end(), [](const float3 &a, const float3 &b) {
    if (a[0] != b[0])
      return a[0] < b[0];
    if (a[1] != b[1])
      return a[1] < b[1];
    return a[2] < b[2];
  });
  auto last = std::unique(out.begin(), out.end(), [](const float3 &a, const float3 &b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
  });
  out.resize(int(last - out.begin()));
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

static int cmpSorted(const litestl::util::Vector<float3> &a,
                     const litestl::util::Vector<float3> &b,
                     float &maxw);

/* Compare the GPU unique-position set against the UNIQUE positions of the
 * live-vert multiset. Returns the mismatch count (size gaps count too). */
static int gpuMatchesMesh(const litestl::util::Vector<float3> &gpuUnique,
                          const litestl::util::Vector<float3> &meshSorted,
                          float &maxw)
{
  litestl::util::Vector<float3> meshUnique;
  for (int i = 0; i < int(meshSorted.size()); i++) {
    if (meshUnique.size() == 0 ||
        std::memcmp(&meshSorted[i], &meshUnique[meshUnique.size() - 1], sizeof(float3)) !=
            0)
    {
      meshUnique.append(meshSorted[i]);
    }
  }
  if (gpuUnique.size() != meshUnique.size()) {
    maxw = -1.0f;
    return int(std::max(gpuUnique.size(), meshUnique.size()) -
               std::min(gpuUnique.size(), meshUnique.size()));
  }
  return cmpSorted(gpuUnique, meshUnique, maxw);
}

static int cmpSorted(const litestl::util::Vector<float3> &a,
                     const litestl::util::Vector<float3> &b,
                     float &maxw)
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

  /* N separate strokes, each its own meshlog step — like the field repro.
   * Snapshot the GPU unique-position set + the live-vert co multiset after
   * each forward step. */
  const int NSTEPS = 4;
  const int NDABS = 5;
  litestl::util::Vector<litestl::util::Vector<float3>> gpuFwd;
  litestl::util::Vector<litestl::util::Vector<float3>> meshFwd;
  gpuFwd.resize(NSTEPS);
  meshFwd.resize(NSTEPS);

  auto collectMeshCo = [&](litestl::util::Vector<float3> &out) {
    out.clear();
    for (int v : m->v) {
      out.append(m->v.co[v]);
    }
    std::sort(out.begin(), out.end(), [](const float3 &a, const float3 &b) {
      if (a[0] != b[0])
        return a[0] < b[0];
      if (a[1] != b[1])
        return a[1] < b[1];
      return a[2] < b[2];
    });
  };

  for (int s = 0; s < NSTEPS; s++) {
    exec.beginStep(true);
    for (int d = 0; d < NDABS; d++) {
      float t = float(d) / float(NDABS - 1);
      /* Sweep a different band of the +Z face each stroke. */
      float bx = -0.18f + 0.12f * float(s);
      float3 origin(bx + 0.1f * t, -0.15f + 0.3f * t, 0.25f);
      exec.applyDab(scene.currentTool,
                    origin,
                    normal,
                    radius,
                    &scene.dyntopoParams,
                    scene.dyntopoSeed + uint32_t(s * 16 + d));
    }
    exec.endDynTopoStroke();
    exec.endStep();
    /* Mirror per-frame tree maintenance between strokes (deferred split/merge). */
    pump(scene);
    collectGpuUnique(scene.tree, gpuFwd[s]);
    collectMeshCo(meshFwd[s]);
    {
      float mw = 0.0f;
      int gd = gpuMatchesMesh(gpuFwd[s], meshFwd[s], mw);
      printf("  step %d: v=%d f=%d gpuUnique=%d gpu-mesh mismatch=%d\n",
             s,
             m->v.count,
             m->f.count,
             (int)gpuFwd[s].size(),
             gd);
      test_assert(gd == 0);
    }
  }

  /* Undo all the way to the base, checking the restored MESH at every cursor
   * (failure class 1), then redo back up checking mesh + GPU. */
  printf("  -- undo descent --\n");
  for (int s = NSTEPS - 1; s >= 0; s--) {
    scene.meshLog.undo(m, scene.tree);
    pump(scene);
    if (s > 0) {
      litestl::util::Vector<float3> meshNow;
      collectMeshCo(meshNow);
      float mw = 0.0f;
      int md = meshFwd[s - 1].size() == meshNow.size()
                   ? cmpSorted(meshFwd[s - 1], meshNow, mw)
                   : -1;
      printf("  undo to cursor %d: meshDiff=%d meshMaxw=%.5f\n", s - 1, md, mw);
      test_assert(md == 0);
    }
  }

  printf("  -- redo ascent --\n");
  for (int s = 0; s < NSTEPS; s++) {
    scene.meshLog.redo(m, scene.tree);
    pump(scene);
    litestl::util::Vector<float3> gpuNow, meshNow;
    collectGpuUnique(scene.tree, gpuNow);
    collectMeshCo(meshNow);
    float gw = 0.0f, meshMaxw = 0.0f;
    int gd = gpuMatchesMesh(gpuNow, meshNow, gw);
    int meshDiff = meshFwd[s].size() == meshNow.size()
                       ? cmpSorted(meshFwd[s], meshNow, meshMaxw)
                       : -1;
    printf("  redo step %d: meshDiff=%d meshMaxw=%.5f | gpu-mesh mismatch=%d\n",
           s,
           meshDiff,
           meshMaxw,
           gd);
    test_assert(meshDiff == 0);
    test_assert(gd == 0);
  }

  printf("dyntopo_multistep_gpu: ok\n");
  return retval;
}
