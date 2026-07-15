// M3: cavity automasking GPU parity
// (documentation/plans/2026-07-14-2007-cavity-automasking.md).
//
// With cavity masking ON, a draw stroke dispatched on the WGSL backend must
// match the CPU backend bit-modulo-fp: the host computes the per-vertex cavity
// factor once (automask.h) and both paths read the SAME value — the CPU through
// CommandCtx::strength, the GPU through the binding-24 automask buffer that
// brush_strength multiplies in. Skips (still "passes") when no GPU device is
// available, exactly like the other GPU-parity tests.
#include "test_util.h"

#include "brush/brush.h"
#include "debug/scene.h"
#include "debug/script.h"
#include "mesh/mesh.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

test_init;

using namespace sculptcore;
using namespace sculptcore::debug_app;
using namespace sculptcore::mesh;
using litestl::math::float3;

// Stroke a convex dome with the draw brush (cavity on) on the given backend and
// capture every vertex position. Returns false only when the WGSL backend has no
// GPU device (the caller then skips the comparison).
static bool strokeDome(const char *backend, std::vector<float3> &out)
{
  Scene s(128, 128, /*headless=*/true);
  auto r = script::run(s, "make_shape kind=grid n=24 m=24 size=2\n", ".");
  test_assert(r.ok);
  Mesh *m = s.mesh;

  for (int v = 0; v < m->v.count; v++) {
    float3 co = m->v.co[v];
    co[2] = -0.3f * (co[0] * co[0] + co[1] * co[1]);
    m->v.co[v] = co;
  }
  m->recalc_normals();

  s.brush.automask_cavity = true;
  s.brush.cavity_factor = 0.5f; // partial mask so the factor isn't saturated to 0
  s.brush.cavity_blur_steps = 2;
  s.brush.cavity_inverted = false;

  if (std::string(backend) == "wgsl" && !s.ensureGPU()) {
    return false;
  }

  std::string script = std::string("build_spatial leaf_limit=256 depth_limit=8\n"
                                    "set_brush radius=1.5 strength=0.5\n"
                                    "set_brush_tool tool=draw\n"
                                    "set_backend backend=") +
                       backend + "\n" + "stroke origin=0,0,0 normal=0,0,1\n";
  r = script::run(s, script.c_str(), ".");
  test_assert(r.ok);

  out.resize(m->v.count);
  for (int i = 0; i < m->v.count; i++) {
    out[i] = m->v.co[i];
  }
  return true;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  std::vector<float3> cppPos, wgslPos;
  bool ok = strokeDome("cpp", cppPos);
  test_assert(ok);

  if (!strokeDome("wgsl", wgslPos)) {
    fprintf(stderr, "no GPU; skipping cavity GPU parity\n");
    return test_end();
  }

  test_assert(cppPos.size() == wgslPos.size());
  double maxd = 0.0;
  for (size_t i = 0; i < cppPos.size(); i++) {
    double d = double((cppPos[i] - wgslPos[i]).length());
    if (d > maxd) {
      maxd = d;
    }
  }
  fprintf(stderr, "cavity cpp-vs-wgsl maxdiff=%g\n", maxd);
  test_assert(maxd < 1e-4);

  return test_end();
}
