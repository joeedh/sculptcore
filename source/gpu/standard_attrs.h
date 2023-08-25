#pragma once
#include "types.h"
#include "util/string.h"

namespace sculptcore::gpu {
template <GPUType type_, int elems_> struct StdAttr {
  int elems = elems_;
  GPUType type = type;
};

struct StdAttrs {
  StdAttr<GPUType::FLOAT32, 3> pos;
  StdAttr<GPUType::FLOAT32, 3> nor;
};

/* Simple compile-time spell checking. */
#define GPU_STD_ATTR_NAME(name) (offsetof(StdAttrs, name), #name)

} // namespace sculptcore::gpu