#pragma once

#include "litestl/binding/binding.h"
#include "litestl/util/compiler_util.h"

#include <type_traits>

using namespace litestl;
namespace sculptcore::mesh {
enum class AttrType {
  NONE = 0,
  FLOAT = 1 << 0,
  FLOAT2 = 1 << 1,
  FLOAT3 = 1 << 2,
  FLOAT4 = 1 << 3,
  BOOL = 1 << 4,
  INT = 1 << 5,
  INT2 = 1 << 6,
  INT3 = 1 << 7,
  INT4 = 1 << 8,
  BYTE = 1 << 9,
  SHORT = 1 << 10,
};
FlagOperators(AttrType);

enum class _AttrFlag {
  NONE = 0,
  TOPO = 1 << 0,
  TEMP = 1 << 1,
  NOCOPY = 1 << 2,
  NOINTERP = 1 << 3,
  /* A TOPO column that the per-frame spatial/render path reads directly, so it
   * must stay materialized in frozen-topology mode rather than being dropped
   * with the pure-iteration links. Currently only .corner.v (triangle vert
   * lookup for bounds/normals/GPU upload). */
  TOPO_KEEP_FROZEN = 1 << 4,
};
MAKE_FLAGS_CLASS(AttrFlag, _AttrFlag, int);

/**
 * Usage flags.
 * UNIT means attribute has unit values (0-1)
 * (which are mapped for numeric types, e.g. a
 * (unsigned byte attr is maps 0-255 to 0-1 in shaders,
 * (a signed byte attr maps to -128-128 to -1-1 in shaders).
 *
 * A UV map is a float2 attr with UV set
 */
enum class _AttrUse {
  NONE = 0,
  UNIT = 1 << 0,      /** attribute has unit values (0-1) */
  COLOR = 1 << 1,     /** for colors */
  UV = 1 << 2,        /** for uv-maps */
  POLYGROUP = 1 << 3, /** per-face poly-group id */
};
MAKE_FLAGS_CLASS(AttrUse, _AttrUse, int);

#define ATTR_PAGESHIFT 12
#define ATTR_PAGESIZE (1 << ATTR_PAGESHIFT)
#define ATTR_PAGEMASK (ATTR_PAGESIZE - 1)

} // namespace sculptcore::mesh

namespace litestl::binding {

template <> struct Binder<sculptcore::mesh::AttrUse> {
  static const types::Enum *bind()
  {
    using namespace sculptcore::mesh;
    types::Enum *e = new types::Enum("sculptcore::mesh::AttrUse", sizeof(AttrUse));
    e->isBitMask = true;
    e->addItem("None", static_cast<int>(AttrUse::NONE));
    e->addItem("Unit", static_cast<int>(AttrUse::UNIT));
    e->addItem("Color", static_cast<int>(AttrUse::COLOR));
    e->addItem("UV", static_cast<int>(AttrUse::UV));
    e->addItem("PolyGroup", static_cast<int>(AttrUse::POLYGROUP));
    return e;
  }
};

template <> struct Binder<sculptcore::mesh::AttrFlag> {
  static const types::Enum *bind()
  {
    using namespace sculptcore::mesh;
    types::Enum *e = new types::Enum("sculptcore::mesh::AttrFlag", sizeof(AttrFlag));
    e->isBitMask = true;
    e->addItem("None", static_cast<int>(AttrFlag::NONE));
    e->addItem("Topo", static_cast<int>(AttrFlag::TOPO));
    e->addItem("Temp", static_cast<int>(AttrFlag::TEMP));
    e->addItem("NoCopy", static_cast<int>(AttrFlag::NOCOPY));
    e->addItem("NoInterp", static_cast<int>(AttrFlag::NOINTERP));
    e->addItem("TopoKeepFrozen", static_cast<int>(AttrFlag::TOPO_KEEP_FROZEN));
    return e;
  }
};

template <> struct Binder<sculptcore::mesh::AttrType> {
  static const types::Enum *bind()
  {
    using namespace sculptcore::mesh;
    types::Enum *e = new types::Enum("sculptcore::mesh::AttrType", sizeof(AttrType));
    e->addItem("Float", static_cast<int>(AttrType::NONE));
    e->addItem("Int", static_cast<int>(AttrType::INT));
    e->addItem("Vec2", static_cast<int>(AttrType::FLOAT));
    e->addItem("Float2", static_cast<int>(AttrType::FLOAT2));
    e->addItem("Float3", static_cast<int>(AttrType::FLOAT3));
    e->addItem("Float4", static_cast<int>(AttrType::FLOAT4));
    e->addItem("Bool", static_cast<int>(AttrType::BOOL));
    e->addItem("Byte", static_cast<int>(AttrType::BYTE));
    e->addItem("Short", static_cast<int>(AttrType::SHORT));
    e->addItem("Int2", static_cast<int>(AttrType::INT2));
    e->addItem("Int3", static_cast<int>(AttrType::INT3));
    e->addItem("Int4", static_cast<int>(AttrType::INT4));
    return e;
  }
};

} // namespace litestl::binding