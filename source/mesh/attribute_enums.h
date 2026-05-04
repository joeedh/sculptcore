#pragma once

#include "litestl/binding/binding.h"
#include "litestl/util/compiler_util.h"

#include <type_traits>

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
};
MAKE_FLAGS_CLASS(AttrFlag, _AttrFlag, int);

#define ATTR_PAGESIZE 4096
#define ATTR_PAGEMASK 4095
#define ATTR_PAGESHIFT 12

static const litestl::binding::types::Enum *Bind(AttrFlag *)
{
  using namespace sculptcore::mesh;
  using namespace litestl::binding;

  types::Enum *e = new types::Enum("sculptcore::mesh::AttrFlag", sizeof(AttrFlag));
  e->isBitMask = true;
  e->addItem("None", static_cast<int>(AttrFlag::NONE));
  e->addItem("Topo", static_cast<int>(AttrFlag::TOPO));
  e->addItem("Temp", static_cast<int>(AttrFlag::TEMP));
  e->addItem("NoCopy", static_cast<int>(AttrFlag::NOCOPY));
  e->addItem("NoInterp", static_cast<int>(AttrFlag::NOINTERP));
  return e;
}

static const litestl::binding::BindingBase *Bind(sculptcore::mesh::AttrType *)
{
  using namespace sculptcore::mesh;
  using namespace litestl::binding;

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

} // namespace sculptcore::mesh