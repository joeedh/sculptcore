#pragma once

#include "gpu/command.h"
#include "gpu/vbo.h"
#include "litestl/binding/binding.h"
#include "litestl/math/aabb.h"
#include "litestl/math/geom.h"
#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/map.h"
#include "litestl/util/ordered_set.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"

#include "spatial_attrs.h"
#include "spatial_base.h"
#include "spatial_enums.h"

#include <cfloat>

using namespace litestl;
using namespace litestl::math;
using sculptcore::mesh::Mesh;

namespace sculptcore::spatial {
struct NodeTri {
  int c[3]; /* corners */
  int f;
  int eflag;
};

struct SpatialNode;

struct SpatialNode {
  using float3 = math::float3;
  using AABB = litestl::math::AABB<float3>;

  struct NodeData {
    util::OrderedSet<int> unique_verts;
    util::OrderedSet<int> other_verts;
    util::OrderedSet<int> unique_faces;
    util::OrderedSet<int> other_faces;

    util::Vector<NodeTri> tris;
    struct GPUData {
      gpu::Buffer *pos = nullptr;
      gpu::Buffer *nor = nullptr;
      gpu::DrawCommand *cmd = nullptr;

      ~GPUData()
      {
        dispose();
      }

      void dispose()
      {
        if (pos) {
          alloc::Delete(pos);
          pos = nullptr;
        }
        if (nor) {
          alloc::Delete(nor);
          nor = nullptr;
        }
        if (cmd) {
          alloc::Delete(cmd);
          cmd = nullptr;
        }
      }
    } gpu;
    Mesh *m;
  };

  SpatialNode *parent = nullptr;
  SpatialNode *children[2];
  int depth = 0;

  AABB aabb;

  NodeFlags flag = Spatial_None;
  NodeData *data = nullptr;
  SpatialTreeMesh *treeMesh = nullptr;

  /* Node IDs are always > 0. */
  int id = 0;
  int index = 0;

  int debugIdOffset = 0;
  
  SpatialNode()
  {
    children[0] = children[1] = nullptr;
  }

  SpatialNode(const SpatialNode &b) = delete;

  SpatialNode(SpatialNode &&b)
      : aabb(b.aabb), flag(b.flag), data(b.data), id(b.id), depth(b.depth),
        treeMesh(b.treeMesh)
  {
    children[0] = b.children[0];
    children[1] = b.children[1];

    b.flag = Spatial_None;
    b.data = nullptr;
  }

  DEFAULT_MOVE_ASSIGNMENT(SpatialNode)

  void update(NodeFlags update_flags)
  {
    flag |= update_flags;
  }

  util::OrderedSet<int> &unique_verts() const
  {
    return data->unique_verts;
  }

  util::OrderedSet<int> &other_verts() const
  {
    return data->other_verts;
  }

  util::OrderedSet<int> &unique_faces() const
  {
    return data->unique_faces;
  }

  util::OrderedSet<int> &other_faces() const
  {
    return data->other_faces;
  }

  util::Vector<NodeTri> &tris() const
  {
    return data->tris;
  }

  ~SpatialNode()
  {
    if (data) {
      alloc::Delete<NodeData>(data);
    }
  }

  void create_data()
  {
    data = alloc::New<NodeData>("Spatial Node Data");
    data->m = treeMesh->m;
  }

  void delete_data()
  {
    alloc::Delete<NodeData>(data);
    data = nullptr;
  }

  void add_face(sculptcore::mesh::Mesh *m, int f);

  static const litestl::binding::types::Struct<SpatialNode> *defineBindings();

  bool castRay(const math::float3 &orig, const math::float3 &dir, CastRayIsect &out)
  {
    if (!(flag & Spatial_Leaf)) {
      bool ok = false;
      for (SpatialNode *child : children) {
        if (math::aabbRayIsects(child->aabb, orig, dir)) {
          if (child->castRay(orig, dir, out)) {
            ok = true;
          }
        }
      }
      return ok;
    }

    bool ok = false;
    for (int i : util::IndexRange(data->tris.size())) {
      NodeTri &tri = data->tris[i];
#if 0
      printf("-- %p %p  %d %d\n",
             data,
             data->m,
             int(data->m->c.count),
             int(data->m->v.count));
      printf("%d %d %d\n", tri.c[0], tri.c[1], tri.c[2]);
#endif
      int v1 = data->m->c.v[tri.c[0]];
      int v2 = data->m->c.v[tri.c[1]];
      int v3 = data->m->c.v[tri.c[2]];

      float3 &co1 = data->m->v.co[v1];
      float3 &co2 = data->m->v.co[v2];
      float3 &co3 = data->m->v.co[v3];

      RayTriIsect<float3> rayIsect;
      if (!math::rayTriIsect(orig, dir, co1, co2, co3, rayIsect)) {
        continue;
      }

      if (rayIsect.t > 0 && rayIsect.t < out.t) {
        ok = true;
        out.t = rayIsect.t;
        out.uv = rayIsect.uv;
        out.triIndex = i;
        out.nodeIndex = this->index;
      }
    }
    return ok;
  }

private:
};

} // namespace sculptcore::spatial
