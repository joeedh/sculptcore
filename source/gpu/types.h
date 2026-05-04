#pragma once

#include "litestl/util/compiler_util.h"

#include "litestl/binding/binding_base.h"
#include "litestl/binding/binding_enum.h"
#include "litestl/math/matrix.h"
#include "litestl/math/vector.h"

#include <type_traits>

namespace sculptcore::gpu {
enum class _GPUCmdType {
  DRAW_TRIS = 0,      //
  DRAW_TRI_STRIP = 1, //
  DRAW_LINES = 2,
  DRAW_POINTS = 3
};
MAKE_ENUM_CLASS(GPUCmdType, _GPUCmdType, uint32_t);

enum class _GPUType {
  TYPE_INVALID = -1,
  FLOAT16,
  FLOAT32,
  FLOAT64,
  INT32,
  INT16,
  INT8,
  UINT32,
  UINT16,
  UINT8,
};
MAKE_ENUM_CLASS(GPUType, _GPUType, uint32_t);

template <typename T> static constexpr GPUType gpu_type_from()
{
  using namespace litestl::math;

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

  GPU_TYPE_FROM_DEF(float, GPUType::FLOAT32);
  GPU_TYPE_FROM_DEF(double, GPUType::FLOAT16);
  GPU_TYPE_FROM_DEF(int, GPUType::INT32);
  GPU_TYPE_FROM_DEF(uint, GPUType::UINT32);
  GPU_TYPE_FROM_DEF(short, GPUType::INT16);
  GPU_TYPE_FROM_DEF(ushort, GPUType::UINT16);
  GPU_TYPE_FROM_DEF(char, GPUType::INT8);
  GPU_TYPE_FROM_DEF(uchar, GPUType::UINT8);

#undef GPU_TYPE_FROM_DEF
  return GPUType::TYPE_INVALID;
}

constexpr int gpu_sizeof(GPUType type)
{
  switch (type) {
  case GPUType::TYPE_INVALID:
    return 0;
  case GPUType::FLOAT32:
    return 4;
  case GPUType::INT32:
  case GPUType::UINT32:
    return 4;
  case GPUType::INT16:
  case GPUType::UINT16:
  case GPUType::FLOAT16:
    return 2;
  case GPUType::INT8:
  case GPUType::UINT8:
    return 2;
  case GPUType::FLOAT64:
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

enum class GPUFetchMode {
  FETCH_NONE = 0,
  FETCH_FLOAT = 1,
};

static const litestl::binding::BindingBase *Bind(GPUCmdType *)
{
  using namespace sculptcore::gpu;
  using namespace litestl::binding;

  types::Enum *e = new types::Enum("sculptcore::gpu::GPUCmdType", sizeof(GPUCmdType));
  e->addItem("DRAW_TRIS", GPUCmdType::DRAW_TRIS);
  e->addItem("DRAW_TRI_STRIP", GPUCmdType::DRAW_TRI_STRIP);
  e->addItem("DRAW_LINES", GPUCmdType::DRAW_LINES);
  e->addItem("DRAW_POINTS", GPUCmdType::DRAW_POINTS);
  return e;
}

static const litestl::binding::BindingBase *Bind(GPUType *)
{
  using namespace litestl::binding;
  using namespace sculptcore::gpu;
  types::Enum *e = new types::Enum("sculptcore::gpu::GPUType", sizeof(GPUType));

  e->addItem("TYPE_INVALID", GPUType::TYPE_INVALID);
  e->addItem("FLOAT16", GPUType::FLOAT16);
  e->addItem("FLOAT32", GPUType::FLOAT32);
  e->addItem("FLOAT64", GPUType::FLOAT64);
  e->addItem("INT32", GPUType::INT32);
  e->addItem("INT16", GPUType::INT16);
  e->addItem("INT8", GPUType::INT8);
  e->addItem("UINT32", GPUType::UINT32);
  e->addItem("UINT16", GPUType::UINT16);
  e->addItem("UINT8", GPUType::UINT8);
  return e;
}

static const litestl::binding::BindingBase *Bind(sculptcore::gpu::GPUFetchMode *)
{
  using namespace sculptcore::gpu;
  using namespace litestl::binding;
  types::Enum *e = new types::Enum("sculptcore::gpu::GPUFetchMode", sizeof(GPUFetchMode));

  e->addItem("FETCH_NONE", GPUFetchMode::FETCH_NONE);
  e->addItem("FETCH_FLOAT", GPUFetchMode::FETCH_FLOAT);
  return e;
}
} // namespace sculptcore::gpu
