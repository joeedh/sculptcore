#pragma once

#include "node.h"

#include "math/vector.h"
#include "util/map.h"
#include "util/vector.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "mesh/mesh_proxy.h"

#include "spatial_attrs.h"

namespace sculptcore::spatial {
struct SpatialTree {
  using Mesh = mesh::Mesh;

  int leaf_limit = 512;

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
    return node->data->faces.size() >= leaf_limit;
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

    add_face_intern(root, f, fcent);
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

private:
  void regen_node_bounds(SpatialNode *node, bool recurse);
  void regen_node_tris(SpatialNode *node);
  void regen_node_gpu_buffers(SpatialNode *node);
  void update_node_gpu_buffers(SpatialNode *node);

  void add_face_intern(SpatialNode *node, int f, math::float3 &fcent);

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

} // namespace sculptcore::spatial
