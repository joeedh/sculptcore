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
    BuiltinAttr<int, ".spatial.v.node"> node;
    BuiltinAttr<float, ".spatial.v.mask"> mask;

    void setup(Mesh *m)
    {
      node.ensure(m->v.attrs);
      mask.ensure(m->v.attrs);
    }
  } v;

  struct {
    BuiltinAttr<int, ".spatial.f.node"> node;

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
