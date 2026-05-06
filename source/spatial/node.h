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
  }

  void delete_data()
  {
    alloc::Delete<NodeData>(data);
    data = nullptr;
  }

  void add_face(sculptcore::mesh::Mesh *m, int f);

  static const litestl::binding::types::Struct<SpatialNode> *defineBindings();

private:
};

} // namespace sculptcore::spatial
