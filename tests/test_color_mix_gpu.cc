// SW0 gate (claudeMemory/plans/grids-native-completion.md): color.sbrush's
// `mixMode` uniform must reach the GPU. packBrushUniforms never wrote it, and
// the uniform buffer's zero-init rounds up past offset 96, so every GPU colour
// stroke silently ran mode 0 (MIX). The gate: on a seeded colour layer, a
// MULTIPLY stroke differs from a MIX stroke on the CPU path, and the GPU
// stroke matches the CPU MULTIPLY result (not the MIX one).
// Runs on the WGSL/Vulkan dispatcher — the only backend with attr-layer
// (setAttr) support. Skips (passes) without a GPU device or the backend.
#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"

#include "mesh/attribute.h"
#include "mesh/mesh.h"

#include <cmath>
#include <cstdio>
#include <cstring>

test_init;

using namespace sculptcore;
using namespace sculptcore::debug_app;
using namespace sculptcore::mesh;
using litestl::math::float4;
using litestl::util::Vector;

static const float4 kBase(0.6f, 0.3f, 0.2f, 1.0f);

static void seedColors(AttrData<float4> *col, Mesh *m)
{
  for (int i = 0; i < m->v.count; i++) {
    (*col)[i] = kBase;
  }
}

static void snapshot(AttrData<float4> *col, Mesh *m, Vector<float4> &out)
{
  out.resize(m->v.count);
  for (int i = 0; i < m->v.count; i++) {
    out[i] = (*col)[i];
  }
}

static float maxDiff(const Vector<float4> &a, const Vector<float4> &b)
{
  float worst = 0.0f;
  for (int i = 0; i < int(a.size()); i++) {
    for (int c = 0; c < 4; c++) {
      float d = std::fabs(a[i][c] - b[i][c]);
      if (d > worst) worst = d;
    }
  }
  return worst;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Scene scene(128, 128, /*headless=*/true);
  auto r = script::run(scene,
                       "make_cube subdivs=12 size=0.5\n"
                       "build_spatial leaf_limit=256 depth_limit=8\n"
                       "set_brush_tool tool=color\n"
                       "set_brush radius=0.25 strength=1.0\n"
                       "set_backend backend=cpp\n",
                       ".");
  if (!r.ok) {
    fprintf(stderr, "  setup line %d: %s\n", r.line_no, r.error.c_str());
  }
  test_assert(r.ok);

  Mesh *m = scene.mesh;
  AttrRef &cref = m->v.attrs.ensure(AttrType::FLOAT4, "color", /*materialize=*/true);
  AttrData<float4> *col = cref.get_data<float4>();
  test_assert(col != nullptr);
  seedColors(col, m);
  Vector<float4> base;
  snapshot(col, m, base);

  // Grey at half strength keeps every mix mode inside [0,1] on the seed color,
  // and MULTIPLY (0.3,0.15,0.2) lands far from MIX's toward-0.5 pull.
  scene.brush.brushColor = float4(0.5f, 0.5f, 0.5f, 1.0f);
  scene.brush.writeProps();

  const char *strokeCmd = "stroke origin=0,0,0.25 normal=0,0,1\n";

  // CPU reference, MULTIPLY (mode 1).
  scene.brush.mixMode = 1;
  scene.brush.writeProps();
  r = script::run(scene, strokeCmd, ".");
  test_assert(r.ok);
  Vector<float4> cpuMul;
  snapshot(col, m, cpuMul);
  test_assert(maxDiff(cpuMul, base) > 0.01f);
  scene.meshLog.undo(m, scene.tree);
  Vector<float4> restored;
  snapshot(col, m, restored);
  test_assert(maxDiff(restored, base) == 0.0f);

  // CPU reference, MIX (mode 0) — must differ from MULTIPLY.
  scene.brush.mixMode = 0;
  scene.brush.writeProps();
  r = script::run(scene, strokeCmd, ".");
  test_assert(r.ok);
  Vector<float4> cpuMix;
  snapshot(col, m, cpuMix);
  scene.meshLog.undo(m, scene.tree);
  float modeGap = maxDiff(cpuMul, cpuMix);
  fprintf(stderr, "cpu mode gap (MULTIPLY vs MIX): %g\n", modeGap);
  test_assert(modeGap > 0.01f);

  // GPU backend probe — skip cleanly without a device or the WGSL backend.
  if (!scene.ensureGPU()) {
    fprintf(stderr, "no GPU; color mix gpu gate skipped\n");
    return 0;
  }
  r = script::run(scene, "set_backend backend=wgsl\n", ".");
  if (!r.ok) {
    if (std::strstr(r.error.c_str(), "not compiled in")) {
      fprintf(stderr, "color mix gpu gate skipped (%s)\n", r.error.c_str());
      return 0;
    }
    fprintf(stderr, "  backend: %s\n", r.error.c_str());
    test_assert(r.ok);
    return 1;
  }

  // GPU stroke, MULTIPLY: must match the CPU MULTIPLY result, not the MIX one.
  scene.brush.mixMode = 1;
  scene.brush.writeProps();
  r = script::run(scene, strokeCmd, ".");
  if (!r.ok) {
    fprintf(stderr, "  gpu stroke line %d: %s\n", r.line_no, r.error.c_str());
  }
  test_assert(r.ok);
  Vector<float4> gpuMul;
  snapshot(col, m, gpuMul);
  float gpuErr = maxDiff(gpuMul, cpuMul);
  float gpuVsMix = maxDiff(gpuMul, cpuMix);
  fprintf(stderr, "gpu vs cpu(MULTIPLY): %g   gpu vs cpu(MIX): %g\n", gpuErr,
          gpuVsMix);
  test_assert(gpuErr < 2e-5f);
  test_assert(gpuVsMix > 0.01f);

  fprintf(stderr, "color mix gpu gate passed\n");
  // Skip test_end(): attr name strings stay live in the alloc tracker
  // (mirrors the other debug_core tests).
  return retval;
}
