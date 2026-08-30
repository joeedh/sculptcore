// Spike A.5 (documentation/plans/native-electron.md): a trivial N-API addon to
// de-risk the primary build question — does the repo's Windows *clang*
// toolchain (clang++ targeting the MSVC ABI, inside the vcvars env) link against
// Electron's MSVC-built node.lib and load in the Electron process?
//
// If `hello()` / `add()` round-trip through Electron, the ABI/link assumption
// holds and Workstream A/B can build the real reflection runtime the same way.

#include <napi.h>

static Napi::String Hello(const Napi::CallbackInfo &info)
{
  return Napi::String::New(info.Env(), "hello from native clang N-API addon");
}

static Napi::Value Add(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();

  // Read via the raw C N-API so we can inspect status + arg count and tell an
  // ABI/marshalling fault apart from a spike-code bug.
  double a = 0.0, b = 0.0;
  napi_status sa = napi_get_value_double(env, info[0], &a);
  napi_status sb = napi_get_value_double(env, info[1], &b);

  Napi::Object o = Napi::Object::New(env);
  o.Set("argc", Napi::Number::New(env, static_cast<double>(info.Length())));
  o.Set("isNum0", Napi::Boolean::New(env, info[0].IsNumber()));
  o.Set("a", Napi::Number::New(env, a));
  o.Set("b", Napi::Number::New(env, b));
  o.Set("statusA", Napi::Number::New(env, static_cast<double>(sa)));
  o.Set("statusB", Napi::Number::New(env, static_cast<double>(sb)));
  o.Set("sum", Napi::Number::New(env, a + b));
  return o;
}

// Reports the compiler that actually built this .node, so the spike result
// proves it was clang (not MSVC cl) that produced the object code.
static Napi::String Compiler(const Napi::CallbackInfo &info)
{
#if defined(__clang__)
  return Napi::String::New(info.Env(), "clang " __clang_version__);
#elif defined(_MSC_VER)
  return Napi::String::New(info.Env(), "msvc");
#else
  return Napi::String::New(info.Env(), "unknown");
#endif
}

// A *raw* napi_callback (no node-addon-api wrappers) — this is the style
// Workstream B's reflection runtime would use. Reads argc + two doubles through
// the C ABI directly to isolate whether any garbage is in the C ABI or only in
// node-addon-api's inline C++ wrappers.
static napi_value RawAdd(napi_env env, napi_callback_info cbinfo)
{
  size_t argc = 4;
  napi_value argv[4];
  napi_get_cb_info(env, cbinfo, &argc, argv, nullptr, nullptr);

  double a = 0.0, b = 0.0;
  if (argc > 0)
    napi_get_value_double(env, argv[0], &a);
  if (argc > 1)
    napi_get_value_double(env, argv[1], &b);

  napi_value out, v;
  napi_create_object(env, &out);
  napi_create_double(env, static_cast<double>(argc), &v);
  napi_set_named_property(env, out, "rawArgc", v);
  napi_create_double(env, a + b, &v);
  napi_set_named_property(env, out, "rawSum", v);
  return out;
}

static Napi::Object Init(Napi::Env env, Napi::Object exports)
{
  exports.Set("hello", Napi::Function::New(env, Hello));
  exports.Set("add", Napi::Function::New(env, Add));
  exports.Set("compiler", Napi::Function::New(env, Compiler));

  napi_value rawAdd;
  napi_create_function(env, "rawAdd", NAPI_AUTO_LENGTH, RawAdd, nullptr, &rawAdd);
  napi_set_named_property(env, exports, "rawAdd", rawAdd);
  return exports;
}

NODE_API_MODULE(sculptcore_napi_spike, Init)
