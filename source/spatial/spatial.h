#pragma once

#include "node.h"

#include "litestl/math/vector.h"
#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "mesh/mesh_proxy.h"
#include "mesh/utils/triangulate.h"

#include "spatial_attrs.h"

namespace sculptcore::gpu {
struct GPUManager;
struct DrawBatch;
} // namespace sculptcore::gpu

using namespace litestl;
namespace sculptcore::spatial {
struct SpatialTree {
  using Mesh = mesh::Mesh;

  int leaf_limit = 512;
  int depth_limit = 10;

  SpatialTreeMesh treeMesh;

  Mesh *m;

  SpatialTree(Mesh *m_) : m(m_)
  {

    root = alloc_node();
    root->flag = Spatial_Leaf;
    root->create_data();
  }

  void setup()
  {
    treeMesh.setup(m);
  }

  bool node_needs_split(SpatialNode *node)
  {
    return ((node->data->unique_verts.size() + node->data->other_verts.size()) >=
            leaf_limit) &&
           node->depth < depth_limit;
  }

  void split_node(SpatialNode *node);

  ~SpatialTree()
  {
    for (SpatialNode *node : nodes) {
      alloc::Delete<SpatialNode>(node);
    }
  }

  SpatialNode *node_from_id(int id)
  {
    return node_idmap[id];
  }

  void add_face(int f)
  {
    mesh::FaceProxy face(m, f);
    math::float3 fcent = face.calc_center();

    if (root->aabb.min[0] == FLT_MAX) {
      root->aabb.min = fcent;
      root->aabb.max = fcent;
    } else {
      root->aabb.min.min(fcent);
      root->aabb.max.max(fcent);
    }

    util::Vector<Tri, 16> tris;
    if (triangulateFace(*m, f, tris)) {
      std::span<Tri> tris_span = tris;
      add_face_intern(root, f, tris_span, fcent);
    } else {
      printf("failed to triangulate face %d\n", f);
    }
  }

  util::Vector<SpatialNode *> leaves();

  bool ensure_node_tris(SpatialNode *node)
  {
    if (node->flag & Spatial_RegenTris) {
      regen_node_tris(node);
      return true;
    }

    return false;
  }

  void buildAll();
  sculptcore::gpu::DrawBatch *buildLeafBoundsBatch(sculptcore::gpu::GPUManager &mgr);

  static binding::types::Struct<SpatialTree> *defineBindings();

private:
  void regen_node_bounds(SpatialNode *node, bool recurse);
  void regen_node_tris(SpatialNode *node);
  void regen_node_gpu_buffers(SpatialNode *node);
  void update_node_gpu_buffers(SpatialNode *node);

  void
  add_face_intern(SpatialNode *node, int f, std::span<Tri> &tris, math::float3 &fcent);

  SpatialNode *alloc_node()
  {
    SpatialNode *node = alloc::New<SpatialNode>("Spatial Node");
    node->id = node_idgen++;
    node->treeMesh = &treeMesh;
    nodes.append(node);

    if (node->id >= node_idmap.size()) {
      node_idmap.resize(node->id + 1);
    }

    node_idmap[node->id] = node;

    return node;
  }

  SpatialNode *root;
  util::Vector<SpatialNode *> nodes;
  util::Vector<SpatialNode *> node_idmap;
  int node_idgen = 1;
};
FORWARD_CLS_BINDING(SpatialTree)

} // namespace sculptcore::spatial
