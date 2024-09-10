#pragma once

#include "litestl/util/compiler_util.h"

#include <type_traits>

using namespace litestl;
namespace sculptcore::mesh {
enum class AttrType {
  NONE = 0,
  FLOAT = 1 << 0,
  FLOAT2 = 1 << 1,
  FLOAT3 = 1 << 2,
  FLOAT4 = 1 << 3,
  BOOL = 1 << 4,
  INT = 1 << 5,
  INT2 = 1 << 6,
  INT3 = 1 << 7,
  INT4 = 1 << 8,
  BYTE = 1 << 9,
  SHORT = 1 << 10,
};
FlagOperators(AttrType);

enum class _AttrFlag {
  NONE = 0,
  TOPO = 1 << 0,
  TEMP = 1 << 1,
  NOCOPY = 1 << 2,
  NOINTERP = 1 << 3,
};
FlagClass(AttrFlag, _AttrFlag);

#define ATTR_PAGESIZE 4096
#define ATTR_PAGEMASK 4095
#define ATTR_PAGESHIFT 12

} // namespace sculptcore::mesh
