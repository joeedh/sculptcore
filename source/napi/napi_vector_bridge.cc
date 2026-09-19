// Bulk-data fast path / minimal Vector<T> surface: typed-array views over a
// bound litestl::util::Vector's contiguous storage, indexed element access,
// and the JS->C++ assign direction (Vector out-params otherwise have no way
// back). Also pointerBytes/objectAddress, the sibling raw-pointer bulk-data
// and identity-key seams.

#include "napi_runtime.h"

#include <cstring>

#include "napi_c_api.h"
#include "napi_util.h"

#include "litestl/binding/binding_types.h"

namespace sculptcore::napi {

using binding::BindingType;
using binding::NumberType;

// litestl::util::Vector layout (native): T* data_ @0, size_t size_ @8.
static const size_t kVecDataOffset = 0;
static const size_t kVecSizeOffset = sizeof(void *);

static bool isVectorStruct(const types::_StructBase *st)
{
  // strcmp avoids the ambiguous util::string == const char* overload.
  return st && std::strcmp(st->name.c_str(), "litestl::util::Vector") == 0 &&
         st->templateParams.size() >= 1;
}

// Map a Number element descriptor to a typed-array kind; false for non-numbers.
static bool numberTypedArrayKind(const binding::BindingBase *elem,
                                 napi_typedarray_type *out)
{
  if (!elem || elem->type != BindingType::Number)
    return false;
  const types::Number<char> *n = asNumber(elem);
  const bool u = isUnsigned(n);
  switch (n->subtype) {
  case NumberType::Int8:
    *out = u ? napi_uint8_array : napi_int8_array;
    return true;
  case NumberType::Int16:
    *out = u ? napi_uint16_array : napi_int16_array;
    return true;
  case NumberType::Int32:
    *out = u ? napi_uint32_array : napi_int32_array;
    return true;
  case NumberType::Int64:
    *out = u ? napi_biguint64_array : napi_bigint64_array;
    return true;
  case NumberType::Float32:
    *out = napi_float32_array;
    return true;
  case NumberType::Float64:
    *out = napi_float64_array;
    return true;
  }
  return false;
}

static Wrapped *unwrapVector(napi_env env, size_t argc, napi_value *argv)
{
  Wrapped *w = nullptr;
  if (argc < 1 || napi_unwrap(env, argv[0], reinterpret_cast<void **>(&w)) != napi_ok) {
    return nullptr;
  }
  return (w && isVectorStruct(w->st)) ? w : nullptr;
}

napi_value NapiRuntime::VectorLength(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  Wrapped *w = unwrapVector(env, argc, argv);
  if (!w) {
    napi_get_undefined(env, &out);
    return out;
  }
  size_t count =
      *reinterpret_cast<size_t *>(static_cast<char *>(w->ptr) + kVecSizeOffset);
  napi_create_uint32(env, static_cast<uint32_t>(count), &out);
  return out;
}

// vectorView(vec) -> a typed array over the Vector's contiguous storage (the
// bulk-data fast path: zero per-element napi calls). External ArrayBuffer, no
// finalizer — the C++ Vector owns the memory, so the caller must keep the bound
// Vector alive while the view is in use.
napi_value NapiRuntime::VectorView(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  Wrapped *w = unwrapVector(env, argc, argv);
  if (!w) {
    napi_get_undefined(env, &out);
    return out;
  }
  void *dataPtr =
      *reinterpret_cast<void **>(static_cast<char *>(w->ptr) + kVecDataOffset);
  size_t count =
      *reinterpret_cast<size_t *>(static_cast<char *>(w->ptr) + kVecSizeOffset);
  const binding::BindingBase *elem = w->st->templateParams[0].type;
  size_t elemSize = elem ? elem->getSize() : 0;
  if (!dataPtr || elemSize == 0) {
    napi_get_undefined(env, &out);
    return out;
  }

  const size_t byteLen = count * elemSize;
  napi_value ab;
  // Prefer a true zero-copy external ArrayBuffer over the C++ storage. Electron
  // enables the V8 sandbox, which forbids ArrayBuffers backed by memory outside
  // the sandbox (returns napi_no_external_buffers_allowed) — so fall back to a
  // one-shot copy into a sandbox-internal ArrayBuffer. Still O(1) napi calls per
  // buffer (one memcpy) vs O(n) per-element getters.
  napi_status st =
      napi_create_external_arraybuffer(env, dataPtr, byteLen, nullptr, nullptr, &ab);
  if (st != napi_ok) {
    bool pending = false;
    napi_is_exception_pending(env, &pending);
    if (pending) {
      napi_value e;
      napi_get_and_clear_last_exception(env, &e);
    }
    void *abData = nullptr;
    napi_create_arraybuffer(env, byteLen, &abData, &ab);
    if (abData)
      std::memcpy(abData, dataPtr, byteLen);
  }

  napi_typedarray_type ta;
  if (numberTypedArrayKind(elem, &ta)) {
    napi_create_typedarray(env, ta, count, ab, 0, &out);
  } else {
    napi_create_typedarray(env, napi_uint8_array, count * elemSize, ab, 0, &out);
  }
  return out;
}

// pointerBytes(boundObj, memberName, byteLen, byteOffset?) -> a Uint8Array over
// the bytes a raw-pointer member points at, starting `byteOffset` (default 0)
// bytes in. The native equivalent of the WASM
// `new Uint8Array(HEAPU8.buffer, buf.data + off, n)` bulk-data read
// (gpuExecutor.ts): the pointer (e.g. gpu::Buffer.data, a void*) deliberately
// never crosses into JS as a number, so C++ reads it off the descriptor here
// and views it. Same external->copy fallback as VectorView (V8 sandbox forbids
// external buffers in Electron). The C++ object owns the storage; the caller
// must keep the bound object alive while the view is used.
napi_value NapiRuntime::PointerBytes(napi_env env, napi_callback_info info)
{
  size_t argc = 4;
  napi_value argv[4];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);
  if (argc < 3)
    return out;

