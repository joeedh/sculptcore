#pragma once

#include "util/compiler_util.h"

namespace sculptcore::props {
enum PropType {
  PROP_INVALID_TYPE = -1,
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
  PROP_BOOL = 10,

  PROP_STRING = 11,
  PROP_STATIC_STRING = 12,

  PROP_LIST = 13,
  PROP_STRUCT = 14,
  PROP_STRUCT_PTR = 15,
  PROP_ARRAYBUFFER = 16,
  PROP_VEC2F = 17,
  PROP_VEC3F = 18,
  PROP_VEC4F = 19,
  PROP_MATRIX4F = 20,
  PROP_CURVE2D = 21,
};

enum PropFlags {
  PROP_FLAG_NONE = 0,
  PROP_FLAG_READ_ONLY = 1 << 0,
};
FlagOperators(PropFlags);

enum PropError {
  PROP_ERROR_NONE = 0,
  PROP_ERROR_INVALID_TYPE = 1 << 0,
  PROP_ERROR_NOT_EXISTS = 1 << 1,
};

} // namespace sculptcore::props