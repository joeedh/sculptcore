#pragma once

#define ELEM_NONE -1

namespace sculptcore::mesh {
enum ElemType {
  VERTEX = 1 << 0,
  EDGE = 1 << 1,
  CORNER = 1 << 2,
  LIST = 1 << 3,
  FACE = 1 << 4,
};
} // namespace sculptcore::mesh
