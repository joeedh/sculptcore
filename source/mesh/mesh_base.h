#pragma once

namespace sculptcore::mesh {
enum ElemType {
  VERTEX = 1 << 0,
  EDGE = 1 << 1,
  CORNER = 1 << 2,
  FACE = 1 << 3,
};
}
