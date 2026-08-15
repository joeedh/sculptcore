#include "test_util.h"

#include "brush/compiler/emit_c.h"
#include "brush/compiler/lexer.h"
#include "brush/compiler/parser.h"
#include "brush/texture_eval.h"
#include "brush/texture_jit.h"
#include "brush/texture_registry.h"

#include <tinycc/libtcc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

test_init;

using namespace sculptcore::brush::sbrush;

/** End-to-end gate for the C99 texture backend (texture-scripts T3.2):
 * emitCTextureDefs' output must actually compile under the embedded tinycc
 * JIT and evaluate identically to the precompiled registry path.
 *
 * Unit 1 (rings.stex's body): value parity against findTexture("Rings")->eval
 * at 1e-6 (clang may contract FMAs; tcc never does, so bit-equality is not
 * required — the definitive gate is T3.4's stroke A/B), plus the emitted dual
 * twin against central differences under an identity seed.
 *
 * Unit 2 (params + ramp + mapPoint): the exported defaults slab, the omitted
 * dual twin (ramp.sample is not differentiable), and eval parity against the
 * same computation done here through the host texture_eval.h helpers. A
 * relocated TCC state cannot compile again, hence the second state. */

static void tccError(void *, const char *msg)
{
  fprintf(stderr, "tcc: %s\n", msg);
}

// The same std:: calls the cpp backend compiles to, so parity is exact
// modulo tcc's lack of FP contraction.
static float hostSinf(float x)
{
  return std::sin(x);
}
static float hostCosf(float x)
{
  return std::cos(x);
}
static float hostSqrtf(float x)
{
  return std::sqrt(x);
}
static float hostFloorf(float x)
{
  return std::floor(x);
}
static float hostFabsf(float x)
{
  return std::abs(x);
}
static float hostPowf(float x, float y)
{
  return std::pow(x, y);
}
static float hostAtan2f(float y, float x)
{
  return std::atan2(y, x);
}
static float hostExpf(float x)
{
  return std::exp(x);
}
static float hostLogf(float x)
{
  return std::log(x);
}

// kernels/rings.stex's body, verbatim.
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

// Runtime param + ramp + mapPoint: exercises the defaults slab, the ctx
// plumbing, and the dual-twin omission path.
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

// The transcendental + shaping intrinsics the Blender procedural ports need.
// Each is fed an argument that stays well clear of its non-smooth points, so
// the dual twin can be checked against central differences: `mod` sits mid
// period, `step`'s edge is far from p, `smoothstep` is inside its ramp, and
// `pow`/`log` see a strictly positive base.
static const char *kMathSrc = R"(
texture Mathy {
  float eval(float3 p, float3 n) {
    float a = pow(p.x + 2.0, 3.0);
    float b = atan2(p.y, p.x + 2.0);
    float c = exp(p.x * 0.5);
    float d = log(p.z + 3.0);
    float e = mod(p.x + 0.5, 1.0);
    float f = step(-1.0, p.x);
    float g = smoothstep(-1.0, 1.0, p.y);
    return a + b + c + d + e + f + g;
  }
}
)";

// Mirrors of the emitted TU's sbdual/sbdual3 structs (plain float layout).
struct CDual {
  float v;
  float d[3];
};
struct CDual3 {
  float v[3];
  float dx[3];
  float dy[3];
  float dz[3];
};

using EvalFn = float (*)(const float *, const float *, const float *, const void *);
using EvalDFn = void (*)(const CDual3 *, const CDual3 *, const float *, const void *, CDual *);

static bool emitUnit(const char *src, const char *fname, EmitResult &er)
{
  LexResult lr = lex(src, fname);
  test_assert(lr.errors.size() == 0);
  ParseResult pr = parse(lr.tokens, fname);
  test_assert(pr.errors.size() == 0);
  test_assert(pr.unit != nullptr);
  if (retval || !pr.unit) {
    return false;
  }

  Brush scratch;
  scratch.sourceFile = pr.unit->sourceFile;
  for (auto &td : pr.unit->textures) {
    scratch.textures.append(std::move(td));
  }

  er = emitCTextureDefs(scratch);
  for (const auto &e : er.errors) {
    fprintf(stderr, "emit: %s\n", e.c_str());
  }
  test_assert(er.errors.size() == 0);
  return er.errors.size() == 0;
}

