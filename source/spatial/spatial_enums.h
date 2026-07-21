#pragma once
#include "litestl/binding/binding_enum.h"
#include "litestl/util/compiler_util.h"

namespace sculptcore::spatial {
enum _NodeFlags {
  Spatial_None = 0,
  Spatial_Leaf = 1 << 0,          // 1
  Spatial_RegenBounds = 1 << 1,   // 2
  Spatial_RegenTris = 1 << 2,     // 4
  Spatial_RegenGPU = 1 << 3,      // 8
  Spatial_UpdateGPU = 1 << 4,     // 16
  Spatial_UpdateNormals = 1 << 5, // 32
  /* Geometry-only GPU refresh: the leaf's pos/nor streams are stale but its
   * attribute streams (color/mask/group/...) are not. Set by pure-deform brush
   * kernels; the slice update skips the attr-stream fills. Spatial_UpdateGPU
   * still means "refresh every stream". */
  Spatial_UpdateGPUGeom = 1 << 6, // 64
  /* The next normals pass must do a FULL leaf rebuild: the leaf's tris were
   * regenerated (topology changed), invalidating — and clearing — the
   * affected_verts hints. Sticky until update_node_normals consumes it, so
   * hints appended between the tris regen and a deferred normals pass (the
   * updateQueries()/update(gpu) split) can't downgrade it to an incremental
   * pass that misses pre-regen motion. */
  Spatial_NormalsFullRebuild = 1 << 7, // 128
};
MAKE_FLAGS_CLASS(NodeFlags, _NodeFlags, int);

/* Values of the FACE-domain `.detail.carrier` tag: which detail carrier owns
 * a face (dyntopo-vdm-region-hybrid.md §3). GEOM = live geometry (default),
 * VDM = UV-keyed vector-displacement texels; the dab loop routes on it. */
enum class DetailCarrier { GEOM = 0, VDM = 1 };

} // namespace sculptcore::spatial

namespace litestl::binding {
template <> struct Binder<sculptcore::spatial::NodeFlags> {
  static const BindingBase *bind()
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
};
} // namespace litestl::binding
