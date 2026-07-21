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
#include "litestl/util/set.h"
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

/* Per-leaf slice inside a GPU node's aggregated vertex buffer. */
struct LeafSlice {
  SpatialNode *leaf = nullptr;
  int vert_start = 0; /* offset in verts */
  int vert_count = 0; /* = tris * 3 */
};

/* GPU mesh owned by a node selected as a "GPU root" — aggregates the
 * triangles of every leaf in its subtree into a single VBO + draw command,
 * so each face is rendered exactly once. */
struct GpuData {
  gpu::Buffer *pos = nullptr;
  gpu::Buffer *nor = nullptr;
  /* One vertex buffer per requested attribute, index-aligned with
   * SpatialTree::requestedAttrs (slot order, after the implicit pos@0/nor@1).
   * When the tree has no requested set this holds a single legacy `color`
   * stream (float4, default white) so the basic mesh shader's @location(2) is
   * never left unbound. */
  util::Vector<gpu::Buffer *> attrBufs;
  /* SpatialTree::requestedAttrsVersion these attrBufs were built against. The
   * slice fast-path falls back to a full regen when it has drifted. */
  uint64_t builtAttrsVersion = 0;
  gpu::DrawCommand *cmd = nullptr;
  util::Vector<LeafSlice> slices; /* DFS-order leaf list defining the layout */
  int total_verts = 0;
  // Buffers were (re)planned since the external-draw consumer last read this
  // node; reported as SC_EXTERNAL_DRAW_UPDATE_TOPOLOGY and cleared there.
  bool extern_topo_dirty = true;

  /* GPU-resident stroke path (debug app): per render-VBO slot, the global
   * vertex index it draws (slot order == pos/nor). Uploaded host->GPU once at
   * stroke begin; the scatter compute pass reads it to fan co/no into pos/nor.
   * Null outside a GPU-resident stroke. */
  gpu::Buffer *slotVertex = nullptr;

  ~GpuData()
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
    for (gpu::Buffer *b : attrBufs) {
      if (b) {
        alloc::Delete(b);
      }
    }
    attrBufs.clear_and_contract();
    builtAttrsVersion = 0;
    if (cmd) {
      alloc::Delete(cmd);
      cmd = nullptr;
    }
    if (slotVertex) {
      alloc::Delete(slotVertex);
      slotVertex = nullptr;
    }
    slices.clear_and_contract();
    total_verts = 0;
  }
};

struct SpatialNode {
  using float3 = math::float3;
  using AABB = litestl::math::AABB<float3>;

  struct NodeData {
    util::OrderedSet<int> unique_verts;
    util::OrderedSet<int> unique_faces;

    util::Vector<NodeTri> tris;
    Mesh *m;
  };

  SpatialNode *parent = nullptr;
  SpatialNode *children[2];
  int depth = 0;

  AABB aabb;

  NodeFlags flag = Spatial_None;
  NodeData *data = nullptr;
  GpuData *gpu_data = nullptr;
  SpatialTreeMesh *treeMesh = nullptr;

  /* Cached count of tris across all leaves in this subtree. Recomputed
   * during SpatialTree::update(). Leaves: == data->tris.size(). */
  int subtree_tri_count = 0;
  bool is_gpu_node = false;

  /* Verts that moved since the last normals/bounds update for this leaf.
   * Populated by brush execution (single-writer per node — the parallel_for
   * over nodes serializes within each node) and consumed + cleared by
   * SpatialTree::update_node_normals. May contain duplicates; left empty
   * to request a full rebuild (e.g. on initial build or topology change). */
  util::Vector<int> affected_verts;

  /* Node IDs are always > 0. */
  int id = 0;
  int index = 0;

  int debugIdOffset = 0;

  /* Meshlog capture walk-elision stamps, one per brush-program sub-command
   * slot: once a dab's execPre has walked (and captured) every element of this
   * leaf for stroke `sid` under sub-command `tool`, later dabs of the same
   * stroke skip the whole leaf. Only consulted for topology-stable steps (no
   * dyntopo), where a leaf's element set can't change mid-stroke. */
  struct CaptureStamp {
    int sid = 0;  /* 0 = never walked (stroke ids stored as sid+1) */
    int tool = 0;
  };
  static constexpr int MAX_CAPTURE_SLOTS = 8;
  CaptureStamp captureStamps[MAX_CAPTURE_SLOTS];

  /* coPrev dirty-list dedup stamp (CommandExecutor::coPrevGen_): a node whose
   * verts a brush exec may have moved is appended to the executor's refresh
   * list once per coPrev refresh generation. */
  int coPrevStamp = 0;

  SpatialNode()
  {
    children[0] = children[1] = nullptr;
  }

  SpatialNode(const SpatialNode &b) = delete;

