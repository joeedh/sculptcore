#pragma once

#include "util/compiler_util.h"

namespace sculptcore::props {
enum class Prop {
  INVALID_TYPE = -1,
  FLOAT32 = 0,
  FLOAT64 = 1,
  INT64 = 2,
  UINT64 = 3,
  INT32 = 4,
  UINT32 = 5,
  INT16 = 6,
  UINT16 = 7,
  INT8 = 8,
  UINT8 = 9,
  BOOL = 10,

  STRING = 11,
  STATIC_STRING = 12,

  LIST = 13,
  STRUCT = 14,
  STRUCT_PTR = 15,
  ARRAYBUFFER = 16,
  VEC2F = 17,
  VEC3F = 18,
  VEC4F = 19,
  MATRIX4F = 20,
  CURVE2D = 21,
};

enum class PropFlag {
  NONE = 0,
  READ_ONLY = 1 << 0,
  INHERIT_PARENT = 1 << 1,
  INHERIT_UNIFIED = 1 << 2
};
FlagOperators(PropFlag);

enum class PropError {
  ERROR_NONE = 0,
  ERROR_INVALID_TYPE = 1 << 0,
  ERROR_NOT_EXISTS = 1 << 1,
};

FlagOperators(PropError);

} // namespace sculptcore::props