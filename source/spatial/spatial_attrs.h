#pragma once

#include "node.h"

#include "litestl/math/vector.h"
#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "mesh/mesh_proxy.h"

using namespace litestl;
using namespace sculptcore::mesh;
using namespace litestl::math;

namespace sculptcore::spatial {
struct SpatialTreeMesh {
  Mesh *m = nullptr;
  struct {
    // Per-tree node ownership: derived state that is rebuilt when a SpatialTree
    // is constructed over the mesh. Must NOT be serialized — a stale index baked
    // into a saved mesh makes a freshly-built tree mis-partition (the
    // ownership-attribute pitfall; see documentation/spatial.md). TEMP drops it
    // from writeMesh so a deserialized mesh starts clean.
    BuiltinAttr<int, ".spatial.v.node", AttrFlag::TEMP> node;
    BuiltinAttr<float, ".spatial.v.mask"> mask;

    void setup(Mesh *m)
    {
      node.ensure(m->v.attrs);
      mask.ensure(m->v.attrs);
    }
  } v;

  struct {
    // Per-tree face ownership — derived, non-persistent (see v.node above).
    BuiltinAttr<int, ".spatial.f.node", AttrFlag::TEMP> node;

    void setup(Mesh *m)
    {
      node.ensure(m->f.attrs);
    }
  } f;

  void setup(Mesh *m)
  {
    this->m = m;
    
    v.setup(m);
    f.setup(m);
  }
};
} // namespace sculptcore::spatial
