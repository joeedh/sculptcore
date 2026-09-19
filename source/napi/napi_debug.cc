// litestl allocator introspection (binding.cc LSTL_*) and process-level
// debug/diagnostic exports: alloc-block dumps, the stdout smoke test used to
// verify native stdout reaches a launched NW.js process, the Crashpad
// self-test, and the GUI-subsystem stdout redirect workaround.

#include "napi_runtime.h"

#include <cstdio>
#include <string>
#include <vector>

#include "napi_c_api.h"

namespace sculptcore::napi {

napi_value NapiRuntime::GetMemSize(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool includePermanent = false;
  if (argc >= 1)
    napi_get_value_bool(env, argv[0], &includePermanent);
  napi_value out;
  napi_create_double(env, static_cast<double>(LSTL_GetMemSize(includePermanent)), &out);
  return out;
}

// printAllocBlocks(includePermanent) -> void. Dumps every live block to the log
// sink (the renderer DevTools console, via consoleSink).
napi_value NapiRuntime::PrintAllocBlocks(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool includePermanent = false;
  if (argc >= 1)
    napi_get_value_bool(env, argv[0], &includePermanent);
  LSTL_PrintAllocBlocks(includePermanent);
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// formatBlock(boundObj) -> a string describing the allocation backing the
// wrapped C++ object. LSTL_FormatBlock returns a raw-heap char* the caller must
// release; we copy it into a JS string and free it with LSTL_FreeFormatBlocks
// (the char* deliberately never crosses into JS as a number).
napi_value NapiRuntime::FormatBlock(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);

  Wrapped *w = nullptr;
  if (argc < 1 || napi_unwrap(env, argv[0], reinterpret_cast<void **>(&w)) != napi_ok ||
      !w || !w->ptr)
  {
    return out;
  }
  char *s = LSTL_FormatBlock(w->ptr);
  if (!s)
    return out;
  napi_create_string_utf8(env, s, NAPI_AUTO_LENGTH, &out);
  LSTL_FreeFormatBlocks(s);
  return out;
}

// formatBlocks(printPermanent) -> a string describing every live allocation
// block (the whole-heap counterpart of formatBlock). Same raw-heap-string /
// LSTL_FreeFormatBlocks release pattern.
napi_value NapiRuntime::FormatBlocks(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);

  bool printPermanent = false;
  if (argc >= 1)
    napi_get_value_bool(env, argv[0], &printPermanent);
  char *s = LSTL_FormatBlocks(printPermanent);
  if (!s)
    return out;
  napi_create_string_utf8(env, s, NAPI_AUTO_LENGTH, &out);
  LSTL_FreeFormatBlocks(s);
  return out;
}

// testPrint(msg?) -> void. Writes the (optional) message to the process stdout
// directly from C++ (not the console.log sink) and flushes, so a headless test
// can assert whether native-side stdout reaches the launched NW.js process's
// captured output. Defaults to a fixed marker when called with no argument.
napi_value NapiRuntime::TestPrint(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value undef;
  napi_get_undefined(env, &undef);

  std::string msg = "[sculptcore::testPrint] native stdout OK";
  if (argc >= 1) {
    napi_valuetype t = napi_undefined;
    napi_typeof(env, argv[0], &t);
    if (t == napi_string) {
      size_t len = 0;
      napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
      std::vector<char> buf(len + 1, 0);
      napi_get_value_string_utf8(env, argv[0], buf.data(), len + 1, &len);
      msg.assign(buf.data(), len);
    }
  }

  std::fputs(msg.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
  return undef;
}

// crashTest() -> void. Crashpad self-test: derefs a null pointer to fault inside
// sculptcore_node, so a minidump carries a native stack symbolicated by the
// addon's CodeView PDB. Driven by the harness --apptest-crash flag; see
// documentation/plans/crashpad.md.
napi_value NapiRuntime::CrashTest(napi_env env, napi_callback_info info)
{
  (void)info;
  volatile int *sculptcoreCrashTestNullDeref = nullptr;
  *sculptcoreCrashTestNullDeref = 0xC0FFEE;
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// redirectStdout(path) -> boolean. freopen()s the C stdout stream onto `path`
// (unbuffered). The NW.js renderer starts with fd 0/1/2 closed (EBADF), so a
// plain printf is written to a dead fd and lost; pointing stdout at a launcher-
// supplied file gives later TestPrint output a real destination the wrapper
// reads back — the standard Windows GUI-subsystem stdout workaround.
napi_value NapiRuntime::RedirectStdout(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  bool ok = false;
  if (argc >= 1) {
    size_t len = 0;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    std::vector<char> buf(len + 1, 0);
    napi_get_value_string_utf8(env, argv[0], buf.data(), len + 1, &len);
    FILE *f = std::freopen(buf.data(), "w", stdout);
    if (f) {
      std::setvbuf(stdout, nullptr, _IONBF, 0);
      ok = true;
    }
  }
  napi_get_boolean(env, ok, &out);
  return out;
}

} // namespace sculptcore::napi
