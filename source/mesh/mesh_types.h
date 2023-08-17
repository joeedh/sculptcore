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
struct Mesh;

struct VertexData : public ElemData {
  using float3 = math::float3;

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

  BuiltinAttr<float3, "positions"> co;
  BuiltinAttr<float3, "normals"> no;

  BuiltinAttr<bool, "select"> select;

  /* Topology attributes. */
  BuiltinAttr<int, ".vert.e", ATTR_TOPO> e;

  /* Move vsrc into vdst; vdst must be freed. */
  void move_elem(Mesh *m, int vsrc, int vdst);
  void swap_elems(Mesh *m, int v1, int v2);

  /* if vtarget is -1 then v will be used. */
  void splice(Mesh *m, int v, int vnew, int vtarget = -1);
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

  BuiltinAttr<int, ".edge.c", ATTR_TOPO> c;

  BuiltinAttr<bool, "select"> select;

  /* Topology attributes. */
  BuiltinAttr<int2, ".edge.vs", ATTR_TOPO> vs;
  BuiltinAttr<int4, ".edge.vs.disk", ATTR_TOPO> disk;

  int swap_elems(int e1, int v2);
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

  BuiltinAttr<int, ".corner.v", ATTR_TOPO> v;
  BuiltinAttr<int, ".corner.e", ATTR_TOPO> e;
  BuiltinAttr<int, ".corner.l", ATTR_TOPO> l; /* Owning list. */
  BuiltinAttr<int, ".corner.next", ATTR_TOPO> next;
  BuiltinAttr<int, ".corner.prev", ATTR_TOPO> prev;
  BuiltinAttr<int, ".corner.radial_next", ATTR_TOPO> radial_next;
  BuiltinAttr<int, ".corner.radial_prev", ATTR_TOPO> radial_prev;
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

  BuiltinAttr<int, ".list.c", ATTR_TOPO> c;
  BuiltinAttr<int, ".list.f", ATTR_TOPO> f;
  BuiltinAttr<int, ".list.next", ATTR_TOPO> next;
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
  BuiltinAttr<int, ".face.l", ATTR_TOPO> l;
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
