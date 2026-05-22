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
#include "spatial_enums.h"

#include "gpu/batch.h"
#include "gpu/manager.h"

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
  /* Target tri count per GPU mesh. The set of "GPU nodes" is chosen so
   * each owns a subtree whose tri count is <= this target (a single leaf
   * exceeding the target becomes its own GPU node). Independent of
   * leaf_limit, so leaves can stay small for spatial queries / brush
   * iteration while GPU draws are batched. */
  int gpu_tri_target = 2048;

  SpatialTreeMesh treeMesh;
  bool done_gpu_assignment = false;
  Mesh *m;

  SpatialTree(Mesh *m_) : m(m_)
  {

    root = alloc_node();
    root->flag =
        Spatial_Leaf | Spatial_RegenTris | Spatial_RegenGPU | Spatial_UpdateNormals;
    root->create_data();
  }

  bool filterNodes(float3 co, float radius, Vector<SpatialNode *> &out);

  bool castRay(const math::float3 &orig, const math::float3 &dir, CastRayIsect &out)
  {
    out.t = std::numeric_limits<float>::max();

    if (root->castRay(orig, dir, out)) {
      SpatialNode *node = nodes[out.nodeIndex];
      NodeTri &tri = node->data->tris[out.triIndex];
      auto *m = node->data->m;

      float w = 1.0 - out.uv[0] - out.uv[1];

      // calculate position/normal
      int v1 = m->c.v[tri.c[0]];
      int v2 = m->c.v[tri.c[1]];
      int v3 = m->c.v[tri.c[2]];

      float3 co1 = m->v.co[v1];
      float3 co2 = m->v.co[v2];
      float3 co3 = m->v.co[v3];
      out.p = co1 * w + co2 * out.uv[0] + co3 * out.uv[1];

      float3 no1 = m->v.no[v1];
      float3 no2 = m->v.no[v2];
      float3 no3 = m->v.no[v3];

      out.normal = no1 * w + no2 * out.uv[0] + no3 * out.uv[1];
      out.normal.normalize();

      return true;
    }
    return false;
  }

  void setup()
  {
    treeMesh.setup(m);
    /* Root's data is created before treeMesh.m is assigned (in our ctor),
     * so its m pointer is stale until we patch it here. */
    if (root && root->data) {
      root->data->m = m;
    }
  }

  void update_node_normals(SpatialNode *node);

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
      alloc::Delete(node);
    }
    if (drawBatch) {
      alloc::Delete(drawBatch);
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
  util::Vector<SpatialNode *> gpu_nodes();

  /* Public so tests can drive partition assignment without a GPUManager. */
  void recompute_subtree_tri_counts();
  void assign_gpu_nodes();
  SpatialNode *find_gpu_owner(SpatialNode *node);

  bool ensure_node_tris(SpatialNode *node)
  {
    if (node->flag & Spatial_RegenTris) {
      regen_node_tris(node);
      return true;
    }

    return false;
  }

  void buildAll();
  sculptcore::gpu::DrawBatch *getDrawBatch()
  {
    return drawBatch;
  }
  sculptcore::gpu::DrawBatch *buildLeafBoundsBatch(sculptcore::gpu::GPUManager &mgr);

  static binding::types::Struct<SpatialTree> *defineBindings();

  bool update(gpu::GPUManager *gpu);

private:
  sculptcore::gpu::DrawBatch *drawBatch = nullptr;
  void regen_node_bounds(SpatialNode *node, bool recurse);
  void regen_node_tris(SpatialNode *node);

  /* GPU node buffer management. A "GPU node" aggregates the triangles of
   * every leaf in its subtree into one VBO + draw command. */
  void regen_gpu_node(SpatialNode *gpu_node, gpu::GPUManager *gpu);
  void update_gpu_node_slice(SpatialNode *gpu_node,
                             SpatialNode *leaf,
                             gpu::GPUManager *gpu);
  void collect_subtree_leaves(SpatialNode *node, util::Vector<SpatialNode *> &out);
  void fill_leaf_slice(SpatialNode *leaf,
                       math::float3 *pos,
                       math::float3 *nor);

  void
  add_face_intern(SpatialNode *node, int f, std::span<Tri> &tris, math::float3 &fcent);

  SpatialNode *alloc_node()
  {
    SpatialNode *node = alloc::New<SpatialNode>("Spatial Node");

    node->id = node_idgen++;
    node->treeMesh = &treeMesh;
    node->index = nodes.size();
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
