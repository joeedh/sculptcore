#include "test_util.h"

#include "brush/host_sampler.h"
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

  // Sampler deps must be registered before compile (T4).
  p = compileTextureScript(
      "sampler float noisefn(float3 p);\n"
      "texture Noisy { float eval(float3 p, float3 n) { return noisefn(p); } }",
      "noisy.stex",
      error);
  test_assert(p == nullptr);
  test_assert(std::strstr(error.c_str(), "not registered") != nullptr);

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

/** Host samplers (texture-scripts T4). The field is quadratic so central
 * differences are exact up to rounding — the FD-synthesized gradient and the
 * analytic one must agree to float noise. The inner point is built with
 * scalar ops (float3 ctor), since dual bodies have no float3 arithmetic. */
static const char *kFieldSrc = R"(
sampler float field(float3 p);

texture Fielded {
  float eval(float3 p, float3 n) {
    return field(float3(p.x * 2.0, p.y, p.z)) * 0.5;
  }
}
)";

static const char *kTwoArgSrc = R"(
sampler float nfield(float3 p, float3 n);

texture TwoArg {
  float eval(float3 p, float3 n) {
    return nfield(p, n);
  }
}
)";

static float hsField(void *user, const float p[3], const float n[3])
{
  (void)n;
  float bias = user ? *(const float *)user : 0.0f;
  return p[0] * p[0] + p[1] * p[1] + p[2] * p[2] + bias;
}

static void hsFieldGrad(void *user, const float p[3], const float n[3], float out[4])
{
  out[0] = hsField(user, p, n);
  out[1] = 2.0f * p[0];
  out[2] = 2.0f * p[1];
  out[3] = 2.0f * p[2];
}

static float hsFieldPlusOne(void *user, const float p[3], const float n[3])
{
  return hsField(user, p, n) + 1.0f;
}

static float hsDotN(void *user, const float p[3], const float n[3])
{
  (void)user;
  return p[0] * n[0] + p[1] * n[1] + p[2] * n[2];
}

static const char *kFieldWgsl = "fn hs_field(p: vec3f, n: vec3f) -> f32 { return dot(p, p); }\n";
static const char *kFieldWgslWithGrad =
    "fn hs_field(p: vec3f, n: vec3f) -> f32 { return dot(p, p); }\n"
    "fn hs_field_grad(p: vec3f, n: vec3f) -> vec4f { return vec4f(dot(p, p), 2.0 * p); }\n";

