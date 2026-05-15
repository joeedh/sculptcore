#include "spatial.h"
#include "shaders/spatial_shaders.h"

#include "node.h"

#include "litestl/math/geom.h"
#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

// #include "litestl/util/map.h"
#include "litestl/util/rand.h"

#include "gpu/batch.h"
#include "gpu/command.h"
#include "gpu/manager.h"
#include "gpu/types.h"
#include "gpu/vbo.h"

#include "mesh/mesh.h"
#include "mesh/mesh_proxy.h"
#include "mesh/utils/triangulate.h"
#include "util/index_range.h"
#include "util/task.h"

#include <cmath>

using namespace litestl::util;
using namespace litestl::math;
using namespace sculptcore::mesh;
using namespace litestl;

static inline float3 calc_eps_float3(float3 size)
{
  float3 eps = size * 0.001f;

  for (int i = 0; i < 3; i++) {
    eps[i] = std::min(eps[i], 0.00001f);
  }

  return eps;
}

namespace sculptcore::spatial {

ATTR_NO_OPT bool
SpatialTree::filterNodes(float3 co, float radius, Vector<SpatialNode *> &out)
{
  printf("\nco: %f %f %f radius: %f\n", co[0], co[1], co[2], radius);

  bool ok = false;
  for (SpatialNode *node : leaves()) {
    if (1 || aabbSphereIsect(co, radius, node->aabb)) {
      node->debugIdOffset++;
      out.append(node);
      ok = true;
    }
  }

  printf("size: %d\n", int(out.size()));
  return ok;
}

void SpatialTree::regen_node_tris(SpatialNode *node)
{
  node->flag &= ~Spatial_RegenTris;

  /* TODO: use a property CDT for > 4 vert or > 1 hole faces.
   * For now just handle triangles and quads.
   */
  node->data->tris.clear_and_contract();
  for (int f : node->data->unique_faces) {
    int l = m->f.l[f];
    int c = m->l.c[l];

    NodeTri &tri = node->data->tris.grow_one();
    tri.c[0] = c;
    tri.c[1] = m->c.next[c];
    tri.c[2] = m->c.next[tri.c[1]];
    tri.f = f;

    if (m->l.size[l] > 3) {
      NodeTri &tri2 = node->data->tris.grow_one();

      tri2.c[0] = c;
      tri2.c[1] = m->c.next[m->c.next[c]];
      tri2.c[2] = m->c.next[tri2.c[1]];
      tri2.f = f;
    }
  }
}

[[clang::optnone]]
void SpatialTree::add_face_intern(SpatialNode *node,
                                  int f,
                                  std::span<Tri> &tris,
                                  float3 &fcent)
{
  if ((node->flag & Spatial_Leaf) && node_needs_split(node)) {
    split_node(node);
  }

  auto &co = m->v.co;

  if (!(node->flag & Spatial_Leaf)) {
    float mindis = FLT_MAX;
    SpatialNode *newnode = nullptr;
    bool ok = false;

    for (int i = 0; i < 2; i++) {
      SpatialNode *c = node->children[i];

      for (auto &tri : tris) {
        auto &co1 = co[tri.v[0]];
        auto &co2 = co[tri.v[1]];
        auto &co3 = co[tri.v[2]];

        if (aabbTriOverlaps(c->aabb, co1, co2, co3)) {
          add_face_intern(c, f, tris, fcent);
          ok = true;
          break;
        }
      }
    }

    if (!ok) {
      printf("not ok! %d (%d tris)\n", f, int(tris.size()));
    }
    return;
  }

  node->flag |= Spatial_RegenTris | Spatial_RegenBounds;
  FaceProxy face(m, f);

  if (treeMesh.f.node[face] == 0) {
    treeMesh.f.node[face] = node->id;
    node->data->unique_faces.add(f);
  } else {
    node->data->other_faces.add(f);
  }

  for (auto list : face.lists()) {
    for (auto c : list) {
      if (treeMesh.v.node[c.v()]) {
        node->data->other_verts.add(c.v());
      } else {
        node->data->unique_verts.add(c.v());
        treeMesh.v.node[c.v()] = node->id;
      }
    }
  }
}

ATTR_NO_OPT
void SpatialTree::split_node(SpatialNode *node)
{
  node->children[0] = alloc_node();
  node->children[1] = alloc_node();

  using namespace litestl::math;
  const float3 min(node->aabb.min), max(node->aabb.max);
  float3 mean(0.0f);

  for (int v : node->data->unique_verts) {
    VertProxy vert(m, v);
    mean += vert.co();

    /* Unassign verts. */
    treeMesh.v.node[v] = 0;
  }

  mean /= node->data->unique_verts.size();

  float3 size = max - min;
  int axis = 0;

#if 1
  for (int i = 1; i < 3; i++) {
    if (size[i] > size[axis]) {
      axis = i;
    } else if (size[i] == size[axis]) {
      // axis = node->depth & 1 ? i : axis;
    }
  }
#else
  axis = node->depth % 3;
#endif

  float t = mean[axis] / (max[axis] - min[axis]);
  t = std::min(std::max(t, 0.01f), 0.99f);
  t = 0.5; // XXX

  for (int i = 0; i < 2; i++) {
    SpatialNode *child = node->children[i];
    child->parent = node;

    child->flag = Spatial_Leaf | Spatial_RegenTris | Spatial_RegenBounds |
                  Spatial_RegenGPU | Spatial_UpdateNormals;
    child->depth = node->depth + 1;

    child->aabb.min = node->aabb.min;
    child->aabb.max = node->aabb.max;

    if (i == 0) {
      child->aabb.max[axis] = child->aabb.min[axis] + size[axis] * (1.0 - t);
    } else {
      child->aabb.min[axis] = child->aabb.min[axis] + size[axis] * t;
    }

    child->create_data();
  }

  node->flag &= ~Spatial_Leaf;
  Vector<Tri, 16> tris;

  for (int f : node->data->unique_faces) {
    FaceProxy face(m, f);
    float3 fcent = face.calc_center();

    // unassign face
    treeMesh.f.node[f] = 0;

    tris.clear();
    if (triangulateFace(*m, f, tris)) {
      std::span<Tri> tris_span = tris;
      add_face_intern(node, f, tris_span, fcent);
    }
  }
  for (int f : node->data->other_faces) {
    FaceProxy face(m, f);
    float3 fcent = face.calc_center();

    tris.clear();
    if (triangulateFace(*m, f, tris)) {
      std::span<Tri> tris_span = tris;
      add_face_intern(node, f, tris_span, fcent);
    }
  }

  node->delete_data();
  node->flag |= Spatial_RegenBounds;
}

ATTR_NO_OPT
void SpatialTree::regen_node_bounds(SpatialNode *node, bool recurse)
{
  node->flag &= ~Spatial_RegenBounds;

  node->aabb.reset();

  if (!(node->flag & Spatial_Leaf)) {
    for (int i = 0; i < 2; i++) {
      if (recurse && (node->children[i]->flag & Spatial_RegenBounds)) {
        regen_node_bounds(node->children[i], true);
      }

      node->aabb.min.min(node->children[i]->aabb.min);
      node->aabb.max.max(node->children[i]->aabb.max);
    }
  } else {
    if (node->data->unique_verts.size() != 0) {
      node->aabb.min = float3(FLT_MAX);
      node->aabb.max = float3(FLT_MIN);
    }

    for (int v : node->data->unique_verts) {
      VertProxy vert(m, v);

      float3 &co = vert.co();

      node->aabb.min.min(co);
      node->aabb.max.max(co);
    }

    for (int f : node->data->unique_faces) {
      FaceProxy face(m, f);

      for (auto list : face.lists()) {
        for (auto c : list) {
          float3 &co = c.v().co();

          node->aabb.min.min(co);
          node->aabb.max.max(co);
        }
      }
    }

    float3 eps = calc_eps_float3(node->aabb.max - node->aabb.min);

    node->aabb.min -= eps;
    node->aabb.max += eps;
  }
}

util::Vector<SpatialNode *> SpatialTree::leaves()
{
  util::Vector<SpatialNode *> leaves;

  for (SpatialNode *node : nodes) {
    if (node->flag & Spatial_Leaf) {
      leaves.append(node);
    }
  }

  return leaves;
}

ATTR_NO_OPT
void SpatialTree::buildAll()
{
  setup();

  m->calcAABB(root->aabb.min, root->aabb.max);
  m->recalc_normals();
  float eps = 0.0000001f;
  root->aabb.min -= eps;
  root->aabb.max += eps;

  int n = m->f.count;

  // insert faces in random order
  // to balance tree better
  litestl::util::Random rnd(0);
  int *faces = new int[n];
  for (int i = 0; i < n; i++) {
    faces[i] = i;
  }
  for (int i = 0; i < (n >> 1); i++) {
    int ri = rnd.get_int() % n;
    std::swap(faces[i], faces[ri]);
  }

  for (int i = 0; i < n; i++) {
    add_face(faces[i]);
  }

  delete[] faces;

  regen_node_bounds(root, true);
}

ATTR_NO_OPT
sculptcore::gpu::DrawBatch *
SpatialTree::buildLeafBoundsBatch(sculptcore::gpu::GPUManager &mgr)
{
  using namespace sculptcore::gpu;
  litestl::util::Random rnd(0);

  util::Vector<SpatialNode *> ls = leaves();

  /* 12 edges per box × 2 endpoints = 24 verts per leaf. */
  const int vertsPerLeaf = 24;
  int totalVerts = ls.size() * vertsPerLeaf;

  Buffer *posBuf = mgr.createBuffer(
      litestl::util::string("position"), GPUType::FLOAT32, 3, totalVerts);
  Buffer *colorBuf =
      mgr.createBuffer(litestl::util::string("color"), GPUType::FLOAT32, 4, totalVerts);
  Buffer *uvBuf =
      mgr.createBuffer(litestl::util::string("uv"), GPUType::FLOAT32, 2, totalVerts);

  float3 *pos = posBuf->get_data<float3>();
  float4 *color = colorBuf->get_data<float4>();
  float2 *uv = uvBuf->get_data<float2>();

  int idx = 0;

  auto addLine =
      [pos, color, uv, &idx](const float3 &a, const float3 &b, const float4 &clr) {
        pos[idx] = a;
        color[idx] = clr;
        uv[idx] = float2(0.0f, 0.0f);
        idx++;

        pos[idx] = b;
        color[idx] = clr;
        uv[idx] = float2(1.0f, 1.0f);
        idx++;
      };

  for (SpatialNode *node : ls) {
    litestl::util::Random rnd2(node->id + node->debugIdOffset);

    float4 clr(0.0);

    clr[0] = rnd2.get_float();
    clr[1] = rnd2.get_float();
    clr[2] = rnd2.get_float();
    clr.normalize();
    clr[3] = 1.0;

    float3 mn = node->aabb.min;
    float3 mx = node->aabb.max;
    float3 c[8] = {
        {mn[0], mn[1], mn[2]},
        {mx[0], mn[1], mn[2]},
        {mx[0], mx[1], mn[2]},
        {mn[0], mx[1], mn[2]},
        {mn[0], mn[1], mx[2]},
        {mx[0], mn[1], mx[2]},
        {mx[0], mx[1], mx[2]},
        {mn[0], mx[1], mx[2]},
    };

    /* Bottom quad (z = mn). */
    addLine(c[0], c[1], clr);
    addLine(c[1], c[2], clr);
    addLine(c[2], c[3], clr);
    addLine(c[3], c[0], clr);

    /* Top quad (z = mx). */
    addLine(c[4], c[5], clr);
    addLine(c[5], c[6], clr);
    addLine(c[6], c[7], clr);
    addLine(c[7], c[4], clr);

    /* Vertical edges. */
    addLine(c[0], c[4], clr);
    addLine(c[1], c[5], clr);
    addLine(c[2], c[6], clr);
    addLine(c[3], c[7], clr);
  }

  posBuf->dirty();

  DrawBatch *batch = mgr.createBatch();
  batch->buffers.append(posBuf);
  batch->buffers.append(colorBuf);
  batch->buffers.append(uvBuf);

  auto *shader = &spatialShaders.basicLineShader;

  DrawCommand *cmd = mgr.createCommand(
      batch, GPUCmdType::DRAW_LINES, shader, 0, totalVerts, totalVerts / 2);
  cmd->attrs.append(posBuf);
  cmd->attrs.append(colorBuf);
  cmd->attrs.append(uvBuf);

  return batch;
}

void SpatialTree::update_node_normals(SpatialNode *node)
{
  node->flag &= ~Spatial_UpdateNormals;

  // very simple normal update for now

  for (int v : node->unique_verts()) {
    m->v.no[v].zero();
  }

  auto &node_vattr = node->treeMesh->v.node;
  auto &node_fattr = node->treeMesh->f.node;

  for (int f : node->unique_faces()) {
    m->f.no[f].zero();
  }

  for (const auto &tri : node->data->tris) {
    int v1 = m->c.v[tri.c[0]];
    int v2 = m->c.v[tri.c[1]];
    int v3 = m->c.v[tri.c[2]];

    float3 n = triNormal(m->v.co[v1], m->v.co[v2], m->v.co[v3]);

    if (node_fattr[tri.f] == node->id) {
      m->f.no[tri.f] += n;
    }

    if (node_vattr[v1] == node->id) {
      m->v.no[v1] += n;
    }
    if (node_vattr[v2] == node->id) {
      m->v.no[v2] += n;
    }
    if (node_vattr[v3] == node->id) {
      m->v.no[v3] += n;
    }
  }

  for (int f : node->data->unique_faces) {
    m->f.no[f].normalize();
  }
  for (int v : node->data->unique_verts) {
    m->v.no[v].normalize();
  }
}

bool SpatialTree::update(gpu::GPUManager *gpu)
{
  bool result = false;
  bool bounds = false;
  bool drawBatchUpdated = false;

  for (SpatialNode *node : nodes) {
    if (node->flag & Spatial_RegenBounds) {
      while (node) {
        node->flag |= Spatial_RegenBounds;
        node = node->parent;
        bounds = true;
      }
    }
  }

  if (bounds) {
    printf("SpatialTree::update: bounds\n");
    regen_node_bounds(root, true);
    result = true;
  }

  Vector<SpatialNode *, 256> updateTriNodes;
  for (SpatialNode *node : nodes) {
    if (!(node->flag & Spatial_Leaf)) {
      continue;
    }
    if (node->flag & Spatial_RegenTris) {
      updateTriNodes.append(node);
      drawBatchUpdated = true;
    }
  }

  if (updateTriNodes.size() > 0) {
    printf("SpatialTree::update: tris\n");
  }

#if 0
  litestl::task::parallel_for(
      util::IndexRange(updateTriNodes.size()),
      [&updateTriNodes, this](util::IndexRange range) //
      {
        for (int i : range) {
          SpatialNode *node = updateTriNodes[i];
          ensure_node_tris(node);
        }
      },
      4);
#else
  for (SpatialNode *node : updateTriNodes) {
    ensure_node_tris(node);
  }
#endif

  for (SpatialNode *node : nodes) {
    if (!(node->flag & Spatial_Leaf)) {
      continue;
    }
    if (node->flag & Spatial_UpdateNormals) {
      update_node_normals(node);
      drawBatchUpdated = true;
    }
  }

  for (SpatialNode *node : nodes) {
    if (!(node->flag & Spatial_Leaf)) {
      continue;
    }

    if (node->flag & Spatial_RegenGPU) {
      regen_node_gpu_buffers(node, gpu);
      printf("SpatialTree::update: regenGPU\n");
      drawBatchUpdated = true;
    } else if (node->flag & Spatial_UpdateGPU) {
      printf("SpatialTree::update: updateGPU\n");
      update_node_gpu_buffers(node, gpu);
    }
  }

  if (drawBatchUpdated || !drawBatch) {
    printf("SpatialTree::update: batch\n");
    if (!drawBatch) {
      drawBatch = gpu->createBatch();
    } else {
      drawBatch->clear();
    }

    for (SpatialNode *node : nodes) {
      if (!(node->flag & Spatial_Leaf)) {
        continue;
      }

      auto &nodeGpu = node->data->gpu;
      drawBatch->buffers.append(nodeGpu.pos);
      drawBatch->buffers.append(nodeGpu.nor);

      if (!nodeGpu.cmd) {
        nodeGpu.cmd = gpu->createCommand(drawBatch,
                                         gpu::GPUCmdType::DRAW_TRIS,
                                         &spatialShaders.basicMeshShader,
                                         0,
                                         nodeGpu.pos->size,
                                         nodeGpu.pos->size / 3);
        nodeGpu.cmd->attrs.append(nodeGpu.pos);
        nodeGpu.cmd->attrs.append(nodeGpu.nor);
      }
      nodeGpu.cmd->primCount = nodeGpu.pos->size / 3;
      nodeGpu.cmd->end = nodeGpu.pos->size;
      drawBatch->commands.append(nodeGpu.cmd);
    }
  }

  return result;
}
} // namespace sculptcore::spatial
