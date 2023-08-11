#pragma once

#define ELEM_NONE -1

#include "util/compiler_util.h"

namespace sculptcore::mesh {
enum ElemType {
  VERTEX = 1 << 0,
  EDGE = 1 << 1,
  CORNER = 1 << 2,
  LIST = 1 << 3,
  FACE = 1 << 4,
};
FlagOperators(ElemType);

template <ElemType elem_type> struct ElemRef {
  int i = -1;

  ElemRef()
  {
  }

  ElemRef(int i_) : i(i_)
  {
  }

  operator int()
  {
    return i;
  }
};

using VertRef = ElemRef<VERTEX>;
using EdgeRef = ElemRef<EDGE>;
using CornerRef = ElemRef<CORNER>;
using FaceRef = ElemRef<FACE>;

} // namespace sculptcore::mesh
