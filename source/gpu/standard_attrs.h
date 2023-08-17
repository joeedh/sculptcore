#pragma once
#include "types.h"
#include "util/string.h"

namespace sculptcore::gpu {
template <GPUValueType type_, int elems_> struct StdAttr {
  int elems = elems_;
  GPUValueType type = type;
};

struct StdAttrs {
  StdAttr<GPU_FLOAT, 3> pos;
  StdAttr<GPU_FLOAT, 3> nor;
};

/* Simple compile-time spell checking. */
#define GPU_STD_ATTR_NAME(name) (offsetof(StdAttrs, name), #name)

} // namespace sculptcore::gpu