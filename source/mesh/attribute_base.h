#pragma once

#include "attribute_enums.h"
#include "math/vector.h"
#include "util/alloc.h"
#include "util/string.h"
#include "util/vector.h"

#include <cmath>
#include <type_traits>

namespace sculptcore::mesh {

using util::string;

template <typename T> static constexpr AttrType type_to_attrtype()
{
  using namespace sculptcore::math;

  if constexpr (std::is_same_v<T, float>) {
    return ATTR_FLOAT;
  } else if constexpr (std::is_same_v<T, float2>) {
    return ATTR_FLOAT2;
  } else if constexpr (std::is_same_v<T, float3>) {
    return ATTR_FLOAT3;
  } else if constexpr (std::is_same_v<T, float4>) {
    return ATTR_FLOAT4;
  } else if constexpr (std::is_same_v<T, bool>) {
    return ATTR_BOOL;
  } else if constexpr (std::is_same_v<T, uint8_t>) {
    return ATTR_BYTE;
  } else if constexpr (std::is_same_v<T, int>) {
    return ATTR_INT;
  } else if constexpr (std::is_same_v<T, int2>) {
    return ATTR_INT2;
  }

  return ATTR_NONE;
}

/* Main attribute storage class (except for bools). */
struct AttrDataBase {
  AttrType type;
  string name;

  AttrDataBase()
  {
  }

  AttrDataBase(AttrType type_, const string &name_) : type(type_), name(name_)
  {
  }

  virtual AttrDataBase &operator=(AttrDataBase &&b)
  {
    type = b.type;
    name = std::move(b.name);

    return *this;
  }

  virtual ~AttrDataBase()
  {
  }
};

} // namespace sculptcore::mesh
