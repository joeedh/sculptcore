#pragma once

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
  ATTR_SHORT = 1 << 10
};

#define ATTR_PAGESIZE 4096
#define ATTR_PAGEMASK 4095
#define ATTR_PAGESHIFT 12

} // namespace sculptcore::mesh