static void testSamplers()
{
  using sculptcore::brush::findHostSampler;
  using sculptcore::brush::HostSampler;
  using sculptcore::brush::registerHostSampler;
  using sculptcore::brush::unregisterHostSampler;

  HostSampler bad;
  test_assert(!registerHostSampler(bad));  // name required
  bad.name = "field";
  test_assert(!registerHostSampler(bad));  // fn required

  HostSampler hs;
  hs.name = "field";
  hs.fn = hsField;
  test_assert(registerHostSampler(hs));
  const HostSampler *entry = findHostSampler("field");
  test_assert(entry != nullptr && entry->fn == hsField);
  test_assert(entry && entry->fd_step == 1e-3f);

  // Wrong arity against the declared signature is an emit error (the dep
  // itself resolves — field is registered).
  litestl::util::string error;
  TextureProgram *p = compileTextureScript(
      "sampler float field(float3 p);\n"
      "texture Bad { float eval(float3 p, float3 n) { return field(p, n); } }",
      "bad.stex",
      error);
  test_assert(p == nullptr);
  test_assert(std::strstr(error.c_str(), "wrong number of arguments") != nullptr);

  p = compileTextureScript(kFieldSrc, "field.stex", error);
  if (!p) {
    fprintf(stderr, "compile error: %s\n", error.c_str());
  }
  test_assert(p != nullptr);
  if (!p) {
    return;
  }

  test_assert(p->samplerDeps.size() == 1);
  test_assert(!p->gpuAvailable && p->wgsl.size() == 0);  // no WGSL registered yet

  const float P[3] = {0.3f, -0.2f, 0.5f};
  const float N[3] = {0.0f, 0.0f, 1.0f};
  // eval = 0.5 * field(2px, py, pz) = 0.5 * (4px^2 + py^2 + pz^2)
  float expect = 0.5f * (4.0f * P[0] * P[0] + P[1] * P[1] + P[2] * P[2]);
  float v = p->eval(P, N, nullptr, nullptr);
  test_assert(std::abs(v - expect) <= 1e-6f);

  // grad chains through the sampler: d = (4px, py, pz). No fn_grad is
  // registered, so this exercises sb_hs_grad's central-difference synthesis.
  test_assert(p->evalDual != nullptr);
  sculptcore::brush::TexDual3 dp = {{P[0], P[1], P[2]},
                                    {1.0f, 0.0f, 0.0f},
                                    {0.0f, 1.0f, 0.0f},
                                    {0.0f, 0.0f, 1.0f}};
  sculptcore::brush::TexDual3 dn = {{N[0], N[1], N[2]},
                                    {0.0f, 0.0f, 0.0f},
                                    {0.0f, 0.0f, 0.0f},
                                    {0.0f, 0.0f, 0.0f}};
  sculptcore::brush::TexDual out = {};
  if (p->evalDual) {
    p->evalDual(&dp, &dn, nullptr, nullptr, &out);
    test_assert(std::abs(out.v - expect) <= 1e-6f);
    test_assert(std::abs(out.d[0] - 4.0f * P[0]) <= 1e-3f);
    test_assert(std::abs(out.d[1] - P[1]) <= 1e-3f);
    test_assert(std::abs(out.d[2] - P[2]) <= 1e-3f);
  }

  // The analytic gradient takes over on re-register — same numbers, float-tight.
  hs.fn_grad = hsFieldGrad;
  test_assert(registerHostSampler(hs));
  if (p->evalDual) {
    p->evalDual(&dp, &dn, nullptr, nullptr, &out);
    test_assert(std::abs(out.d[0] - 4.0f * P[0]) <= 1e-6f);
    test_assert(std::abs(out.d[1] - P[1]) <= 1e-6f);
    test_assert(std::abs(out.d[2] - P[2]) <= 1e-6f);
  }

  // Re-registering swaps the callback under a live program — no recompile.
  HostSampler swap = hs;
  swap.fn = hsFieldPlusOne;
  test_assert(registerHostSampler(swap));
  test_assert(findHostSampler("field") == entry);  // entry address is stable
  float v2 = p->eval(P, N, nullptr, nullptr);
  test_assert(std::abs(v2 - (expect + 0.5f)) <= 1e-6f);
  hs.fn = hsField;
  test_assert(registerHostSampler(hs));
  freeTextureProgram(p);

  // A WGSL snippet without a grad fn gets the FD wrapper synthesized.
  hs.wgsl = kFieldWgsl;
  test_assert(registerHostSampler(hs));
  p = compileTextureScript(kFieldSrc, "field.stex", error);
  test_assert(p != nullptr);
  if (p) {
    test_assert(p->gpuAvailable);
    test_assert(std::strstr(p->wgsl.c_str(), "fn hs_field(") != nullptr);
    test_assert(std::strstr(p->wgsl.c_str(), "fn hs_field_grad(") != nullptr);
    test_assert(std::strstr(p->wgsl.c_str(), "tex_fielded_eval") != nullptr);
    freeTextureProgram(p);
  }

  // A snippet with its own grad fn is used as-is — exactly one definition.
  hs.wgsl = kFieldWgslWithGrad;
  test_assert(registerHostSampler(hs));
  p = compileTextureScript(kFieldSrc, "field.stex", error);
  test_assert(p != nullptr);
  if (p) {
    test_assert(p->gpuAvailable);
    const char *first = std::strstr(p->wgsl.c_str(), "fn hs_field_grad");
    test_assert(first != nullptr);
    test_assert(first && std::strstr(first + 1, "fn hs_field_grad") == nullptr);
    freeTextureProgram(p);
  }

  // Two-arg samplers receive the eval's n.
  HostSampler nh;
  nh.name = "nfield";
  nh.fn = hsDotN;
  test_assert(registerHostSampler(nh));
  p = compileTextureScript(kTwoArgSrc, "twoarg.stex", error);
  if (!p) {
    fprintf(stderr, "compile error: %s\n", error.c_str());
  }
  test_assert(p != nullptr);
  if (p) {
    float d = p->eval(P, N, nullptr, nullptr);
    test_assert(std::abs(d - P[2]) <= 1e-6f);  // p . (0,0,1)
    freeTextureProgram(p);
  }

  // Unregister: bound programs read 0.0, fresh compiles fail dep resolution.
  p = compileTextureScript(kFieldSrc, "field.stex", error);
  test_assert(p != nullptr);
  test_assert(unregisterHostSampler("field"));
  if (p) {
    test_assert(p->eval(P, N, nullptr, nullptr) == 0.0f);
    freeTextureProgram(p);
  }
  TextureProgram *q = compileTextureScript(kFieldSrc, "field.stex", error);
  test_assert(q == nullptr);
  test_assert(std::strstr(error.c_str(), "not registered") != nullptr);
  test_assert(!unregisterHostSampler("never_registered"));

  // The ctypes-facing c-api mirror round-trips (defaulted fd_step, no wgsl).
  test_assert(sc_host_sampler_register("cfield", hsField, nullptr, nullptr, nullptr, 0.0f) == 1);
  const HostSampler *ce = findHostSampler("cfield");
  test_assert(ce != nullptr && ce->fn == hsField);
  test_assert(ce && ce->fd_step == 1e-3f && ce->wgsl.size() == 0);
  test_assert(sc_host_sampler_register(nullptr, hsField, nullptr, nullptr, nullptr, 0.0f) == 0);
  test_assert(sc_host_sampler_unregister(nullptr) == 0);
  test_assert(sc_host_sampler_unregister("cfield") == 1);
  test_assert(ce->fn == nullptr);
}

