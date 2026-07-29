// From-orig grab strokes must not get more expensive as the drag grows.
//
// A grab-class kernel re-bases every vert from its stroke-start position, so the
// verts it can ever move are exactly those inside the falloff radius of the
// FIXED anchor — the set is decided by dab 1 and never grows. The host still
// widens the radius it passes applyDab by the cumulative drag (the GPU dab and
// the anchored preview snapshot need that), and the CPU executor used to follow
// it: every dab then re-stamped, re-ran, re-normalled and re-uploaded the whole
// swept region, so per-dab cost grew linearly with the drag for no extra
// coverage. CommandExecutor::grabFilterNodes pins the region instead.
//
// Guards both halves: the leaf count handed to the deform program stays flat
// (cost), and a 24-dab growing drag lands exactly where a single dab of the full
// drag does (correctness — pinning must not drop a leaf that should have moved).
#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"

#include "brush/brush_executor.h"
#include "mesh/mesh.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>

test_init;

using namespace sculptcore::debug_app;
using namespace sculptcore::mesh;
using namespace litestl::math;

namespace {

struct GrabRun {
  litestl::util::Vector<float3> co;     // final vert positions
  litestl::util::Vector<int> nodeCount; // leaves per dab
  float maxPull = 0.0f;                 // peak displacement from the start mesh
  int naiveLast = 0;                    // leaves the unpinned (drag-widened) filter would take
};

const float3 kAnchor{0.0f, 0.0f, 0.25f};
const float3 kNormal{0, 0, 1};
const float kTotalDrag = 0.60f; // >> radius, so the naive filter widens ~6x

// Drag the anchored region along +Y in `ndabs` even steps, exactly as the TS
// bridge drives it: cumulative grabTo, dab centered on the anchor, node-filter
// radius widened by the drag.
GrabRun grabStroke(int ndabs)
{
  GrabRun out;

  Scene scene(64, 64, /*headless=*/true);
  auto r = script::run(scene,
                       "make_cube subdivs=32 size=0.5\n"
                       "build_spatial leaf_limit=256 depth_limit=8\n",
                       ".");
  test_assert(r.ok);
  if (!r.ok) {
    fprintf(stderr, "  setup line %d: %s\n", r.line_no, r.error.c_str());
    return out;
  }

  Mesh *m = scene.mesh;
  litestl::util::Vector<float3> start;
  start.resize(m->v.count);
  for (int i = 0; i < m->v.count; i++) {
    start[i] = m->v.co[i];
  }

  scene.currentTool = sculptcore::brush::SculptBrushes::GRAB;
  scene.brush.radius = 0.10f;
  scene.brush.strength = 1.0f;
  scene.brush.invert = false;
  scene.brush.writeProps();

  sculptcore::brush::CommandExecutor exec(scene.tree, &scene.brush);
  exec.meshLog = &scene.meshLog;
  exec.setStrokeGen(1);
  exec.beginStep(false);

  for (int d = 1; d <= ndabs; d++) {
    const float ydrag = kTotalDrag * float(d) / float(ndabs);
    scene.brush.grabFrom = kAnchor;
    scene.brush.grabTo = float3{0, ydrag, 0};
    scene.brush.writeProps();
    exec.setGrabAccumAdd(false);
    exec.applyDab(scene.currentTool, kAnchor, kNormal, scene.brush.radius + ydrag, nullptr, 0);
    out.nodeCount.append(exec.lastDabNodeCount);
  }

  exec.endStep();

  // What the old (drag-following) filter would have handed the last dab, so the
  // assertion below is measuring a real gap rather than a tautology.
  {
    litestl::util::Vector<sculptcore::spatial::SpatialNode *> naive;
    scene.tree->filterNodes(kAnchor, scene.brush.radius + kTotalDrag, naive);
    out.naiveLast = int(naive.size());
  }

  out.co.resize(m->v.count);
  for (int i = 0; i < m->v.count; i++) {
    out.co[i] = m->v.co[i];
    out.maxPull = std::fmax(out.maxPull, (m->v.co[i] - start[i]).length());
  }
  return out;
}

} // namespace

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  // Scoped so the runs' buffers are freed before test_end()'s leak check.
  {
    GrabRun one = grabStroke(1);
    GrabRun many = grabStroke(24);
    test_assert(one.co.size() > 0 && many.co.size() == one.co.size());

    // Cost: the pinned region is decided by dab 1 and never grows.
    const int first = many.nodeCount[0];
    int peak = 0;
    for (int n : many.nodeCount) {
      peak = peak > n ? peak : n;
    }
    fprintf(stderr, "grab leaves: dab1=%d peak=%d last=%d (unpinned would be %d; drag %.2f vs radius 0.10)\n",
            first, peak, many.nodeCount[int(many.nodeCount.size()) - 1], many.naiveLast, kTotalDrag);
    test_assert(first > 0);
    test_assert(peak == first);
    test_assert(many.naiveLast > 2 * first);

    // Correctness: 24 growing dabs == 1 dab of the whole drag, and the stroke
    // actually moved something.
    float maxDelta = 0.0f;
    for (int i = 0; i < int(one.co.size()); i++) {
      maxDelta = std::fmax(maxDelta, (many.co[i] - one.co[i]).length());
    }
    fprintf(stderr, "grab agreement: max|24dab - 1dab| = %.6f, max pull = %.4f / %.4f\n", maxDelta,
            one.maxPull, many.maxPull);
    test_assert(one.maxPull > 0.5f * kTotalDrag);
    test_assert(maxDelta < 1e-4f);
  }

  return test_end();
}
