#pragma once

#include "util/compiler_util.h"

#include "math/matrix.h"
#include "math/vector.h"
#include "opengl.h"

#include <type_traits>

namespace sculptcore::gpu {
enum GPUValueType {
  GPU_TYPE_INVALID = -1,
  GPU_FLOAT = GL_FLOAT,
  GPU_INT32 = GL_INT,
  GPU_INT16 = GL_SHORT,
  GPU_INT8 = GL_BYTE,
  GPU_UINT32 = GL_UNSIGNED_INT,
  GPU_UINT16 = GL_UNSIGNED_SHORT,
  GPU_UINT8 = GL_UNSIGNED_BYTE,
  GPU_DOUBLE = GL_DOUBLE,
  GPU_HALF = GL_HALF_FLOAT
};

template <typename T> static constexpr GPUValueType gpu_type_from()
{
  using namespace sculptcore::math;

#ifdef GPU_TYPE_FROM_DEF
#undef GPU_TYPE_FROM_DEF
#endif

#define GPU_TYPE_FROM_DEF(Base, Type)                                                    \
  if constexpr (std::is_same_v<T, Base> || std::is_same_v<T, Base##1> ||                 \
                std::is_same_v<T, Base##2> || std::is_same_v<T, Base##3> ||              \
                std::is_same_v<T, Base##4>)                                              \
  {                                                                                      \
    return Type;                                                                         \
  }

  GPU_TYPE_FROM_DEF(float, GPU_FLOAT);
  GPU_TYPE_FROM_DEF(int, GPU_INT32);
  GPU_TYPE_FROM_DEF(uint, GPU_UINT32);
  GPU_TYPE_FROM_DEF(short, GPU_INT16);
  GPU_TYPE_FROM_DEF(ushort, GPU_UINT16);
  GPU_TYPE_FROM_DEF(char, GPU_INT8);
  GPU_TYPE_FROM_DEF(uchar, GPU_UINT8);

#undef GPU_TYPE_FROM_DEF
  return GPU_TYPE_INVALID;
}

constexpr int gpu_sizeof(GPUValueType type)
{
  switch (type) {
  case GPU_FLOAT:
    return 4;
  case GPU_INT32:
    return 4;
  case GPU_INT16:
  case GPU_UINT16:
  case GPU_HALF:
    return 2;
  case GPU_INT8:
  case GPU_UINT8:
    return 2;
  case GPU_DOUBLE:
    return 8;
  }

  return 0;
}

template <typename T> constexpr int gpu_base_type_sizeof()
{
  return gpu_sizeof(gpu_type_from<T>());
}

template <typename T> constexpr int gpu_type_elems()
{
  return sizeof(T) / gpu_base_type_sizeof<T>();
}

enum GPUValueMode {
  GPU_FETCH_NONE = 0,
  GPU_FETCH_FLOAT = 1,
};
} // namespace sculptcore::gpu
