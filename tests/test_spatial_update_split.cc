/* Regression gates for the update()/updateQueries() split
 * (documentation/plans/2026-07-14-1953-spatial-update-queries-split.md):
 *
 *  1. Query correctness after a queries-only update: a dab followed by
 *     updateQueries() must leave filterNodes/castRay seeing the moved
 *     surface, while the leaves still carry their GPU dirty bits for a
 *     later update(gpu).
 *
 *  2. Deferred-GPU parity: scene A runs update(&gpu) per dab (today's
 *     cadence), scene B runs updateQueries() per dab and one update(&gpu)
 *     at the checkpoint. Both also run a "frame" update(&gpu) at each
 *     checkpoint so the queries-half call count — and with it the
 *     deferred-merge cadence — stays aligned. Gates: identical final mesh,
 *     identical draw-batch command multisets, B's flushed staging buffers
 *     byte-identical to a from-scratch refill of the final mesh, and both
 *     worlds byte-identical after a forced from-scratch refill.
 *
 *     (Exact A-vs-B staging parity is deliberately NOT gated: in the per-dab
 *     world a border leaf's replica of a moved vert refreshes only when that
 *     leaf is next flagged, so A's buffers legitimately hold mid-stroke
 *     values at dab-band borders. B's sticky dirty bits refill those at the
 *     flush with final values — the split world is strictly fresher.)
 *
 *  3. Interleave: B keeps dabbing after the flush; a second flush must
 *     converge again (sticky pendingGpuTopology_ re-arms, no GPU bit was
 *     lost), and no leaf may be left carrying GPU dirt after a flush. */
#include "test_util.h"

#include "brush/brush_executor.h"
#include "debug/scene.h"
#include "debug/script.h"
#include "gpu/vbo.h"
#include "mesh/mesh.h"
#include "spatial/spatial.h"
#include "spatial/spatial_base.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

test_init;