  Wrapped *w = nullptr;
  if (napi_unwrap(env, argv[0], reinterpret_cast<void **>(&w)) != napi_ok || !w ||
      !w->ptr || !w->st)
  {
    return out;
  }

  char nameBuf[128] = {};
  size_t nameLen = 0;
  if (napi_get_value_string_utf8(env, argv[1], nameBuf, sizeof(nameBuf), &nameLen) !=
      napi_ok)
  {
    return out;
  }
  double dBytes = 0;
  napi_get_value_double(env, argv[2], &dBytes);
  const size_t byteLen = static_cast<size_t>(dBytes);
  if (byteLen == 0)
    return out;

  // Resolve the member offset by name from the descriptor (same source the
  // member accessors use), then read the pointer field directly.
  const types::StructMember *found = nullptr;
  for (const auto &m : w->st->members) {
    if (std::strcmp(m.name.c_str(), nameBuf) == 0) {
      found = &m;
      break;
    }
  }
  if (!found)
    return out;

  void *dataPtr = *reinterpret_cast<void **>(static_cast<char *>(w->ptr) + found->offset);
  if (!dataPtr)
    return out;

  if (argc >= 4) {
    double dOff = 0;
    if (napi_get_value_double(env, argv[3], &dOff) == napi_ok && dOff > 0) {
      dataPtr = static_cast<char *>(dataPtr) + static_cast<size_t>(dOff);
    }
  }

  napi_value ab;
  napi_status st =
      napi_create_external_arraybuffer(env, dataPtr, byteLen, nullptr, nullptr, &ab);
  if (st != napi_ok) {
    bool pending = false;
    napi_is_exception_pending(env, &pending);
    if (pending) {
      napi_value e;
      napi_get_and_clear_last_exception(env, &e);
    }
    void *abData = nullptr;
    napi_create_arraybuffer(env, byteLen, &abData, &ab);
    if (abData)
      std::memcpy(abData, dataPtr, byteLen);
  }
  napi_create_typedarray(env, napi_uint8_array, byteLen, ab, 0, &out);
  return out;
}

