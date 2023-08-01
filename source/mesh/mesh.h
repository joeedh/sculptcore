#pragma once

#include "attribute.h"
#include "attribute_builtin.h"
#include "math/vector.h"
#include "mesh_base.h"
#include "util/string.h"

#include <algorithm>
#include <concepts>
#include <cstdio>
#include <type_traits>

namespace sculptcore::mesh {

struct ElemData {
  ElemType domain;
  AttrGroup attrs;
  int count;

  ElemData(ElemType domain_, int count_) : domain(domain_), count(count_)
  {
  }
};

struct VertexData : protected ElemData {
  VertexData(int count_) : ElemData(VERTEX, count_)
  {
    co.ensure(attrs, count);
    select.ensure(attrs, count);
    // no.ensure(attrs, count);
  }

  BuiltinAttr<math::float3, "positions"> co;
  // BuiltinAttr<math::float3, "normals"> no;

  BuiltinAttr<bool, "select"> select;

  /* Topology attributes. */
  // BuiltinAttr<int, "e"> e;
  // BuiltinAttr<int, "l"> l;
};

struct Mesh {
  VertexData v;

  Mesh(int nv, int ne, int nc, int nf) : v(nv)
  {
  }
};
}; // namespace sculptcore::mesh
