#pragma once
#include "litestl/binding/binding_enum.h"
#include "litestl/util/compiler_util.h"

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

namespace litestl::binding {
template <std::same_as<sculptcore::spatial::NodeFlags> T> static const BindingBase *Bind()
{
  using namespace sculptcore::spatial;
  types::Enum *e = new types::Enum("sculptcore::spatial::NodeFlags", sizeof(NodeFlags));
  e->isBitMask = true;
  e->addItem("Spatial_None", Spatial_None);
  e->addItem("Spatial_Leaf", Spatial_Leaf);
  e->addItem("Spatial_RegenBounds", Spatial_RegenBounds);
  e->addItem("Spatial_RegenTris", Spatial_RegenTris);
  e->addItem("Spatial_RegenGPU", Spatial_RegenGPU);
  e->addItem("Spatial_UpdateGPU", Spatial_UpdateGPU);
  return e;
}
} // namespace litestl::binding
