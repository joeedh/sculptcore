#include "spatial.h"

#include "node.h"

#include "litestl/util/vector.h"
#include "litestl/math/geom.h"
#include "litestl/math/vector.h"
//#include "litestl/util/map.h"

#include "gpu/batch.h"
#include "gpu/command.h"
#include "gpu/manager.h"
#include "gpu/types.h"
#include "gpu/vbo.h"

#include "mesh/mesh.h"
#include "mesh/mesh_proxy.h"

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
ATTR_NO_OPT
void SpatialTree::regen_node_tris(SpatialNode *node)
{
  node->flag &= ~Spatial_RegenTris;

  /* TODO: use a property CDT for > 4 vert or > 1 hole faces.
   * For now just handle triangles and quads.
   */
  node->data->tris.clear_and_contract();
  for (int f : node->data->faces) {
    int l = m->f.l[f];
    int c = m->l.c[l];

    NodeTri &tri = node->data->tris.grow_one();
    tri.c[0] = c;
    tri.c[1] = m->c.next[c];
    tri.c[2] = m->c.next[tri.c[1]];

    if (m->l.size[l] > 3) {
      NodeTri &tri2 = node->data->tris.grow_one();

      tri2.c[0] = c;
      tri2.c[1] = m->c.next[m->c.next[c]];
      tri2.c[2] = m->c.next[tri2.c[1]];
    }
  }
}

[[clang::optnone]]
void SpatialTree::add_face_intern(SpatialNode *node, int f, float3 &fcent)
{
  if ((node->flag & Spatial_Leaf) && node_needs_split(node)) {
    split_node(node);
  }

  if (!(node->flag & Spatial_Leaf)) {
    float mindis = FLT_MAX;
    SpatialNode *newnode = nullptr;
    bool ok = false;
    int newnode_i = 0;

    for (int i = 0; i < 2; i++) {
      SpatialNode *c = node->children[i];

      float3 cent = (c->min + c->max) * 0.5;
      float dis = (cent - fcent).length();

      if (!newnode || dis < mindis) {
        mindis = dis;
        newnode = c;
        newnode_i = i;
      }
    }

    add_face_intern(newnode, f, fcent);
    return;
  }

  node->flag |= Spatial_RegenTris | Spatial_RegenBounds;

  FaceProxy face(m, f);
  treeMesh.f.node[face] = node->id;

  node->data->faces.add(f);

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
  float3 min(FLT_MAX), max(FLT_MIN);
  float3 mean(0.0f);

  for (int v : node->data->other_verts) {
    VertProxy vert(m, v);
    min.min(vert.co());
    max.max(vert.co());
  }

  for (int v : node->data->unique_verts) {
    VertProxy vert(m, v);
    min.min(vert.co());
    max.max(vert.co());
    mean += vert.co();

    /* Unassign verts. */
    treeMesh.v.node[v] = 0;
  }

  mean /= node->data->unique_verts.size();

  float3 size = max - min;
  float3 eps = calc_eps_float3(size);
  int axis = 0;

  #if 0
  for (int i = 0; i < 3; i++) {
    if (size[i] > size[axis]) {
      axis = i;
    }
  }
  #else
  axis = node->depth % 3;
  #endif

  min -= eps;
  max += eps;

  for (int i = 0; i < 2; i++) {
    SpatialNode *child = node->children[i];

    child->flag = Spatial_Leaf | Spatial_RegenTris | Spatial_RegenBounds;
    child->depth = node->depth + 1;

    if (i == 0) {
      child->min = node->min;
      child->max = node->max;
      child->max[axis] = child->min[axis] + size[axis] * 0.5f;
    } else {
      child->min = node->min;
      child->min[axis] = child->min[axis] + size[axis] * 0.5f;
      child->max = node->max;
    }

    child->create_data();
  }

  node->flag &= ~Spatial_Leaf;

  for (int f : node->data->faces) {
    FaceProxy face(m, f);
    float3 fcent = face.calc_center();

    add_face_intern(node, f, fcent);
  }

  node->delete_data();

  regen_node_bounds(node, true);
}

