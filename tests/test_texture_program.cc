#include "test_util.h"

#include "brush/texture_eval.h"
#include "brush/texture_jit.h"
#include "brush/texture_program.h"
#include "brush/texture_registry.h"

#include <cmath>
#include <cstdio>
#include <cstring>

test_init;

using sculptcore::brush::compileTextureScript;
using sculptcore::brush::freeTextureProgram;
using sculptcore::brush::TextureProgram;

/** compileTextureScript (texture-scripts T3.3): the runtime parse -> emit-C ->
 * tinycc pipeline behind TextureProgram, gated by test_texture_c_jit's
 * lower-level emit/JIT coverage. Checks the program surface — entries, param
 * manifest, defaults slab, WGSL module, error paths — and value parity
 * against the precompiled registry Rings. */

static const char *kRingsSrc = R"(
texture Rings {
  float eval(float3 p, float3 n) {
    float d = length(p);
    float rings = 0.5 + 0.5 * sin(d * 40.0);
    float bands = fract(d * 6.0);
    float steps = floor(bands * 4.0) * 0.25;
    float tilt = 0.5 + 0.5 * cos(dot(n, p) * 8.0);
    return rings * steps * tilt;
  }
}
)";

static const char *kScaledSrc = R"(
texture Scaled {
  param float scale = 2.0 @range(0.5, 8.0);
  param ramp shape;

  float eval(float3 p, float3 n) {
    float3 q = mapPoint(p);
    return shape.sample(fract(length(q) * scale));
  }
}
)";

static void testRings()
{
  litestl::util::string error;
  TextureProgram *p = compileTextureScript(kRingsSrc, "rings.stex", error);
  if (!p) {
    fprintf(stderr, "compile error: %s\n", error.c_str());
  }
  test_assert(p != nullptr);
  if (!p) {
    return;
  }

  test_assert(std::strcmp(p->name.c_str(), "Rings") == 0);
  test_assert(p->eval != nullptr);
  test_assert(p->evalDual != nullptr);
  test_assert(p->paramSlabSize == 0);
  test_assert(p->params.size() == 0);
  test_assert(p->samplerDeps.size() == 0);
  test_assert(!p->usesMap);
  test_assert(std::strstr(p->wgsl.c_str(), "tex_rings_eval") != nullptr);
  test_assert(p->gpuAvailable);

  const sculptcore::brush::TextureRegistryEntry *ref = sculptcore::brush::findTexture("Rings");
  test_assert(ref != nullptr);
  if (ref && p->eval) {
    const float P[3] = {0.31f, -0.14f, 0.52f};
    const float N[3] = {0.0f, 0.0f, 1.0f};
    float jit = p->eval(P, N, nullptr, nullptr);
    float refv = ref->eval(litestl::math::float3{P[0], P[1], P[2]},
                           litestl::math::float3{N[0], N[1], N[2]},
                           ref->defaults,
                           nullptr);
    test_assert(std::abs(jit - refv) <= 1e-6f);
  }

  freeTextureProgram(p);
}

static void testScaled()
{
  litestl::util::string error;
  TextureProgram *p = compileTextureScript(kScaledSrc, "scaled.stex", error);
  if (!p) {
    fprintf(stderr, "compile error: %s\n", error.c_str());
  }
  test_assert(p != nullptr);
  if (!p) {
    return;
  }

  test_assert(p->paramSlabSize == 1 + sculptcore::brush::kTexRampSize);
  test_assert(p->defaults.size() == p->paramSlabSize);
  test_assert(p->params.size() == 2);
  if (p->params.size() == 2) {
    test_assert(std::strcmp(p->params[0].name.c_str(), "scale") == 0);
    test_assert(!p->params[0].isRamp && !p->params[0].isConst);
    test_assert(p->params[0].offset == 0);
    test_assert(p->params[0].hasRange && p->params[0].rangeMin == 0.5f &&
                p->params[0].rangeMax == 8.0f);
    test_assert(std::strcmp(p->params[1].name.c_str(), "shape") == 0);
    test_assert(p->params[1].isRamp);
    test_assert(p->params[1].offset == 1);
  }
  if (p->defaults.size() == p->paramSlabSize) {
    test_assert(p->defaults[0] == 2.0f);
    test_assert(p->defaults[1] == 0.0f);
    test_assert(p->defaults[256] == 1.0f);
  }
  test_assert(p->evalDual == nullptr);  // ramp.sample has no derivative rule
  test_assert(p->usesMap);

  if (p->eval) {
    sculptcore::brush::TexEvalCtx ctx = {{
        1.5f, 0.0f, 0.0f, 0.2f,   //
        0.0f, 0.8f, 0.0f, -0.1f,  //
        0.0f, 0.0f, 1.1f, 0.05f,  //
        0.0f, 0.0f, 0.0f, 1.0f,   //
    }};
    const float P[3] = {0.31f, -0.14f, 0.52f};
    const float N[3] = {0.0f, 0.0f, 1.0f};
    float jit = p->eval(P, N, p->defaults.data(), &ctx);

    litestl::math::float3 q = sculptcore::brush::texMapPoint(
        &ctx, litestl::math::float3{P[0], P[1], P[2]});
    float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    float t = len * p->defaults[0];
    t = t - std::floor(t);
    float expect = sculptcore::brush::texRampSample(p->defaults.data() + 1, t);
    test_assert(std::abs(jit - expect) <= 1e-6f);
  }

  freeTextureProgram(p);
}

static void testErrors()
{
  litestl::util::string error;

  // A brush source is not a texture unit.
  TextureProgram *p = compileTextureScript(
      "@brush(\"x\")\nbrush X { vertex void apply(inout Vertex v) { } }", "x.sbrush", error);
  test_assert(p == nullptr);
  test_assert(std::strstr(error.c_str(), "not a texture unit") != nullptr);

  // Sampler calls are runtime-only until T4.
  p = compileTextureScript(
      "sampler float noisefn(float3 p);\n"
      "texture Noisy { float eval(float3 p, float3 n) { return noisefn(p); } }",
      "noisy.stex",
      error);
  test_assert(p == nullptr);
  test_assert(std::strstr(error.c_str(), "runtime-only (T4)") != nullptr);

  // Exactly one texture per runtime script.
  p = compileTextureScript(
      "texture A { float eval(float3 p, float3 n) { return 1.0; } }\n"
      "texture B { float eval(float3 p, float3 n) { return 2.0; } }",
      "two.stex",
      error);
  test_assert(p == nullptr);
  test_assert(std::strstr(error.c_str(), "exactly one texture") != nullptr);

  // A syntax error reports rather than crashing.
  p = compileTextureScript("texture Broken { float eval(", "broken.stex", error);
  test_assert(p == nullptr);
  test_assert(error.size() > 0);
}

static void runTests()
{
  test_assert(sculptcore::brush::textureScriptCpuAvailable());
  testRings();
  testScaled();
  testErrors();
}

int main()
{
  runTests();
  return test_end();
}