  SpatialNode(SpatialNode &&b)
      : aabb(b.aabb), flag(b.flag), data(b.data), gpu_data(b.gpu_data), id(b.id),
        depth(b.depth), subtree_tri_count(b.subtree_tri_count),
        is_gpu_node(b.is_gpu_node), treeMesh(b.treeMesh)
  {
    children[0] = b.children[0];
    children[1] = b.children[1];

    b.flag = Spatial_None;
    b.data = nullptr;
    b.gpu_data = nullptr;
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

  util::OrderedSet<int> &unique_faces() const
  {
    return data->unique_faces;
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
    if (gpu_data) {
      alloc::Delete<GpuData>(gpu_data);
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
      /* Near-first, pruned descent: visit the closer child first and skip any
       * subtree whose AABB entry distance already exceeds the best hit — a ray
       * into a closed mesh then never descends into the geometry behind the
       * front surface. (Accepted hits require t > 0, so the forward-ray slab
       * test drops nothing the old unordered walk could actually hit.) */
      float tc[2];
      SpatialNode *order[2];
      int n = 0;
      for (SpatialNode *child : children) {
        float tEnter;
        if (child && math::aabbRayEnter(child->aabb, orig, dir, tEnter)) {
          order[n] = child;
          tc[n] = tEnter;
          n++;
        }
      }
      if (n == 2 && tc[1] < tc[0]) {
        std::swap(order[0], order[1]);
        std::swap(tc[0], tc[1]);
      }
      bool ok = false;
      for (int i = 0; i < n; i++) {
        if (tc[i] >= out.t) {
          continue;
        }
        if (order[i]->castRay(orig, dir, out)) {
          ok = true;
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

  /* Faces whose triangles intersect the cone (co, ray=base->tip, r1->r2). */
  void collectConeFaces(const float3 &co,
                        const float3 &ray,
                        float r1,
                        float r2,
                        util::Set<int> &out)
  {
    if (!(flag & Spatial_Leaf)) {
      for (SpatialNode *child : children) {
        if (math::aabbConeIsects(co, ray, r1, r2, child->aabb)) {
          child->collectConeFaces(co, ray, r1, r2, out);
        }
      }
      return;
    }

    float3 tip = co + ray;
    for (NodeTri &tri : data->tris) {
      float3 &c1 = data->m->v.co[data->m->c.v[tri.c[0]]];
      float3 &c2 = data->m->v.co[data->m->c.v[tri.c[1]]];
      float3 &c3 = data->m->v.co[data->m->c.v[tri.c[2]]];

      if (math::triConeIsects(co, tip, r1, r2, c1, c2, c3)) {
        out.add(tri.f);
      }
    }
  }

  /* Verts inside the cone. */
  void collectConeVerts(const float3 &co,
                        const float3 &ray,
                        float r1,
                        float r2,
                        util::Set<int> &out)
  {
    if (!(flag & Spatial_Leaf)) {
      for (SpatialNode *child : children) {
        if (math::aabbConeIsects(co, ray, r1, r2, child->aabb)) {
          child->collectConeVerts(co, ray, r1, r2, out);
        }
      }
      return;
    }

    float raylen = ray.length();
    if (raylen < 1e-8f) {
      return;
    }
    float3 nray = ray * (1.0f / raylen);

    for (int v : data->unique_verts) {
      float3 &vco = data->m->v.co[v];
      float t = (vco - co).dot(nray);

      if (t < 0.0f || t >= raylen) {
        continue;
      }

      float3 proj = co + nray * t;
      float r = r1 + (r2 - r1) * (t / raylen);

      if (proj.distanceSqr(vco) < r * r) {
        out.add(v);
      }
    }
  }

  /* Faces whose triangles intersect the frustum (n inward planes). */
  void collectFrustumFaces(const float4 *planes, int n, util::Set<int> &out)
  {
    if (!(flag & Spatial_Leaf)) {
      for (SpatialNode *child : children) {
        if (math::aabbFrustumIsects(planes, n, child->aabb)) {
          child->collectFrustumFaces(planes, n, out);
        }
      }
      return;
    }

    for (NodeTri &tri : data->tris) {
      float3 &c1 = data->m->v.co[data->m->c.v[tri.c[0]]];
      float3 &c2 = data->m->v.co[data->m->c.v[tri.c[1]]];
      float3 &c3 = data->m->v.co[data->m->c.v[tri.c[2]]];

      if (math::triFrustumIsects(planes, n, c1, c2, c3)) {
        out.add(tri.f);
      }
    }
  }

  /* Verts inside the frustum (n inward planes). */
  void collectFrustumVerts(const float4 *planes, int n, util::Set<int> &out)
  {
    if (!(flag & Spatial_Leaf)) {
      for (SpatialNode *child : children) {
        if (math::aabbFrustumIsects(planes, n, child->aabb)) {
          child->collectFrustumVerts(planes, n, out);
        }
      }
      return;
    }

    for (int v : data->unique_verts) {
      if (math::pointInFrustum(planes, n, data->m->v.co[v])) {
        out.add(v);
      }
    }
  }

private:
};

} // namespace sculptcore::spatial
