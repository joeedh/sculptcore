// Symmetry grab strength parity (#35 follow-up). A symmetric grab stroke must
// deform the mirror side by the SAME amount as the primary side. The TS dab
// dispatch (runSculptcoreStroke / SculptPaintOp.applyDab) marks each symmetry
// image via setGrabAccumAdd: false on the primary image, true on the mirrors.
//
// The historical bug: the primary image wrote `live = want` (re-base from orig
// each dab) while every mirror image wrote `live += want - base`. Because the
// grab kernel reads the CUMULATIVE drag (`grabTo`) from the stroke-start base,
// the primary re-based (final = orig + drag_N) but the mirror SUMMED every dab
// (final = orig + Σ drag_d), so over an N-dab drag the mirror side ended up
// ~(N+1)/2 times stronger. The fix (AccumOrigGrab) arbitrates per-dab: the first
// image to touch a vert re-bases it, later images of the same dab add — so
// mirror-only verts re-base every dab too (no cross-dab runaway).
//
// This drives a disjoint (non-overlapping) symmetric grab through the executor
// directly and measures the peak pull on each X-half; the two halves must match.
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

struct SideMax {
  float pos = 0.0f; // peak +Y pull on the +X half (the primary image)
  float neg = 0.0f; // peak +Y pull on the -X half (the X-mirror image)
};

// Run an X-symmetric grab stroke of `ndabs` dabs that drags the surface along
// +Y. The primary anchor sits at +x0, the mirror anchor at -x0; x0 > radius so
// the two regions never share a vertex (isolating Absolute vs Add). Each dab's
// grabTo is the cumulative drag (step*d), exactly as the TS bridge sets it.
static SideMax symGrab(int ndabs)
{
  Scene scene(64, 64, /*headless=*/true);
  auto r = script::run(scene,
                       "make_cube subdivs=24 size=0.5\n"
                       "build_spatial leaf_limit=256 depth_limit=8\n",
                       ".");
  test_assert(r.ok);
  if (!r.ok) {
    fprintf(stderr, "  setup line %d: %s\n", r.line_no, r.error.c_str());
    return {};
  }

  Mesh *m = scene.mesh;
  litestl::util::Vector<float3> start;
  start.resize(m->v.count);
  for (int i = 0; i < m->v.count; i++) {
    start[i] = m->v.co[i];
  }

  scene.currentTool = sculptcore::brush::SculptBrushes::GRAB;
  scene.brush.radius = 0.18f;
  scene.brush.strength = 1.0f;
  scene.brush.invert = false;
  scene.brush.writeProps();

  sculptcore::brush::CommandExecutor exec(scene.tree, &scene.brush);
  exec.meshLog = &scene.meshLog;
  exec.setStrokeGen(1);
  exec.beginStep(false);

  const float x0 = 0.30f, z0 = 0.25f;
  const float step = 0.02f;
  const float3 normal{0, 0, 1};

  for (int d = 1; d <= ndabs; d++) {
    const float ydrag = step * float(d); // cumulative drag from the anchor
    const float filterR = scene.brush.radius + ydrag;

    // Primary image (setGrabAccumAdd(false) begins a new logical dab).
    scene.brush.grabFrom = float3{x0, 0, z0};
    scene.brush.grabTo = float3{0, ydrag, 0};
    scene.brush.writeProps();
    exec.setGrabAccumAdd(false);
    exec.applyDab(scene.currentTool, float3{x0, 0, z0}, normal, filterR, nullptr, 0);

    // X-mirror image (setGrabAccumAdd(true)). mul = [-1,1,1]: the anchor reflects
    // to -x0 and a +Y drag stays +Y.
    scene.brush.grabFrom = float3{-x0, 0, z0};
    scene.brush.grabTo = float3{0, ydrag, 0};
    scene.brush.writeProps();
    exec.setGrabAccumAdd(true);
    exec.applyDab(scene.currentTool, float3{-x0, 0, z0}, normal, filterR, nullptr, 0);
  }

  exec.endStep();

  SideMax out;
  for (int i = 0; i < m->v.count; i++) {
    const float dy = m->v.co[i][1] - start[i][1];
    if (start[i][0] > 0.05f) {
      out.pos = std::fmax(out.pos, dy);
    } else if (start[i][0] < -0.05f) {
      out.neg = std::fmax(out.neg, dy);
    }
  }
  return out;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  for (int n : {1, 3, 6}) {
    SideMax s = symGrab(n);
    const float ratio = s.pos > 1e-6f ? s.neg / s.pos : 0.0f;
    fprintf(stderr, "ndabs=%d  +X(primary)=%.5f  -X(mirror)=%.5f  ratio=%.3f (ideal 1.0, bug ~%.2f)\n",
            n, s.pos, s.neg, ratio, (n + 1) * 0.5f);
    test_assert(s.pos > 1e-4f); // the primary actually pulled
    test_assert(s.neg > 1e-4f); // the mirror actually pulled
    // Parity: a symmetric stroke must pull both halves equally (within 15%).
    // Pre-fix this fails for n>1 (mirror ~ (n+1)/2 x stronger).
    test_assert(std::fabs(s.neg - s.pos) < 0.15f * s.pos);
  }

  return test_end();
}