/** Builtin vnoise: compileTextureScript registers it itself — no host
 * registration step — with native CPU code and a WGSL twin. */
static void testBuiltinVnoise()
{
  static const char *kSrc =
      "sampler float vnoise(float3 p);\n"
      "texture Vn { float eval(float3 p, float3 n) { float v = vnoise(p); return v; } }";

  litestl::util::string error;
  TextureProgram *p = compileTextureScript(kSrc, "vn.stex", error);
  if (!p) {
    fprintf(stderr, "compile error: %s\n", error.c_str());
  }
  test_assert(p != nullptr);
  if (!p) {
    return;
  }
  test_assert(p->gpuAvailable);
  test_assert(std::strstr(p->wgsl.c_str(), "fn hs_vnoise(") != nullptr);

  const float N[3] = {0.0f, 0.0f, 1.0f};
  // Values stay in [0, 1) and the field is not constant.
  float lo = 2.0f, hi = -1.0f;
  for (int i = 0; i < 64; i++) {
    const float P[3] = {i * 0.37f - 8.0f, i * 0.61f + 2.0f, i * -0.23f};
    float v = p->eval(P, N, nullptr, nullptr);
    test_assert(v >= 0.0f && v < 1.0f);
    lo = v < lo ? v : lo;
    hi = v > hi ? v : hi;
  }
  test_assert(hi - lo > 0.1f);

  // C0 across a cell border (corner hashes are shared between cells).
  const float A[3] = {2.0f - 1e-4f, 0.4f, -1.3f};
  const float B[3] = {2.0f + 1e-4f, 0.4f, -1.3f};
  float va = p->eval(A, N, nullptr, nullptr);
  float vb = p->eval(B, N, nullptr, nullptr);
  test_assert(std::abs(va - vb) < 5e-3f);

  freeTextureProgram(p);
}

