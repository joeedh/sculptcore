// Leaf-seam gate for `@unbounded` brush fields
// (plans/2026-07-27-unbounded-brush-fields-and-mask-decomposition.md). Kelvinlet
// carries no distance falloff of its own — only `unboundedWindow`'s C1 cutoff at
// R = radius * unboundedExtent — so a node filter sized from brush.radius stops
// while the field is still live. Whole leaves fall out of the set, and two
// vertices the same distance from the dab center end up with wildly different
// displacement depending on which leaf they landed in: the tear.
//
// The metric is exactly that. Force the kelvinlet force vector along the surface
// normal so on the flat +Z face `dot(grabTo, r)` vanishes and the displacement
// magnitude becomes a strictly decreasing function of radial distance alone —
// then any vertex that displaces MORE than a vertex closer to the center is a
// tear, with no tolerance to tune. The test drives applyDab twice, once through
// the engine's filterRadiusFloor and once with the floor defeated, so it also
// proves the metric detects the bug it guards against.
#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"

#include "brush/brush_executor.h"
#include "mesh/mesh.h"

#include "litestl/util/vector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

test_init;

using namespace sculptcore::debug_app;
using namespace sculptcore::mesh;
using namespace litestl::math;
namespace brush = sculptcore::brush;

static constexpr float kRadius = 0.03f;
static constexpr float kExtent = 8.0f;
static constexpr float kR = kRadius * kExtent; // 0.24, inside the 0.25 half-face

struct SeamStats {
  // Largest violation of "displacement decreases with radius", as a fraction of
  // the dab's peak displacement. Zero for an intact field, order-1 for a tear.
  float worstInversion = 0.0f;
  float peakDisp = 0.0f;
  float dispAtRim = 0.0f; // largest displacement in the outer 5% of the field
  int sampleCount = 0;
};

/** One kelvinlet dab at the +Z pole with the force along +Z, measured over the
 * flat face. `defeatFloor` bypasses the engine's `filterRadiusFloor` by
 * filtering + executing by hand at the brush radius — what every C++ host did
 * before this gate existed. */
static SeamStats runDab(bool defeatFloor)
{
  Scene scene(64, 64, /*headless=*/true);
  auto r = script::run(scene,
                       "make_cube subdivs=64 size=0.5\n"
                       "build_spatial leaf_limit=64 depth_limit=10\n",
                       ".");
  test_assert(r.ok);

  Mesh *m = scene.mesh;
  litestl::util::Vector<float3> start;
  start.resize(m->v.count);
  for (int i = 0; i < m->v.count; i++) {
    start[i] = m->v.co[i];
  }

  const float3 center{0.0f, 0.0f, 0.25f};
  const float3 normal{0.0f, 0.0f, 1.0f};

  scene.currentTool = brush::SculptBrushes::KELVINLET;
  scene.brush.radius = kRadius;
  scene.brush.strength = 1.0f;
  scene.brush.unboundedExtent = kExtent;
  scene.brush.grabFrom = center;
  // Force along +Z only: on the flat face r lies in the XY plane, so the
  // kernel's `dot(grabTo, r)` term drops out and |disp| depends only on |r|.
  scene.brush.grabTo = float3{0.0f, 0.0f, 0.02f};
  scene.brush.writeProps();

  brush::CommandExecutor exec(scene.tree, &scene.brush);
  exec.meshLog = &scene.meshLog;
  exec.setStrokeGen(1);
  exec.beginStep(false);
  if (defeatFloor) {
    litestl::util::Vector<sculptcore::spatial::SpatialNode *> nodes;
    scene.tree->filterNodes(center, kRadius, nodes);
    test_assert(nodes.size() > 0);
    exec.execBrush(m, scene.currentTool, &nodes, center, normal);
  } else {
    exec.applyDab(scene.currentTool, center, normal, kRadius, nullptr, 0);
  }
  exec.endStep();

  // Collect (radius, displacement) for the +Z face, measuring radius from the
  // *start* positions — the dab moves verts along Z, which would otherwise
  // perturb their own radial coordinate.
  SeamStats st;
  litestl::util::Vector<float2> samples;
  for (int i = 0; i < m->v.count; i++) {
    if (start[i][2] < 0.2499f) {
      continue; // other cube faces; all far outside R, but keep radius honest
    }
    float d = (start[i] - center).length();
    if (d >= kR) {
      continue;
    }
    float disp = (m->v.co[i] - start[i]).length();
    samples.append(float2{d, disp});
    st.peakDisp = std::fmax(st.peakDisp, disp);
    if (d > 0.95f * kR) {
      st.dispAtRim = std::fmax(st.dispAtRim, disp);
    }
  }
  st.sampleCount = int(samples.size());
  std::sort(samples.begin(), samples.end(), [](const float2 &a, const float2 &b) {
    return a[0] < b[0];
  });

  // Walk outward tracking the smallest displacement seen so far. A vertex that
  // displaces more than one nearer the center inverts the field's monotonicity,
  // which on a smooth radial field can only mean its neighbor was never run.
  float runningMin = 1e30f;
  for (const float2 &s : samples) {
    st.worstInversion = std::fmax(st.worstInversion, s[1] - runningMin);
    runningMin = std::fmin(runningMin, s[1]);
  }
  if (st.peakDisp > 0.0f) {
    st.worstInversion /= st.peakDisp;
  }
  return st;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  SeamStats fixed = runDab(/*defeatFloor=*/false);
  fprintf(stderr,
          "floored:   n=%d peak=%.6f inversion=%.6f rim=%.8f\n",
          fixed.sampleCount,
          fixed.peakDisp,
          fixed.worstInversion,
          fixed.dispAtRim);
  test_assert(fixed.sampleCount > 500); // enough of the face to see a seam
  test_assert(fixed.peakDisp > 1e-4f);  // the dab actually deformed something
  // Monotone in radius to within fp noise: no vertex was skipped.
  test_assert(fixed.worstInversion < 1e-3f);
  // The window really does take the field down before the rim, which is what
  // lets the filtered region end without a step. It reaches exactly 0 only at R
  // itself; the outermost ring of verts sits a little inside that.
  test_assert(fixed.dispAtRim < 0.05f * fixed.peakDisp);

  // Same dab with the floor defeated: the filter stops at kRadius = R/8, so the
  // leaves beyond it never run and their verts stay put while their equidistant
  // neighbors inside an included leaf move the full amount.
  SeamStats torn = runDab(/*defeatFloor=*/true);
  fprintf(stderr,
          "unfloored: n=%d peak=%.6f inversion=%.6f rim=%.8f\n",
          torn.sampleCount,
          torn.peakDisp,
          torn.worstInversion,
          torn.dispAtRim);
  test_assert(torn.worstInversion > 0.2f);

  return test_end();
}