#define TASSERT(expr)                                                                    \
  do {                                                                                   \
    if (!(expr)) {                                                                       \
      retval = 1;                                                                        \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                  \
      fflush(stderr);                                                                    \
    }                                                                                    \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::debug_app;
using litestl::math::float3;

namespace {

constexpr const char *SCENE_SRC = "make_cube subdivs=24 size=0.5\n"
                                  "build_spatial leaf_limit=128 depth_limit=10\n"
                                  "set_brush_tool tool=draw\n"
                                  "set_brush radius=0.18 strength=0.5\n";

bool setupScene(Scene &scene)
{
  auto r = script::run(scene, SCENE_SRC, ".");
  if (!r.ok) {
    fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    return false;
  }
  scene.dyntopoEnabled = true;
  scene.dyntopoParams.l_max = 0.05f;
  scene.dyntopoParams.l_min = 0.02f;
  scene.dyntopoParams.mode = dyntopo::DynTopoMode::Both;
  return true;
}

/* The dab sweep both parity scenes replay: a band across the +Z face,
 * phase-offset so case 3's second batch keeps deforming fresh geometry. */
float3 dabOrigin(int step, int dab, int ndabs)
{
  float t = float(dab) / float(ndabs - 1);
  float bx = -0.18f + 0.12f * float(step);
  return float3(bx + 0.1f * t, -0.15f + 0.3f * t, 0.25f);
}

/* One GPU node's CPU staging bytes over the DRAWN range: pos + nor + every
 * attr stream. Buffer slack past total_verts holds stale data and is never
 * drawn, so it stays out of the blob. */
std::vector<unsigned char> nodeBlob(spatial::SpatialNode *node)
{
  std::vector<unsigned char> blob;
  spatial::GpuData &gd = *node->gpu_data;

  auto appendBuf = [&blob](gpu::Buffer *buf, int slots) {
    if (!buf || !buf->data) {
      return;
    }
    int n = std::min(buf->size, slots);
    size_t bytes = size_t(n) * size_t(buf->elemsize) * sizeof(float);
    const unsigned char *d = static_cast<const unsigned char *>(buf->data);
    blob.insert(blob.end(), d, d + bytes);
  };

  appendBuf(gd.pos, gd.total_verts);
  appendBuf(gd.nor, gd.total_verts);
  for (gpu::Buffer *b : gd.attrBufs) {
    appendBuf(b, gd.total_verts);
  }
  return blob;
}

/* All GPU-node blobs, sorted, so the comparison is independent of nodes[]
 * order (parallel deferred splits make node ids/order nondeterministic while
 * tree structure and per-node content stay deterministic). */
std::vector<std::vector<unsigned char>> collectBlobs(spatial::SpatialTree *tree)
{
  std::vector<std::vector<unsigned char>> blobs;
  for (spatial::SpatialNode *node : tree->gpu_nodes()) {
    if (node->gpu_data && node->gpu_data->pos) {
      blobs.push_back(nodeBlob(node));
    }
  }
  std::sort(blobs.begin(), blobs.end());
  return blobs;
}

/* Draw-batch command multiset as sorted (primCount, end) pairs. */
std::vector<std::pair<int, int>> collectCommands(spatial::SpatialTree *tree)
{
  std::vector<std::pair<int, int>> cmds;
  gpu::DrawBatch *batch = tree->getDrawBatch();
  if (batch) {
    for (gpu::DrawCommand *cmd : batch->commands) {
      cmds.push_back({cmd->primCount, cmd->end});
    }
  }
  std::sort(cmds.begin(), cmds.end());
  return cmds;
}

int countGpuDirtyLeaves(spatial::SpatialTree *tree)
{
  int n = 0;
  for (spatial::SpatialNode *leaf : tree->leaves()) {
    if (leaf->flag & (spatial::Spatial_RegenGPU | spatial::Spatial_UpdateGPU)) {
      n++;
    }
  }
  return n;
}

/* From-scratch refill: reflag every leaf, full update, collect. The reference
 * every correctly-flushed staging state must be byte-identical to. */
std::vector<std::vector<unsigned char>> forcedRegenBlobs(Scene &scene)
{
  for (spatial::SpatialNode *leaf : scene.tree->leaves()) {
    leaf->flag |= spatial::Spatial_RegenGPU;
  }
  scene.tree->update(&scene.gpu);
  return collectBlobs(scene.tree);
}

std::vector<float3> meshCoSorted(Scene &scene)
{
  std::vector<float3> out;
  for (int v : scene.mesh->v) {
    out.push_back(scene.mesh->v.co[v]);
  }
  std::sort(out.begin(), out.end(), [](const float3 &a, const float3 &b) {
    return std::memcmp(&a, &b, sizeof(float3)) < 0;
  });
  return out;
}

} // namespace

/* Case 1: after a dab + updateQueries(), spatial queries see the moved
 * surface and the GPU dirty bits survive untouched. */
static void testQueriesOnly()
{
  Scene scene(256, 256, /*headless=*/true);
  TASSERT(setupScene(scene));

  /* Establish buffers + partition, and consume the build-time dirt. */
  scene.tree->update(&scene.gpu);
  TASSERT(countGpuDirtyLeaves(scene.tree) == 0);

  brush::CommandExecutor exec(scene.tree, &scene.brush);
  exec.meshLog = &scene.meshLog;
  exec.ctx.renderMatrix = scene.renderMatrix;

  const float3 center(0.0f, 0.0f, 0.25f);
  const float3 normal(0, 0, 1);
  const float radius = scene.brush.radius;

  exec.beginStep(true);
  exec.applyDab(scene.currentTool, center, normal, radius, &scene.dyntopoParams,
                scene.dyntopoSeed);
  exec.endDynTopoStroke();
  exec.endStep();

  scene.tree->updateQueries();

  /* filterNodes finds the leaves under the dab. */
  litestl::util::Vector<spatial::SpatialNode *> hitNodes;
  TASSERT(scene.tree->filterNodes(center, radius, hitNodes));
  TASSERT(hitNodes.size() > 0);

  /* castRay hits the displaced surface: the draw brush pushed the +Z face
   * outward, so the hit lands above the rest cube surface (z=0.25). */
  spatial::CastRayIsect isect;
  TASSERT(scene.tree->castRay(float3(0, 0, 2), float3(0, 0, -1), isect));
  TASSERT(isect.p[2] > 0.25f + 1e-4f);

  /* The queries half must not have consumed any GPU dirty bit. */
  TASSERT(countGpuDirtyLeaves(scene.tree) > 0);

  /* And a full update consumes them all. */
  scene.tree->update(&scene.gpu);
  TASSERT(countGpuDirtyLeaves(scene.tree) == 0);
}

/* Cases 2 + 3: deferred-GPU parity and interleave/reconvergence. */
static void testDeferredParity()
{
  Scene sceneA(256, 256, /*headless=*/true);
  Scene sceneB(256, 256, /*headless=*/true);
  TASSERT(setupScene(sceneA));
  TASSERT(setupScene(sceneB));

  sceneA.tree->update(&sceneA.gpu);
  sceneB.tree->update(&sceneB.gpu);

  brush::CommandExecutor execA(sceneA.tree, &sceneA.brush);
  execA.meshLog = &sceneA.meshLog;
  execA.ctx.renderMatrix = sceneA.renderMatrix;

  brush::CommandExecutor execB(sceneB.tree, &sceneB.brush);
  execB.meshLog = &sceneB.meshLog;
  execB.ctx.renderMatrix = sceneB.renderMatrix;

  const float3 normal(0, 0, 1);
  const float radiusA = sceneA.brush.radius;
  const float radiusB = sceneB.brush.radius;
  const int NDABS = 6;

  auto runStep = [&](int step) {
    execA.beginStep(true);
    execB.beginStep(true);
    for (int d = 0; d < NDABS; d++) {
      float3 origin = dabOrigin(step, d, NDABS);
      uint32_t seed = sceneA.dyntopoSeed + uint32_t(step * 16 + d);

      execA.applyDab(sceneA.currentTool, origin, normal, radiusA,
                     &sceneA.dyntopoParams, seed);
      sceneA.tree->update(&sceneA.gpu); /* today's per-dab cadence */

      execB.applyDab(sceneB.currentTool, origin, normal, radiusB,
                     &sceneB.dyntopoParams, seed);
      sceneB.tree->updateQueries(); /* split-world per-dab cadence */
    }
    execA.endDynTopoStroke();
    execA.endStep();
    execB.endDynTopoStroke();
    execB.endStep();

    /* The draw frame: both worlds run a full update, so the queries-half
     * call count (deferred-merge cadence) stays aligned; B's is also the
     * deferred GPU flush under test. */
    sceneA.tree->update(&sceneA.gpu);
    sceneB.tree->update(&sceneB.gpu);
  };

  /* Case 2: one stroke, flush. Structural gates only here — the byte gates
   * run after case 3 so the interleaved state is never perturbed early. */
  runStep(0);
  TASSERT(collectCommands(sceneA.tree).size() > 0);
  TASSERT(collectCommands(sceneA.tree) == collectCommands(sceneB.tree));
  TASSERT(countGpuDirtyLeaves(sceneA.tree) == 0);
  TASSERT(countGpuDirtyLeaves(sceneB.tree) == 0);

  /* Case 3: keep dabbing after the flush; the second flush must converge
   * again (pendingGpuTopology_ re-armed, no GPU bit lost at the first
   * flush). */
  runStep(1);
  TASSERT(collectCommands(sceneA.tree) == collectCommands(sceneB.tree));
  TASSERT(countGpuDirtyLeaves(sceneA.tree) == 0);
  TASSERT(countGpuDirtyLeaves(sceneB.tree) == 0);

  /* Both worlds deformed the mesh identically. */
  {
    auto coA = meshCoSorted(sceneA);
    auto coB = meshCoSorted(sceneB);
    TASSERT(coA.size() == coB.size());
    TASSERT(std::memcmp(coA.data(), coB.data(), coA.size() * sizeof(float3)) == 0);
  }

  /* Byte gates: B's flushed staging state must already be exactly what a
   * from-scratch refill of the final mesh produces (sticky dirty bits leave
   * no stale slice behind), and both worlds must agree after a from-scratch
   * refill. A's own flushed state is NOT gated against B — per-dab fills
   * legitimately capture mid-stroke values at dab-band borders. */
  auto blobsB = collectBlobs(sceneB.tree);
  TASSERT(blobsB.size() > 0);
  auto freshB = forcedRegenBlobs(sceneB);
  TASSERT(blobsB == freshB);
  auto freshA = forcedRegenBlobs(sceneA);
  TASSERT(freshA == freshB);
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  testQueriesOnly();
  testDeferredParity();

  return retval;
}