// objectAddress(boundObj) -> the wrapped C++ object's address as a JS number, an
// *opaque identity key* only (e.g. gpuExecutor's per-Buffer GL-buffer cache).
// It is never dereferenced in JS; the address is < 2^48 on win64 so a double is
// exact. WASM uses the numeric `.ptr` for the same purpose.
napi_value NapiRuntime::ObjectAddress(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);
  if (argc < 1)
    return out;
  Wrapped *w = nullptr;
  if (napi_unwrap(env, argv[0], reinterpret_cast<void **>(&w)) != napi_ok || !w)
    return out;
  napi_create_double(env, static_cast<double>(reinterpret_cast<uintptr_t>(w->ptr)), &out);
  return out;
}

// vectorGet(vec, i) — i-th element as a bound value/wrapper, via getBoundPointer
// on the element's storage. Enables iteration of a bound Vector (what the
// getBoundVector use site in sculptcore_ops needs).
napi_value NapiRuntime::VectorGet(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  void *data;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);
  napi_value out;
  Wrapped *w = unwrapVector(env, argc, argv);
  if (!w) {
    napi_get_undefined(env, &out);
    return out;
  }
  int32_t i = -1;
  if (argc >= 2)
    napi_get_value_int32(env, argv[1], &i);
  void *dataPtr =
      *reinterpret_cast<void **>(static_cast<char *>(w->ptr) + kVecDataOffset);
  size_t count =
      *reinterpret_cast<size_t *>(static_cast<char *>(w->ptr) + kVecSizeOffset);
  const binding::BindingBase *elem = w->st->templateParams[0].type;
  size_t elemSize = elem ? elem->getSize() : 0;
  if (i < 0 || static_cast<size_t>(i) >= count || !dataPtr || elemSize == 0) {
    napi_get_undefined(env, &out);
    return out;
  }
  void *elemAddr = static_cast<char *>(dataPtr) + static_cast<size_t>(i) * elemSize;
  return rt->getBoundPointer(elem, elemAddr);
}

// intVectorAssign(vec, data) — replace a Vector<int>'s contents from a JS
// array. The JS->C++ direction the seam otherwise lacks: bound Vector params
// are out-params only, so an app that computed an index set had no way to hand
// it back. Delegates to binding.cc's IntVector_assign so the Vector's own
// clear/reserve/append run -- writing the size field from here would overrun
// capacity and trample its inline storage.
napi_value NapiRuntime::IntVectorAssign(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);

  Wrapped *w = unwrapVector(env, argc, argv);
  if (!w || argc < 2) {
    return out;
  }

  bool isArray = false;
  napi_is_array(env, argv[1], &isArray);
  if (!isArray) {
    return out;
  }

  uint32_t count = 0;
  napi_get_array_length(env, argv[1], &count);

  litestl::util::Vector<int> scratch;
  scratch.ensure_capacity(size_t(count));
  for (uint32_t i = 0; i < count; i++) {
    napi_value elem;
    int32_t v = 0;
    if (napi_get_element(env, argv[1], i, &elem) == napi_ok) {
      napi_get_value_int32(env, elem, &v);
    }
    scratch.append(v);
  }

  IntVector_assign(reinterpret_cast<litestl::util::Vector<int> *>(w->ptr),
                   count ? &scratch[0] : nullptr,
                   int(count));
  return out;
}

// floatVectorAssign(vec, data) — IntVectorAssign for Vector<float>. Same
// delegate-to-binding.cc discipline, so the Vector's own clear/reserve/append
// run rather than the size field being written from here.
napi_value NapiRuntime::FloatVectorAssign(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);

  Wrapped *w = unwrapVector(env, argc, argv);
  if (!w || argc < 2) {
    return out;
  }

  bool isArray = false;
  napi_is_array(env, argv[1], &isArray);
  if (!isArray) {
    return out;
  }

  uint32_t count = 0;
  napi_get_array_length(env, argv[1], &count);

  litestl::util::Vector<float> scratch;
  scratch.ensure_capacity(size_t(count));
  for (uint32_t i = 0; i < count; i++) {
    napi_value el;
    napi_get_element(env, argv[1], i, &el);
    double v = 0.0;
    napi_get_value_double(env, el, &v);
    scratch.append(float(v));
  }

  FloatVector_assign(reinterpret_cast<litestl::util::Vector<float> *>(w->ptr),
                     count ? &scratch[0] : nullptr,
                     int(count));
  return out;
}

} // namespace sculptcore::napi
