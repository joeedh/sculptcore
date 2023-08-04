#pragma once

#include "attribute.h"
#include "attribute_builtin.h"
#include "math/vector.h"
#include "util/boolvector.h"
#include "util/string.h"

#include "elem_data.h"
#include "mesh_base.h"
#include "mesh_enums.h"

#include <algorithm>
#include <concepts>
#include <cstdio>
#include <type_traits>

namespace sculptcore::mesh {
struct VertexData : public ElemData {
  VertexData(int count_ = 0) : ElemData(VERTEX, count_)
  {
    co.ensure(attrs);
    select.ensure(attrs);
    no.ensure(attrs);
    e.ensure(attrs);

    for (int i = 0; i < count_; i++) {
      e[i] = ELEM_NONE;
    }
  }

  BuiltinAttr<math::float3, "positions"> co;
  BuiltinAttr<math::float3, "normals"> no;

  BuiltinAttr<bool, "select"> select;

  /* Topology attributes. */
  BuiltinAttr<int, "e"> e;

  void swap_elems(int v1, int v2);
};

struct EdgeData : public ElemData {
  using int2 = math::int2;
  using int4 = math::int4;

  EdgeData(int count_ = 0) : ElemData(EDGE, count_)
  {
    vs.ensure(attrs);
    disk.ensure(attrs);
    select.ensure(attrs);
    c.ensure(attrs);
  }

  BuiltinAttr<int, ".edge.c"> c;

  BuiltinAttr<bool, "select"> select;

  /* Topology attributes. */
  BuiltinAttr<int2, ".edge.vs"> vs;
  BuiltinAttr<int4, ".edge.vs.disk"> disk;
};

struct CornerData : public ElemData {
  using int2 = math::int2;
  using int4 = math::int4;

  CornerData(int count_ = 0) : ElemData(CORNER, count_)
  {
    v.ensure(attrs);
    e.ensure(attrs);
    l.ensure(attrs);
    next.ensure(attrs);
    prev.ensure(attrs);
    radial_next.ensure(attrs);
    radial_prev.ensure(attrs);
  }

  BuiltinAttr<int, ".corner.v"> v;
  BuiltinAttr<int, ".corner.e"> e;
  BuiltinAttr<int, ".corner.l"> l; /* Owning list. */
  BuiltinAttr<int, ".corner.next"> next;
  BuiltinAttr<int, ".corner.prev"> prev;
  BuiltinAttr<int, ".corner.radial_next"> radial_next;
  BuiltinAttr<int, ".corner.radial_prev"> radial_prev;
};

struct ListData : public ElemData {
  using int2 = math::int2;
  using int4 = math::int4;
  ListData(int count_ = 0) : ElemData(LIST, count_)
  {
    c.ensure(attrs);
    f.ensure(attrs);
    next.ensure(attrs);
    size.ensure(attrs);
  }

  BuiltinAttr<int, ".list.c"> c;
  BuiltinAttr<int, ".list.f"> f;
  BuiltinAttr<int, ".list.next"> next;
  BuiltinAttr<int, ".list.size"> size;
};

struct FaceData : public ElemData {
  using int2 = math::int2;
  using int4 = math::int4;
  using float3 = math::float3;

  FaceData(int count_ = 0) : ElemData(FACE, count_)
  {
    list_count.ensure(attrs);
    l.ensure(attrs);
    no.ensure(attrs);
  }

  BuiltinAttr<short, ".face.list_count"> list_count;
  BuiltinAttr<int, ".face.l"> l;
  BuiltinAttr<float3, ".face.normal"> no;
};

struct MeshBase {
  VertexData v;
  EdgeData e;
  CornerData c;
  ListData l;
  FaceData f;
};

} // namespace sculptcore::mesh