static TCCState *jitCompile(const char *text)
{
  TCCState *s = tcc_new();
  test_assert(s != nullptr);
  if (!s) {
    return nullptr;
  }

  tcc_set_error_func(s, nullptr, tccError);
  tcc_set_options(s, "-nostdlib");
  test_assert(tcc_set_output_type(s, TCC_OUTPUT_MEMORY) == 0);
  test_assert(tcc_add_symbol(s, "sinf", (const void *)hostSinf) == 0);
  test_assert(tcc_add_symbol(s, "cosf", (const void *)hostCosf) == 0);
  test_assert(tcc_add_symbol(s, "sqrtf", (const void *)hostSqrtf) == 0);
  test_assert(tcc_add_symbol(s, "floorf", (const void *)hostFloorf) == 0);
  test_assert(tcc_add_symbol(s, "fabsf", (const void *)hostFabsf) == 0);
  // The dual prelude is emitted unconditionally, and sbd_pow/exp/log/atan2
  // reference these — so every JIT site must bind them, not just scripts that
  // call the intrinsics.
  test_assert(tcc_add_symbol(s, "powf", (const void *)hostPowf) == 0);
  test_assert(tcc_add_symbol(s, "atan2f", (const void *)hostAtan2f) == 0);
  test_assert(tcc_add_symbol(s, "expf", (const void *)hostExpf) == 0);
  test_assert(tcc_add_symbol(s, "logf", (const void *)hostLogf) == 0);

  if (tcc_compile_string(s, text) != 0 || tcc_relocate(s) != 0) {
    test_assert(!"tcc compile/relocate failed");
    tcc_delete(s);
    return nullptr;
  }
  return s;
}

static void testRings()
{
  EmitResult er;
  if (!emitUnit(kRingsSrc, "rings.stex", er)) {
    return;
  }

  TCCState *s = jitCompile(er.text.c_str());
  if (!s) {
    return;
  }

  EvalFn eval = (EvalFn)tcc_get_symbol(s, "tex_rings_eval");
  test_assert(eval != nullptr);

  const sculptcore::brush::TextureRegistryEntry *ref = sculptcore::brush::findTexture("Rings");
  test_assert(ref != nullptr);

  if (eval && ref) {
    static const float pts[5][3] = {
        {0.31f, -0.14f, 0.52f},
        {0.0f, 0.0f, 0.0f},
        {1.7f, 0.3f, -0.9f},
        {-0.25f, 0.6f, 0.1f},
        {0.05f, -0.05f, 0.02f},
    };
    const float n[3] = {0.0f, 0.0f, 1.0f};
    for (int i = 0; i < 5; i++) {
      const float *p = pts[i];
      float jit = eval(p, n, ref->defaults, nullptr);
      float refv = ref->eval(litestl::math::float3{p[0], p[1], p[2]},
                             litestl::math::float3{n[0], n[1], n[2]},
                             ref->defaults,
                             nullptr);
      test_assert(std::abs(jit - refv) <= 1e-6f);
    }
  }

  EvalDFn evalD = (EvalDFn)tcc_get_symbol(s, "tex_rings_eval_d");
  test_assert(evalD != nullptr);

  if (eval && evalD) {
    // 0.31/-0.14/0.52 puts d*24 ~0.09 from the nearest floor/fract
    // discontinuity, so h=1e-3 central differences stay on one branch.
    const float P[3] = {0.31f, -0.14f, 0.52f};
    const float N[3] = {0.0f, 0.0f, 1.0f};

    CDual3 p = {};
    CDual3 nn = {};
    for (int i = 0; i < 3; i++) {
      p.v[i] = P[i];
      nn.v[i] = N[i];
    }
    p.dx[0] = 1.0f;
    p.dy[1] = 1.0f;
    p.dz[2] = 1.0f;

    CDual out = {};
    evalD(&p, &nn, nullptr, nullptr, &out);
    test_assert(std::abs(out.v - eval(P, N, nullptr, nullptr)) <= 1e-6f);

    const float h = 1e-3f;
    for (int i = 0; i < 3; i++) {
      float lo[3] = {P[0], P[1], P[2]};
      float hi[3] = {P[0], P[1], P[2]};
      lo[i] -= h;
      hi[i] += h;
      float fd = (eval(hi, N, nullptr, nullptr) - eval(lo, N, nullptr, nullptr)) / (2.0f * h);
      test_assert(std::abs(out.d[i] - fd) <= 2e-2f * (1.0f + std::abs(fd)));
    }
  }

  tcc_delete(s);
}

