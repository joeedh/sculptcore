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
  /** Sparse deform weights: a WeightSlot index into Mesh's DeformPool. */
  WEIGHTS = 1 << 11,
};
FlagOperators(AttrType);

/**
 * A vertex's deform-weight run, stored as an index into the mesh-owned
 * DeformPool (mesh/deform_pool.h) rather than as a value. Slot 0 is the
 * canonical empty run and is never freed, so a default-constructed (or
 * zero-filled) slot already means "no weights".
 *
 * It is a distinct type from int on purpose. The column is bit-identical to an
 * int column — which is what lets the meshlog memcpy it and the serializer
 * write it as raw bytes — but the type makes every generic attribute path
 * dispatch to WeightSlot instead of silently treating a pool index as a number
 * it may copy, lerp or upload.
 */
struct WeightSlot {
  int32_t index = 0;

  WeightSlot() = default;
  explicit WeightSlot(int32_t index_) : index(index_)
  {
  }

  bool operator==(const WeightSlot &b) const
  {
    return index == b.index;
  }
  bool operator!=(const WeightSlot &b) const
  {
    return index != b.index;
  }
};

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
  /* Recomputable from the authoritative columns, so serial::writeMesh drops it
   * (like TEMP) and serial::readMesh rebuilds it — normals, the ngon counts, and
   * the radial-edge link columns. See mesh_serialize.cc / rebuildDerivedTopo. */
  DERIVED = 1 << 5,
};
MAKE_FLAGS_CLASS(AttrFlag, _AttrFlag, int);

/**
 * How a layer produces the value of an element a topological operator creates or
 * merges from two sources (an edge split's midpoint, an edge collapse's
 * survivor). Not a bitmask — one policy per layer.
 *
 * Stamped by name when the layer is created (`resolveMergePolicy`, attr_merge.cc),
 * so it costs nothing in the file format and is re-derived on load.
 */
enum class AttrMerge : uint8_t {
  /** Lerp float-backed columns; copy src0 for int/bool/byte/short. */
  DEFAULT = 0,
  /** Snap to the nearest source — copy src0 even for float-backed columns. */
  COPY_SRC0,
  /** Leave dst untouched. What AttrFlag::NOINTERP means, as a policy. */
  NONE,
  /** Dispatch through AttrRef::merge_fn (falls back to DEFAULT if unset). */
  CUSTOM,
};

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
  SELECT = 1 << 4,    /** per-element selection bool (box-modeling) */
  SCULPT_LAYER =
      1 << 5, /** float3 vertex sculpt-layer delta (see mesh/sculpt_layers.h) */
  DEFORM_WEIGHTS = 1 << 6, /** vertex-group weights (see mesh/deform_pool.h) */
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
    e->addItem("Select", static_cast<int>(AttrUse::SELECT));
    e->addItem("SculptLayer", static_cast<int>(AttrUse::SCULPT_LAYER));
    e->addItem("DeformWeights", static_cast<int>(AttrUse::DEFORM_WEIGHTS));
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
    e->addItem("Derived", static_cast<int>(AttrFlag::DERIVED));
    return e;
  }
};

template <> struct Binder<sculptcore::mesh::AttrMerge> {
  static const types::Enum *bind()
  {
    using namespace sculptcore::mesh;
    types::Enum *e = new types::Enum("sculptcore::mesh::AttrMerge", sizeof(AttrMerge));
    e->addItem("Default", static_cast<int>(AttrMerge::DEFAULT));
    e->addItem("CopySrc0", static_cast<int>(AttrMerge::COPY_SRC0));
    e->addItem("None", static_cast<int>(AttrMerge::NONE));
    e->addItem("Custom", static_cast<int>(AttrMerge::CUSTOM));
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
    e->addItem("Weights", static_cast<int>(AttrType::WEIGHTS));
    return e;
  }
};

} // namespace litestl::binding