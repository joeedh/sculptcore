#pragma once

#include "attribute.h"
#include "attribute_builtin.h"
#include "deform_pool.h"

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
struct DeformPool;

/** Disk-link encoding (`.edge.vs.disk`): each live link stores
 * `(edge << 1) | side`, where `side` is the slot of the shared vertex in the
 * linked edge's `.edge.vs` — a disk walk needs no `e.vs` load to pick its next
 * slot. Dead/free slots stay raw `ELEM_NONE` (never encoded). */
constexpr int diskPack(int e, int side)
{
  return (e << 1) | side;
}
constexpr int diskEdge(int link)
{
  return link >> 1;
}
constexpr int diskSide(int link)
{
  return link & 1;
}

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
  BuiltinAttr<float3, "normals", AttrFlag::DERIVED> no;

  BuiltinAttr<bool, "select", AttrFlag::NONE, AttrUse::SELECT> select;

  /* Topology attributes. Disk head is derived from .edge.vs on load. */
  BuiltinAttr<int, ".vert.e", AttrFlag::TOPO | AttrFlag::DERIVED> e;

  /* is not instantiated until first use */
  BuiltinAttr<bool, ".boundary.vertex.dirty"> boundaryDirty;
  
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

  BuiltinAttr<int, ".edge.c", AttrFlag::TOPO | AttrFlag::DERIVED> c;

  BuiltinAttr<bool, "select", AttrFlag::NONE, AttrUse::SELECT> select;

  /* Topology attributes. Disk links are side-bit encoded — see diskPack(). .vs is
   * authoritative; the disk cycle is derived from it on load. */
  BuiltinAttr<int2, ".edge.vs", AttrFlag::TOPO> vs;
  BuiltinAttr<int4, ".edge.vs.disk", AttrFlag::TOPO | AttrFlag::DERIVED> disk;

  /* is not instantiated until first use */
  BuiltinAttr<bool, ".boundary.edge.dirty"> boundaryDirty;

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
  BuiltinAttr<int, ".corner.e", AttrFlag::TOPO | AttrFlag::DERIVED> e;
  BuiltinAttr<int, ".corner.l", AttrFlag::TOPO | AttrFlag::DERIVED> l; /* Owning list. */
  BuiltinAttr<int, ".corner.next", AttrFlag::TOPO> next;
  BuiltinAttr<int, ".corner.prev", AttrFlag::TOPO | AttrFlag::DERIVED> prev;
  BuiltinAttr<int, ".corner.radial_next", AttrFlag::TOPO | AttrFlag::DERIVED> radial_next;
  BuiltinAttr<int, ".corner.radial_prev", AttrFlag::TOPO | AttrFlag::DERIVED> radial_prev;
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
  BuiltinAttr<int, ".list.f", AttrFlag::TOPO | AttrFlag::DERIVED> f;
  BuiltinAttr<int, ".list.next", AttrFlag::TOPO> next;
  BuiltinAttr<int, ".list.size", AttrFlag::DERIVED> size;
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
    BIND_STRUCT_MEMBER(st, select);
    return st;
  }

  FaceData(int count_ = 0) : ElemData(FACE, count_)
  {
    list_count.ensure(attrs);
    l.ensure(attrs);
    no.ensure(attrs);
    select.ensure(attrs);
  }

  BuiltinAttr<short, ".face.list_count", AttrFlag::DERIVED> list_count;
  BuiltinAttr<int, ".face.list", AttrFlag::TOPO> l;
  BuiltinAttr<float3, ".face.normal", AttrFlag::DERIVED> no;

  BuiltinAttr<bool, "select", AttrFlag::NONE, AttrUse::SELECT> select;
};

struct MeshBase {
  /** The mesh's user of its DeformPool. A member rather than a raw pointer plus
   * a ~MeshBase body: base-class members are destroyed *after* the destructor
   * body, so only a member declared ahead of the element groups outlives their
   * AttrGroups — whose destructors release the weight slots their columns hold.
   * It is a *user*, not the sole owner: a meshlog step outlives its mesh. */
  DeformPoolUser deform_pool_;

  VertexData v;
  EdgeData e;
  CornerData c;
  ListData l;
  FaceData f;

  /** The mesh's deform pool, creating it (and pointing every element group at
   * it) on first use. Every AttrType::WEIGHTS column on this mesh shares it, as
   * does any meshlog chunk logging one. */
  DeformPool &deformPool();

  DeformPool *deformPoolOrNull()
  {
    return deform_pool_.ptr;
  }

  /* Set when any boundary source/derived flag is marked dirty (boundary::mark*
   * / setEdgeFlag), cleared by boundary::recomputeDirty. An O(1) "is the derived
   * vertex classification stale?" check so consumers (the boundary-aware smooth
   * brush) can skip the recompute — and its topology thaw — when nothing changed
   * since the last recompute. */
  bool boundaryDirty = false;
};

} // namespace sculptcore::mesh