static void testScaled()
{
  EmitResult er;
  if (!emitUnit(kScaledSrc, "scaled.stex", er)) {
    return;
  }
  test_assert(std::strstr(er.text.c_str(), "tex_scaled_eval_d omitted") != nullptr);

  TCCState *s = jitCompile(er.text.c_str());
  if (!s) {
    return;
  }

  // Slab layout: scale at 0, shape's 256 identity-ramp slots at 1..256.
  const float *defaults = (const float *)tcc_get_symbol(s, "tex_scaled_param_defaults");
  test_assert(defaults != nullptr);
  if (defaults) {
    test_assert(defaults[0] == 2.0f);
    test_assert(defaults[1] == 0.0f);
    test_assert(defaults[256] == 1.0f);
  }

  test_assert(tcc_get_symbol(s, "tex_scaled_eval_d") == nullptr);

  EvalFn eval = (EvalFn)tcc_get_symbol(s, "tex_scaled_eval");
  test_assert(eval != nullptr);

  if (eval && defaults) {
    sculptcore::brush::TexEvalCtx ctx = {{
        1.5f, 0.0f, 0.0f, 0.2f,   //
        0.0f, 0.8f, 0.0f, -0.1f,  //
        0.0f, 0.0f, 1.1f, 0.05f,  //
        0.0f, 0.0f, 0.0f, 1.0f,   //
    }};
    const float P[3] = {0.31f, -0.14f, 0.52f};
    const float N[3] = {0.0f, 0.0f, 1.0f};

    float jit = eval(P, N, defaults, &ctx);

    litestl::math::float3 q = sculptcore::brush::texMapPoint(
        &ctx, litestl::math::float3{P[0], P[1], P[2]});
    float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    float t = len * defaults[0];
    t = t - std::floor(t);
    float expect = sculptcore::brush::texRampSample(defaults + 1, t);
    test_assert(std::abs(jit - expect) <= 1e-6f);
  }

  tcc_delete(s);
}

static void testMath()
{
  EmitResult er;
  if (!emitUnit(kMathSrc, "mathy.stex", er)) {
    return;
  }

  TCCState *s = jitCompile(er.text.c_str());
  if (!s) {
    return;
  }

  EvalFn eval = (EvalFn)tcc_get_symbol(s, "tex_mathy_eval");
  test_assert(eval != nullptr);

  // No intrinsic here may roll the dual body back — each has an sbd_* rule.
  EvalDFn evalD = (EvalDFn)tcc_get_symbol(s, "tex_mathy_eval_d");
  test_assert(evalD != nullptr);

  const float P[3] = {0.31f, -0.14f, 0.52f};
  const float N[3] = {0.0f, 0.0f, 1.0f};

  if (eval) {
    float expect = std::pow(P[0] + 2.0f, 3.0f) + std::atan2(P[1], P[0] + 2.0f) +
                   std::exp(P[0] * 0.5f) + std::log(P[2] + 3.0f);
    float m = P[0] + 0.5f;
    expect += m - std::floor(m);
    expect += P[0] < -1.0f ? 0.0f : 1.0f;
    float t = std::clamp((P[1] + 1.0f) * 0.5f, 0.0f, 1.0f);
    expect += t * t * (3.0f - 2.0f * t);
    test_assert(std::abs(eval(P, N, nullptr, nullptr) - expect) <= 1e-5f);
  }

  if (eval && evalD) {
    CDual3 p = {};
    CDual3 nn = {};
    for (int i = 0; i < 3; i++) {
      p.v[i] = P[i];
      nn.v[i] = N[i];
    }
    p.dx[0] = 1.0f;
    p.dy[1] = 1.0f;
    p.dz[2] = 1.0f;

    CDual out = {};
    evalD(&p, &nn, nullptr, nullptr, &out);
    test_assert(std::abs(out.v - eval(P, N, nullptr, nullptr)) <= 1e-5f);

    const float h = 1e-3f;
    for (int i = 0; i < 3; i++) {
      float lo[3] = {P[0], P[1], P[2]};
      float hi[3] = {P[0], P[1], P[2]};
      lo[i] -= h;
      hi[i] += h;
      float fd = (eval(hi, N, nullptr, nullptr) - eval(lo, N, nullptr, nullptr)) / (2.0f * h);
      test_assert(std::abs(out.d[i] - fd) <= 2e-2f * (1.0f + std::abs(fd)));
    }
  }

  tcc_delete(s);
}

static void runTests()
{
  test_assert(sculptcore::brush::textureScriptCpuAvailable());
  testRings();
  testScaled();
  testMath();
}

int main()
{
  runTests();
  return test_end();
}
