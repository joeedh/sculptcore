// Enhance-details brush (difference-of-smooths band-pass, enhance.h). Two checks:
//   A) On a smooth mid-scale bump, an enhance stroke amplifies it — the apex
//      moves further out along the normal (band-pass detail points outward).
//   B) The inner ring (band-pass vs high-pass) is load-bearing: on high-frequency
//      per-vertex noise, inner=0 (classic unsharp / high-pass) amplifies the
//      noise MORE than inner=1 (difference-of-smooths band-pass), which averages
//      the noise away before differencing.
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

static int centerVert(Mesh *m)
{
  int best = 0;
  float bestR = 1e30f;
  for (int v = 0; v < m->v.count; v++) {
    float3 co = m->v.co[v];
    float r = co[0] * co[0] + co[1] * co[1];
    if (r < bestR) {
      bestR = r;
      best = v;
    }
  }
  return best;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  const char *SETUP = "build_spatial leaf_limit=256 depth_limit=8\n"
                      "set_brush radius=1.5 strength=1.0\n"
                      "set_brush_tool tool=enhance\n"
                      "set_backend backend=cpp\n";

  // --- A) amplify a smooth mid-scale bump -------------------------------
  {
    Scene s(128, 128, /*headless=*/true);
    auto r = script::run(s, "make_shape kind=grid n=32 m=32 size=4\n", ".");
    test_assert(r.ok);
    Mesh *m = s.mesh;
    for (int v = 0; v < m->v.count; v++) {
      float3 co = m->v.co[v];
      float r2 = co[0] * co[0] + co[1] * co[1];
      co[2] = 0.3f * std::exp(-r2 / (2.0f * 0.5f * 0.5f)); // bump, sigma=0.5
      m->v.co[v] = co;
    }
    m->recalc_normals();

    int apex = centerVert(m);
    float zBefore = m->v.co[apex][2];

    s.brush.enhance_rings = 4;
    s.brush.enhance_inner = 1;
    r = script::run(
        s, (std::string(SETUP) + "stroke origin=0,0,0.3 normal=0,0,1\n").c_str(), ".");
    test_assert(r.ok);

    float zAfter = m->v.co[apex][2];
    fprintf(stderr, "bump apex z: before=%g after=%g\n", zBefore, zAfter);
    test_assert(zAfter > zBefore + 1e-4f); // bump amplified outward
  }

  // --- B) band-pass rejects high-frequency noise more than high-pass ----
  auto noiseStrokeTotal = [&](int inner) -> double {
    Scene s(128, 128, /*headless=*/true);
    auto r = script::run(s, "make_shape kind=grid n=32 m=32 size=4\n", ".");
    test_assert(r.ok);
    Mesh *m = s.mesh;
    std::vector<float3> before(m->v.count);
    for (int v = 0; v < m->v.count; v++) {
      float3 co = m->v.co[v];
      co[2] = ((v & 1) ? 0.02f : -0.02f); // top-octave per-vertex noise
      m->v.co[v] = co;
      before[v] = co;
    }
    m->recalc_normals();

    s.brush.enhance_rings = 4;
    s.brush.enhance_inner = inner;
    r = script::run(
        s, (std::string(SETUP) + "stroke origin=0,0,0 normal=0,0,1\n").c_str(), ".");
    test_assert(r.ok);

    double total = 0.0;
    for (int v = 0; v < m->v.count; v++) {
      total += double((m->v.co[v] - before[v]).length());
    }
    return total;
  };

  double highpass = noiseStrokeTotal(0); // classic unsharp — keeps noise
  double bandpass = noiseStrokeTotal(1); // difference-of-smooths — rejects noise
  fprintf(stderr,
          "noise displacement: highpass(inner=0)=%g bandpass(inner=1)=%g\n",
          highpass,
          bandpass);
  test_assert(highpass > bandpass * 1.1); // band-pass amplifies noise less

  return test_end();
}
