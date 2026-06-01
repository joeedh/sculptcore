#pragma once

#include "attribute.h"
#include "attribute_builtin.h"

#include "litestl/math/vector.h"

#include "litestl/util/boolvector.h"
#include "litestl/util/string.h"

#include "elem_data.h"
#include "mesh_base.h"
#include "mesh_enums.h"

#include <algorithm>
#include <concepts>
#include <cstdio>
#include <type_traits>

#include "litestl/binding/binding.h"

using namespace litestl;

namespace sculptcore::mesh {
struct Mesh;

struct VertexData : public ElemData {
  using float3 = math::float3;

  static binding::types::Struct<VertexData> *defineBindings()
  {
    using binding::types::Struct;
    Struct<VertexData> *st =
        new Struct<VertexData>("sculptcore::mesh::VertexData", sizeof(VertexData));
    st->inherit(ElemData::defineBindings());

    BIND_STRUCT_MEMBER(st, co);
    BIND_STRUCT_MEMBER(st, no);
    BIND_STRUCT_MEMBER(st, e);

    return st;
  }

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
  BuiltinAttr<int, ".vert.e", AttrFlag::TOPO> e;

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

  static binding::types::Struct<EdgeData> *defineBindings()
  {
    using binding::types::Struct;
    Struct<EdgeData> *st =
        new Struct<EdgeData>("sculptcore::mesh::EdgeData", sizeof(EdgeData));
    BIND_STRUCT_MEMBER(st, capacity_);
    BIND_STRUCT_MEMBER(st, c);
    BIND_STRUCT_MEMBER(st, vs);
    BIND_STRUCT_MEMBER(st, select);
    BIND_STRUCT_MEMBER(st, disk);

    return st;
  }

  BuiltinAttr<int, ".edge.c", AttrFlag::TOPO> c;

  BuiltinAttr<bool, "select"> select;

  /* Topology attributes. */
  BuiltinAttr<int2, ".edge.vs", AttrFlag::TOPO> vs;
  BuiltinAttr<int4, ".edge.vs.disk", AttrFlag::TOPO> disk;

  int swap_elems(int e1, int v2);
};

struct CornerData : public ElemData {
  using int2 = math::int2;
  using int4 = math::int4;

  static binding::types::Struct<CornerData> *defineBindings()
  {
    using binding::types::Struct;
    Struct<CornerData> *st =
        new Struct<CornerData>("sculptcore::mesh::CornerData", sizeof(CornerData));
    st->inherit(ElemData::defineBindings());
    BIND_STRUCT_MEMBER(st, capacity_);
    BIND_STRUCT_MEMBER(st, v);
    BIND_STRUCT_MEMBER(st, e);
    BIND_STRUCT_MEMBER(st, l);
    BIND_STRUCT_MEMBER(st, next);
    BIND_STRUCT_MEMBER(st, prev);
    BIND_STRUCT_MEMBER(st, radial_next);
    BIND_STRUCT_MEMBER(st, radial_prev);

    return st;
  }

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

  BuiltinAttr<int, ".corner.v", AttrFlag::TOPO | AttrFlag::TOPO_KEEP_FROZEN> v;
  BuiltinAttr<int, ".corner.e", AttrFlag::TOPO> e;
  BuiltinAttr<int, ".corner.l", AttrFlag::TOPO> l; /* Owning list. */
  BuiltinAttr<int, ".corner.next", AttrFlag::TOPO> next;
  BuiltinAttr<int, ".corner.prev", AttrFlag::TOPO> prev;
  BuiltinAttr<int, ".corner.radial_next", AttrFlag::TOPO> radial_next;
  BuiltinAttr<int, ".corner.radial_prev", AttrFlag::TOPO> radial_prev;
};

struct ListData : public ElemData {
  using int2 = math::int2;
  using int4 = math::int4;

  static binding::types::Struct<ListData> *defineBindings()
  {
    using binding::types::Struct;
    Struct<ListData> *st =
        new Struct<ListData>("sculptcore::mesh::ListData", sizeof(ListData));
    BIND_STRUCT_MEMBER(st, capacity_);
    BIND_STRUCT_MEMBER(st, c);
    BIND_STRUCT_MEMBER(st, f);
    BIND_STRUCT_MEMBER(st, next);
    BIND_STRUCT_MEMBER(st, size);

    return st;
  }

  ListData(int count_ = 0) : ElemData(LIST, count_)
  {
    c.ensure(attrs);
    f.ensure(attrs);
    next.ensure(attrs);
    size.ensure(attrs);
  }

  BuiltinAttr<int, ".list.c", AttrFlag::TOPO> c;
  BuiltinAttr<int, ".list.f", AttrFlag::TOPO> f;
  BuiltinAttr<int, ".list.next", AttrFlag::TOPO> next;
  BuiltinAttr<int, ".list.size"> size;
};

struct FaceData : public ElemData {
  using int2 = math::int2;
  using int4 = math::int4;
  using float3 = math::float3;

  static binding::types::Struct<FaceData> *defineBindings()
  {
    using binding::types::Struct;
    Struct<FaceData> *st = new Struct<FaceData>("sculptcore::mesh::FaceData", sizeof(FaceData));
    st->inherit(ElemData::defineBindings());
    BIND_STRUCT_MEMBER(st, list_count);
    BIND_STRUCT_MEMBER(st, l);
    BIND_STRUCT_MEMBER(st, no);
    return st;
  }

  FaceData(int count_ = 0) : ElemData(FACE, count_)
  {
    list_count.ensure(attrs);
    l.ensure(attrs);
    no.ensure(attrs);
  }

  BuiltinAttr<short, ".face.list_count"> list_count;
  BuiltinAttr<int, ".face.list", AttrFlag::TOPO> l;
  BuiltinAttr<float3, ".face.normal"> no;
};

struct MeshBase {
  VertexData v;
  EdgeData e;
  CornerData c;
  ListData l;
  FaceData f;

  /* Set when any boundary source/derived flag is marked dirty (boundary::mark*
   * / setEdgeFlag), cleared by boundary::recomputeDirty. An O(1) "is the derived
   * vertex classification stale?" check so consumers (the boundary-aware smooth
   * brush) can skip the recompute — and its topology thaw — when nothing changed
   * since the last recompute. */
  bool boundaryDirty = false;
};

} // namespace sculptcore::mesh
