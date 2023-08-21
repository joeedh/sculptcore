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
    return AttrType::FLOAT;
  } else if constexpr (std::is_same_v<T, float2>) {
    return AttrType::FLOAT2;
  } else if constexpr (std::is_same_v<T, float3>) {
    return AttrType::FLOAT3;
  } else if constexpr (std::is_same_v<T, float4>) {
    return AttrType::FLOAT4;
  } else if constexpr (std::is_same_v<T, bool>) {
    return AttrType::BOOL;
  } else if constexpr (std::is_same_v<T, uint8_t>) {
    return AttrType::BYTE;
  } else if constexpr (std::is_same_v<T, int>) {
    return AttrType::INT;
  } else if constexpr (std::is_same_v<T, int2>) {
    return AttrType::INT2;
  } else if constexpr (std::is_same_v<T, int3>) {
    return AttrType::INT3;
  } else if constexpr (std::is_same_v<T, int4>) {
    return AttrType::INT4;
  } else if constexpr (std::is_same_v<T, short> || std::is_same_v<T, unsigned short>) {
    return AttrType::SHORT;
  }

  return AttrType::NONE;
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