ATTR_NO_OPT
void SpatialTree::regen_node_bounds(SpatialNode *node, bool recurse)
{
  return; // XXX
  node->flag &= ~Spatial_RegenBounds;

  node->min = float3(FLT_MAX);
  node->max = float3(FLT_MIN);

  if (!(node->flag & Spatial_Leaf)) {
    for (int i = 0; i < 2; i++) {
      if (recurse) {
        regen_node_bounds(node->children[i], true);
      }

      node->min.min(node->children[i]->min);
      node->max.max(node->children[i]->max);
    }
  } else {
    if (node->data->unique_verts.size() != 0) {
      node->min = float3(FLT_MAX);
      node->max = float3(FLT_MIN);
    }

    for (int v : node->data->unique_verts) {
      VertProxy vert(m, v);

      float3 &co = vert.co();

      node->min.min(co);
      node->max.max(co);
    }

    for (int f : node->data->faces) {
      FaceProxy face(m, f);

      for (auto list : face.lists()) {
        for (auto c : list) {
          float3 &co = c.v().co();

          node->min.min(co);
          node->max.max(co);
        }
      }
    }

    float3 eps = calc_eps_float3(node->max - node->min);

    node->min -= eps;
    node->max += eps;
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

  m->calcAABB(root->min, root->max);
  float eps = 0.0000001f;
  root->min -= eps;
  root->max += eps;

  int n = m->f.count;
  for (int i = 0; i < n; i++) {
    add_face(i);
  }

  regen_node_bounds(root, true);
}

ATTR_NO_OPT
sculptcore::gpu::DrawBatch *
SpatialTree::buildLeafBoundsBatch(sculptcore::gpu::GPUManager &mgr)
{
  using namespace sculptcore::gpu;

  util::Vector<SpatialNode *> ls = leaves();

  /* 12 edges per box × 2 endpoints = 24 verts per leaf. */
  const int vertsPerLeaf = 24;
  int totalVerts = ls.size() * vertsPerLeaf;

  Buffer *posBuf = mgr.createBuffer(
      litestl::util::string("position"), GPUType::FLOAT32, 3, totalVerts);
  Buffer *colorBuf =
      mgr.createBuffer(litestl::util::string("uv"), GPUType::FLOAT32, 4, totalVerts);
  Buffer *uvBuf =
      mgr.createBuffer(litestl::util::string("color"), GPUType::FLOAT32, 2, totalVerts);

  float3 *pos = posBuf->get_data<float3>();
  float4 *color = colorBuf->get_data<float4>();
  float2 *uv = uvBuf->get_data<float2>();

  int idx = 0;

  auto addLine = [pos, color, uv, &idx](const float3 &a, const float3 &b) {
    pos[idx] = a;
    color[idx] = float4(1.0f, 1.0f, 1.0f, 1.0f);
    uv[idx] = float2(0.0f, 0.0f);
    idx++;
    pos[idx] = b;
    color[idx] = float4(1.0f, 1.0f, 1.0f, 1.0f);
    uv[idx] = float2(1.0f, 1.0f);
    idx++;
  };

  for (SpatialNode *node : ls) {
    printf("node %f %f\n", node->min[0], node->min[1]);

    float3 mn = node->min;
    float3 mx = node->max;
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
    addLine(c[0], c[1]);
    addLine(c[1], c[2]);
    addLine(c[2], c[3]);
    addLine(c[3], c[0]);

    /* Top quad (z = mx). */
    addLine(c[4], c[5]);
    addLine(c[5], c[6]);
    addLine(c[6], c[7]);
    addLine(c[7], c[4]);

    /* Vertical edges. */
    addLine(c[0], c[4]);
    addLine(c[1], c[5]);
    addLine(c[2], c[6]);
    addLine(c[3], c[7]);
  }

  posBuf->dirty();

  printf("pos: %d %f %f %f\n", int(pos), pos[0][0], pos[0][1], pos[0][2]);

  DrawBatch *batch = mgr.createBatch();
  batch->buffers.append(posBuf);
  batch->buffers.append(uvBuf);
  batch->buffers.append(colorBuf);

  DrawCommand *cmd = mgr.createCommand(
      batch, GPUCmdType::DRAW_LINES, nullptr, 0, totalVerts, totalVerts / 2);
  cmd->attrs.append(posBuf);
  cmd->attrs.append(uvBuf);
  cmd->attrs.append(colorBuf);

  return batch;
}

} // namespace sculptcore::spatial
