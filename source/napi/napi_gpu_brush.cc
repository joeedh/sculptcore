#include "napi_runtime.h"

#include "napi_c_api.h"

namespace sculptcore::napi {

// --- GPU brush-stroke seam (gpu_brush_c_api.cc) ------------------------------
// The session pointer crosses as a napi external: opaque, never dereferenced by
// JS, lifetime owned by GpuBrush_endStroke/GpuBrush_free (no GC finalizer — a
// leaked session is a bug the debug surface should expose, not silently free).

namespace {

void *externalArg(napi_env env, napi_value v)
{
  void *p = nullptr;
  if (napi_get_value_external(env, v, &p) != napi_ok) {
    return nullptr;
  }
  return p;
}

// Borrow a Float32Array argument's backing store; null for non-typed-array
// args (undefined/null cross as "absent").
const float *floatsArg(napi_env env, napi_value v, size_t *outLen)
{
  *outLen = 0;
  bool isTa = false;
  napi_is_typedarray(env, v, &isTa);
  if (!isTa) {
    return nullptr;
  }
  napi_typedarray_type type;
  size_t len = 0;
  void *data = nullptr;
  if (napi_get_typedarray_info(env, v, &type, &len, &data, nullptr, nullptr) != napi_ok ||
      type != napi_float32_array)
  {
    return nullptr;
  }
  *outLen = len;
  return static_cast<const float *>(data);
}

} // namespace

napi_value NapiRuntime::GpuBrushBeginStroke(napi_env env, napi_callback_info info)
{
  size_t argc = 5;
  napi_value argv[5];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);
  if (argc < 5) {
    return out;
  }
  Wrapped *mw = nullptr, *tw = nullptr, *bw = nullptr, *lw = nullptr;
  if (napi_unwrap(env, argv[0], reinterpret_cast<void **>(&mw)) != napi_ok || !mw ||
      napi_unwrap(env, argv[1], reinterpret_cast<void **>(&tw)) != napi_ok || !tw ||
      napi_unwrap(env, argv[2], reinterpret_cast<void **>(&bw)) != napi_ok || !bw ||
      napi_unwrap(env, argv[3], reinterpret_cast<void **>(&lw)) != napi_ok || !lw)
  {
    return out;
  }
  int32_t tool = 0;
  napi_get_value_int32(env, argv[4], &tool);
  void *s = GpuBrush_beginStroke(mw->ptr, tw->ptr, bw->ptr, lw->ptr, tool);
  if (!s) {
    return out;
  }
  napi_create_external(env, s, nullptr, nullptr, &out);
  return out;
}

napi_value NapiRuntime::GpuBrushFree(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc >= 1) {
    GpuBrush_free(externalArg(env, argv[0]));
  }
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value NapiRuntime::GpuBrushKernelName(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  const char *name = argc >= 1 ? GpuBrush_kernelName(externalArg(env, argv[0])) : "";
  napi_value out;
  napi_create_string_utf8(env, name ? name : "", NAPI_AUTO_LENGTH, &out);
  return out;
}

napi_value NapiRuntime::GpuBrushInfo(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t which = 0;
  int v = 0;
  if (argc >= 2) {
    napi_get_value_int32(env, argv[1], &which);
    v = GpuBrush_info(externalArg(env, argv[0]), which);
  }
  napi_value out;
  napi_create_int32(env, v, &out);
  return out;
}

napi_value NapiRuntime::GpuBrushMarshalDab(napi_env env, napi_callback_info info)
{
  size_t argc = 11;
  napi_value argv[11];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_create_int32(env, 0, &out);
  if (argc < 11) {
    return out;
  }
  void *s = externalArg(env, argv[0]);
  double f[8] = {};
  for (int i = 0; i < 8; i++) {
    napi_get_value_double(env, argv[1 + i], &f[i]);
  }
  int32_t mirrorIdx = 0, nonaccum = 0;
  napi_get_value_int32(env, argv[9], &mirrorIdx);
  napi_get_value_int32(env, argv[10], &nonaccum);
  int n = GpuBrush_marshalDab(s,
                              float(f[0]),
                              float(f[1]),
                              float(f[2]),
                              float(f[3]),
                              float(f[4]),
                              float(f[5]),
                              float(f[6]),
                              float(f[7]),
                              mirrorIdx,
                              nonaccum);
  napi_create_int32(env, n, &out);
  return out;
}

napi_value NapiRuntime::GpuBrushData(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);
  if (argc < 2) {
    return out;
  }
  void *s = externalArg(env, argv[0]);
  int32_t which = 0;
  napi_get_value_int32(env, argv[1], &which);
  const int size = GpuBrush_dataSize(s, which);
  const void *src = GpuBrush_dataPtr(s, which);
  // Never throw on the bulk-data seam: an empty blob crosses as a 0-length view.
  const size_t n = (size > 0 && src) ? size_t(size) : 0;
  napi_value ab;
  void *abData = nullptr;
  napi_create_arraybuffer(env, n, &abData, &ab);
  if (abData && n) {
    std::memcpy(abData, src, n);
  }
  napi_create_typedarray(env, napi_uint8_array, n, ab, 0, &out);
  return out;
}

napi_value NapiRuntime::GpuBrushApplyCo(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value undef;
  napi_get_undefined(env, &undef);
  if (argc < 2) {
    return undef;
  }
  size_t len = 0;
  const float *co = floatsArg(env, argv[1], &len);
  if (co && len % 3 == 0) {
    GpuBrush_applyCo(externalArg(env, argv[0]), co, int(len / 3));
  }
  return undef;
}

napi_value NapiRuntime::GpuBrushEndStroke(napi_env env, napi_callback_info info)
{
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value undef;
  napi_get_undefined(env, &undef);
  if (argc < 1) {
    return undef;
  }
  size_t coLen = 0, noLen = 0;
  const float *co = argc >= 2 ? floatsArg(env, argv[1], &coLen) : nullptr;
  const float *no = argc >= 3 ? floatsArg(env, argv[2], &noLen) : nullptr;
  (void)noLen;
  GpuBrush_endStroke(externalArg(env, argv[0]), co, no, int(coLen / 3));
  return undef;
}

} // namespace sculptcore::napi