/** T5 splice: spliceTextureProgramWgsl's string surgery — the bitmap
 * brush_sample_tex definition renamed aside, the program WGSL appended, and a
 * wrapper calling tex_<name>_eval (with the render matrix when the program
 * maps points); plus the passthrough / error paths. */
static void testSplice()
{
  using sculptcore::brush::spliceTextureProgramWgsl;

  // A minimal stand-in for a generated kernel: one definition site plus a
  // call site that must survive untouched.
  static const char *kKernel =
      "fn brush_sample_tex(co: vec3<f32>, no: vec3<f32>) -> f32 {\n"
      "  return 1.0;\n"
      "}\n"
      "fn brush_strength(co: vec3<f32>, no: vec3<f32>) -> f32 {\n"
      "  return brush_sample_tex(co, no);\n"
      "}\n";

  litestl::util::string error;
  TextureProgram *rings = compileTextureScript(kRingsSrc, "rings.stex", error);
  test_assert(rings != nullptr && rings->gpuAvailable);
  if (rings) {
    litestl::util::string out = spliceTextureProgramWgsl(kKernel, *rings, error);
    test_assert(out.size() > 0);
    test_assert(std::strstr(out.c_str(), "fn brush_sample_tex_bitmap(") != nullptr);
    test_assert(std::strstr(out.c_str(), "tex_rings_eval(co, no)") != nullptr);
    test_assert(std::strstr(out.c_str(), "render_matrix") == nullptr);  // !usesMap
    // Exactly one brush_sample_tex definition remains: the wrapper.
    const char *def = std::strstr(out.c_str(), "fn brush_sample_tex(");
    test_assert(def != nullptr);
    test_assert(def && std::strstr(def + 1, "fn brush_sample_tex(") == nullptr);
    // A no-op program flag flip is refused (CPU-only programs never splice).
    rings->gpuAvailable = false;
    error = "";
    out = spliceTextureProgramWgsl(kKernel, *rings, error);
    test_assert(out.size() == 0 && error.size() > 0);
    freeTextureProgram(rings);
  }

  TextureProgram *scaled = compileTextureScript(kScaledSrc, "scaled.stex", error);
  test_assert(scaled != nullptr && scaled->gpuAvailable);
  if (scaled) {
    // Params read the host-uploaded binding-26 slab on the runtime path, not a
    // baked defaults const; mapPoint programs get the ctx matrix threaded in.
    test_assert(std::strstr(scaled->wgsl.c_str(), "sb_tex_params") != nullptr);
    test_assert(std::strstr(scaled->wgsl.c_str(), "@binding(26)") != nullptr);
    test_assert(std::strstr(scaled->wgsl.c_str(), "tex_scaled_defaults") == nullptr);
    litestl::util::string out = spliceTextureProgramWgsl(kKernel, *scaled, error);
    test_assert(out.size() > 0);
    test_assert(std::strstr(out.c_str(), "tex_scaled_eval(co, no, ctx_u.render_matrix)") !=
                nullptr);

    // A kernel that never defines brush_sample_tex (skip stub) passes through.
    static const char *kStub = "@compute fn nop() { }\n";
    out = spliceTextureProgramWgsl(kStub, *scaled, error);
    test_assert(out.size() > 0 && std::strcmp(out.c_str(), kStub) == 0);

    // Two definition sites are malformed input.
    litestl::util::string twice = litestl::util::string(kKernel) + kKernel;
    error = "";
    out = spliceTextureProgramWgsl(twice.c_str(), *scaled, error);
    test_assert(out.size() == 0);
    test_assert(std::strstr(error.c_str(), "more than once") != nullptr);
    freeTextureProgram(scaled);
  }
}

static void runTests()
{
  test_assert(sculptcore::brush::textureScriptCpuAvailable());
  testRings();
  testScaled();
  testErrors();
  testSamplers();
  testBuiltinVnoise();
  testSplice();
}

int main()
{
  runTests();
  return test_end();
}
