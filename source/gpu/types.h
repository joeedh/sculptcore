#pragma once

#include "opengl.h"

namespace sculptcore::gpu {
enum GPUValueType {
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

enum GPUValueMode {
  GPU_FETCH_NONE = 0,
  GPU_FETCH_FLOAT = 1,
};
} // namespace sculptcore::gpu
