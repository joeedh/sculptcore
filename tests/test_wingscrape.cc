// Wing Scrape must cut, and must leave a convex crease.
//
// It used to be a no-op: both wing planes passed through surfacePos itself, so
// on a flat face no vertex was ever above a wing and nothing moved. The kernel
// now sinks the shared apex line `planeoff * radius` below the surface point,
// exactly as plane.sbrush's Scrape does.
//
// The wings must also tent UP over the stroke line: each leans toward its own
// lateral side, so the cut deepens away from the line and leaves a ridge along
// it. Leaning them the other way gouges a valley — the shape this test pins
// down. A flat scrape cuts to ONE plane, so its depth falls off exactly as
// fast as the brush falloff; both runs share a fixture and are compared as
// rim/ridge ratios, so that falloff cancels out of the comparison.
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

const float3 kCenter{0, 0, 0}; // center of the flat +Z-facing grid
const float3 kNormal{0, 0, 1};
const float3 kStrokeDir{1, 0, 0};
const float kRadius = 0.15f;
const float kPlaneOff = -0.05f; // what wingApexOffset() feeds the tool by default

struct CutProfile {
  float atRidge = 0.0f; // mean cut depth in the band straddling the stroke line
  float atRim = 0.0f;   // mean cut depth in an outer lateral band
  float maxCut = 0.0f;

  float ratio() const
  {
    return atRidge > 0.0f ? atRim / atRidge : 0.0f;
  }
};

// One wing-scrape dab at the given wing half-angle (radians), profiled across
// the stroke. wingAngle == 0 collapses both wings onto surfaceNo, i.e. a plain
// flat scrape — the control.
CutProfile wingDab(float wingAngleRad)
{
  CutProfile out;

  Scene scene(64, 64, /*headless=*/true);
  auto r = script::run(scene,
                       "make_shape kind=grid n=64 m=64 size=1.0\n"
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

  scene.currentTool = sculptcore::brush::SculptBrushes::WINGSCRAPE;
  scene.brush.radius = kRadius;
  scene.brush.strength = 1.0f;
  scene.brush.invert = false;
  scene.brush.planeoff = kPlaneOff;
  scene.brush.planeSide = -1.0f;
  scene.brush.wingAngle = wingAngleRad;
  // Host-owned tangent: the debug script's stroke verb dabs at one origin, so a
  // derived strokeDir would stay degenerate.
  scene.brush.strokeDir = kStrokeDir;
  scene.brush.strokeDirHostSet = true;
  scene.brush.writeProps();

  sculptcore::brush::CommandExecutor exec(scene.tree, &scene.brush);
  exec.meshLog = &scene.meshLog;
  exec.setStrokeGen(1);
  exec.beginStep(false);
  exec.applyDab(scene.currentTool, kCenter, kNormal, kRadius, nullptr, 0);
  exec.endStep();

  int nRidge = 0, nRim = 0;
  for (int i = 0; i < m->v.count; i++) {
    const float cut = start[i][2] - m->v.co[i][2]; // scraping a +Z surface is -Z
    out.maxCut = std::fmax(out.maxCut, cut);

    if (std::fabs(start[i][0]) > 0.3f * kRadius) {
      continue; // stay near the dab center along the stroke
    }
    const float lat =
        std::fabs(start[i][1]); // lat axis = cross(surfaceNo, strokeDir) = +Y
    if (lat <= 0.25f * kRadius) {
      out.atRidge += cut;
      nRidge++;
    } else if (lat >= 0.55f * kRadius && lat <= 0.85f * kRadius) {
      out.atRim += cut;
      nRim++;
    }
  }
  test_assert(nRidge > 0 && nRim > 0);
  if (nRidge > 0) {
    out.atRidge /= float(nRidge);
  }
  if (nRim > 0) {
    out.atRim /= float(nRim);
  }
  return out;
}

} // namespace

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  // Scoped so the profiles' buffers are freed before test_end()'s leak check.
  {
    CutProfile wing = wingDab(25.0f * 3.14159265f / 180.0f);
    CutProfile flat = wingDab(0.0f);

    fprintf(stderr,
            "wing 25deg: maxCut=%.5f ridge=%.5f rim=%.5f rim/ridge=%.3f\n",
            wing.maxCut,
            wing.atRidge,
            wing.atRim,
            wing.ratio());
    fprintf(stderr,
            "flat  0deg: maxCut=%.5f ridge=%.5f rim=%.5f rim/ridge=%.3f\n",
            flat.maxCut,
            flat.atRidge,
            flat.atRim,
            flat.ratio());

    // It cuts at all — the "wing scrape doesn't work" regression.
    test_assert(wing.maxCut > 1e-4f);
    test_assert(wing.atRidge > 1e-4f);
    test_assert(flat.atRidge > 1e-4f);
    // Convex: the wings tent up over the stroke line, so the cut deepens
    // outward — faster than the control's plain falloff decay, and the reverse
    // of what a valley would give.
    test_assert(wing.ratio() > 2.5f * flat.ratio());
  }

  return test_end();
}
