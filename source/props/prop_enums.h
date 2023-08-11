#pragma once

#include "util/compiler_util.h"

namespace sculptcore::props {
enum PropType {
  PROP_FLOAT32 = 0,
  PROP_FLOAT64 = 1,
  PROP_INT64 = 2,
  PROP_UINT64 = 3,
  PROP_INT32 = 4,
  PROP_UINT32 = 5,
  PROP_INT16 = 6,
  PROP_UINT16 = 7,
  PROP_INT8 = 8,
  PROP_UINT8 = 9,
  PROP_STRING = 10,
  PROP_BOOL = 11,
  PROP_LIST = 12,
  PROP_STRUCT = 13,
  PROP_STRUCT_PTR = 14,
};

enum PropFlags {
  PROP_FLAG_READ_ONLY = 1 << 0,
};
FlagOperators(PropFlags);

} // namespace sculptcore::props