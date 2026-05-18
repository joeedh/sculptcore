#pragma once

#include "attribute_enums.h"
#include "litestl/math/vector.h"
#include "litestl/util/string.h"
#include <type_traits>

using namespace litestl;

namespace sculptcore::mesh {

using util::string;

template <typename T> static constexpr AttrType type_to_attrtype()
{
  using namespace litestl::math;

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
  int elemSize;

  AttrDataBase()
  {
  }

  AttrDataBase(AttrType type_, const string &name_, int elemSize_)
      : type(type_), name(name_), elemSize(elemSize_)
  {
  }

  virtual AttrDataBase &operator=(AttrDataBase &&b)
  {
    if (this == &b) {
      return *this;
    }
    type = b.type;
    name = std::move(b.name);
    elemSize = b.elemSize;

    return *this;
  }

  virtual ~AttrDataBase()
  {
  }

  virtual void *getElemData(int i)
  {
    fprintf(stderr, "default getElemData called in AttrDataBase class\n");
    abort();
    return nullptr;
  }
  virtual const void *getElemData(int i) const
  {
    fprintf(stderr, "default getElemData called in AttrDataBase class\n");
    abort();
    return nullptr;
  }
};

} // namespace sculptcore::mesh
