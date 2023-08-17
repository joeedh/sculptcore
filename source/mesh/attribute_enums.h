#pragma once

#include "util/compiler_util.h"

namespace sculptcore::mesh {
enum AttrType {
  ATTR_NONE = 0,
  ATTR_FLOAT = 1 << 0,
  ATTR_FLOAT2 = 1 << 1,
  ATTR_FLOAT3 = 1 << 2,
  ATTR_FLOAT4 = 1 << 3,
  ATTR_BOOL = 1 << 4,
  ATTR_INT = 1 << 5,
  ATTR_INT2 = 1 << 6,
  ATTR_INT3 = 1 << 7,
  ATTR_INT4 = 1 << 8,
  ATTR_BYTE = 1 << 9,
  ATTR_SHORT = 1 << 10,
};
FlagOperators(AttrType);

enum AttrFlags {
  ATTR_FLAG_NONE = 0,
  ATTR_TOPO = 1 << 0,
  ATTR_TEMP = 1 << 1,
  ATTR_NOCOPY = 1 << 2,
  ATTR_NOINTERP = 1 << 3,
};
FlagOperators(AttrFlags);

#define ATTR_PAGESIZE 4096
#define ATTR_PAGEMASK 4095
#define ATTR_PAGESHIFT 12

} // namespace sculptcore::mesh
