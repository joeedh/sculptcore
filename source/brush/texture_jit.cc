#include "texture_jit.h"

#ifdef SCULPTCORE_HAVE_TCC
#include <tinycc/libtcc.h>
#endif

namespace sculptcore::brush {

#ifdef SCULPTCORE_HAVE_TCC
/* A failed probe reports through the return value; keep tcc's stderr quiet. */
static void probeErrorSink(void *, const char *)
{
}

static bool jitProbe()
{
  TCCState *s = tcc_new();
  if (!s) {
    return false;
  }

  bool ok = false;
  tcc_set_error_func(s, nullptr, probeErrorSink);
  tcc_set_options(s, "-nostdlib");

  if (tcc_set_output_type(s, TCC_OUTPUT_MEMORY) == 0 &&
      tcc_compile_string(s, "int sc_tcc_probe(int x) { return x + 41; }") == 0 &&
      tcc_relocate(s) == 0)
  {
    int (*fn)(int) = (int (*)(int))tcc_get_symbol(s, "sc_tcc_probe");
    ok = fn && fn(1) == 42;
  }

  tcc_delete(s);
  return ok;
}
#endif

bool textureScriptCpuAvailable()
{
#ifdef SCULPTCORE_HAVE_TCC
  static bool available = jitProbe();
  return available;
#else
  return false;
#endif
}

}  // namespace sculptcore::brush
