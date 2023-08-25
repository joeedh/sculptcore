#pragma once

#include "math/vector.h"
#include "util/alloc.h"
#include "util/map.h"
#include "util/ordered_set.h"
#include "util/vector.h"

#include "mesh/mesh.h"

#include "spatial_enums.h"

#include <cfloat>

namespace sculptcore::gpu {
struct VBO;
struct Buffer;
} // namespace sculptcore::gpu

namespace sculptcore::spatial {
struct NodeTri {
  int c[3]; /* corners */
  int f;
  int eflag;
};

struct SpatialNode {
  struct NodeData {
    struct GPUData {
      sculptcore::gpu::VBO *vbo = nullptr;
      sculptcore::gpu::Buffer *tris = nullptr;
      sculptcore::gpu::Buffer *lines = nullptr;

      ~GPUData();
    };

    util::OrderedSet<int> unique_verts;
    util::OrderedSet<int> other_verts;
    util::OrderedSet<int> faces;

    util::Vector<NodeTri> tris;

    GPUData gpu;
  };

  SpatialNode *children[2];

  using float3 = math::float3;

  float3 min = float3(FLT_MAX);
  float3 max = float3(FLT_MIN);

  NodeFlags flag = Spatial_None;
  NodeData *data = nullptr;

  /* Node IDs are always > 0. */
  int id = 0;

  SpatialNode()
  {
    children[0] = children[1] = nullptr;
  }

  SpatialNode(SpatialNode &&b)
  {
    min = b.min;
    max = b.max;
    flag = b.flag;
    data = b.data;
    id = b.id;

    children[0] = b.children[0];
    children[1] = b.children[1];

    b.flag = Spatial_None;
    b.data = nullptr;
  }

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

  util::OrderedSet<int> &faces() const
  {
    return data->faces;
  }

  util::Vector<NodeTri> &tris() const
  {
    return data->tris;
  }

  SpatialNode(const SpatialNode &b) = delete;

  DEFAULT_MOVE_ASSIGNMENT(SpatialNode)

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

private:
};
} // namespace sculptcore::spatial
