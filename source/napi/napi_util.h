#pragma once

// Small helpers shared by the napi_*.cc translation units that marshal
// values between JS and the litestl::binding descriptors (member/array
// accessors in napi_object_model.cc, method/constructor argument marshalling
// in napi_introspection.cc, typed-array element mapping in
// napi_vector_bridge.cc).

#include <node_api.h>

#include "litestl/binding/binding_base.h"
#include "litestl/binding/binding_number.h"
#include "napi_runtime.h"

namespace sculptcore::napi {

// All Number<T> instantiations share an identical layout (the template param
// is only a `using`, never a stored field), so subtype/flags can be read
// through any one of them.
const litestl::binding::types::Number<char> *asNumber(const litestl::binding::BindingBase *b);
bool isUnsigned(const litestl::binding::types::Number<char> *n);

// Read a JS value as the C++ number described by `num` and write it to `dst`
// (which must point at storage of the matching size). Shared by member
// setters and method-argument marshalling.
void writeNumberValue(napi_env env,
                      napi_value v,
                      const litestl::binding::types::Number<char> *num,
                      void *dst);

// Extract the raw C++ pointer behind a bound JS wrapper (or null for
// null/undefined / non-wrapped values).
void *unwrapPtr(napi_env env, napi_value v);

} // namespace sculptcore::napi
