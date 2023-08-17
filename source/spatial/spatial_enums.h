#pragma once
#include "util/compiler_util.h"

namespace sculptcore::spatial {
enum NodeFlags {
  Spatial_None = 0,
  Spatial_Leaf = 1 << 0,
  Spatial_RegenBounds = 1 << 1,
  Spatial_RegenTris = 1 << 2,
  Spatial_RegenGPU = 1 << 3,
  Spatial_UpdateGPU = 1 << 4,
};
FlagOperators(NodeFlags);

} // namespace sculptcore::spatial
