#include "test_util.h"

#include "brush/texture_jit.h"

#include <tinycc/libtcc.h>

test_init;

/** Gate for the embedded tinycc JIT (texture-scripts milestone T3.1).
 *
 * Two layers: the engine's own capability query (textureScriptCpuAvailable —
 * its first-call probe is the structural stand-in for the macOS MAP_JIT /
 * allow-jit entitlement check, which only a real hardened-runtime host can
 * exercise), and a direct run of the strategy compileTextureScript will use:
 * -nostdlib, host symbols bound with tcc_add_symbol, float math through a
 * pointer parameter — the shape of a texture eval entry point. */

static void tccError(void *, const char *msg)
{
  fprintf(stderr, "tcc: %s\n", msg);
}

static float hostMul(float a, float b)
{
  return a * b;
}

static void runTests()
{
  test_assert(sculptcore::brush::textureScriptCpuAvailable());

  TCCState *s = tcc_new();
  test_assert(s != nullptr);
  if (!s) {
    return;
  }

  tcc_set_error_func(s, nullptr, tccError);
  tcc_set_options(s, "-nostdlib");
  test_assert(tcc_set_output_type(s, TCC_OUTPUT_MEMORY) == 0);
  test_assert(tcc_add_symbol(s, "host_mul", (const void *)hostMul) == 0);

  static const char *src = "float host_mul(float a, float b);\n"
                           "float sc_scale(const float *p, float k) {\n"
                           "  return host_mul(p[0], k) + host_mul(p[1], k) + p[2];\n"
                           "}\n";
  test_assert(tcc_compile_string(s, src) == 0);
  test_assert(tcc_relocate(s) == 0);

  float (*fn)(const float *, float);
  fn = (float (*)(const float *, float))tcc_get_symbol(s, "sc_scale");
  test_assert(fn != nullptr);
  if (fn) {
    const float p[3] = {1.0f, 2.0f, 4.0f};
    test_assert(fn(p, 3.0f) == 13.0f);
  }

  tcc_delete(s);
}

int main()
{
  runTests();
  return test_end();
}
