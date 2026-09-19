#include "napi_util.h"

namespace sculptcore::napi {

using binding::NumberFlags;
using binding::NumberType;

const types::Number<char> *asNumber(const binding::BindingBase *b)
{
  return reinterpret_cast<const types::Number<char> *>(b);
}

bool isUnsigned(const types::Number<char> *n)
{
  return (static_cast<int>(n->flags) & static_cast<int>(NumberFlags::Unsigned)) != 0;
}

void writeNumberValue(napi_env env,
                      napi_value v,
                      const types::Number<char> *num,
                      void *dst)
{
  const bool uns = isUnsigned(num);
  if (num->subtype == NumberType::Int64) {
    bool lossless = false;
    if (uns) {
      uint64_t x = 0;
      napi_get_value_bigint_uint64(env, v, &x, &lossless);
      *static_cast<uint64_t *>(dst) = x;
    } else {
      int64_t x = 0;
      napi_get_value_bigint_int64(env, v, &x, &lossless);
      *static_cast<int64_t *>(dst) = x;
    }
    return;
  }
  double d = 0.0;
  napi_get_value_double(env, v, &d);
  switch (num->subtype) {
  case NumberType::Int8:
    uns ? (*static_cast<uint8_t *>(dst) = static_cast<uint8_t>(d))
        : (*static_cast<int8_t *>(dst) = static_cast<int8_t>(d));
    break;
  case NumberType::Int16:
    uns ? (*static_cast<uint16_t *>(dst) = static_cast<uint16_t>(d))
        : (*static_cast<int16_t *>(dst) = static_cast<int16_t>(d));
    break;
  case NumberType::Int32:
    uns ? (*static_cast<uint32_t *>(dst) = static_cast<uint32_t>(d))
        : (*static_cast<int32_t *>(dst) = static_cast<int32_t>(d));
    break;
  case NumberType::Float32:
    *static_cast<float *>(dst) = static_cast<float>(d);
    break;
  case NumberType::Float64:
    *static_cast<double *>(dst) = d;
    break;
  case NumberType::Int64:
    break; // handled above
  }
}

void *unwrapPtr(napi_env env, napi_value v)
{
  if (!v)
    return nullptr;
  napi_valuetype t;
  napi_typeof(env, v, &t);
  if (t == napi_null || t == napi_undefined)
    return nullptr;
  Wrapped *w = nullptr;
  if (napi_unwrap(env, v, reinterpret_cast<void **>(&w)) == napi_ok && w) {
    return w->ptr;
  }
  return nullptr;
}

} // namespace sculptcore::napi
