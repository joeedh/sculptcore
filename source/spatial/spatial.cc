#include "spatial.h"

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
  for (int f : node->data->unique_faces) {
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

    child->flag = Spatial_Leaf | Spatial_RegenTris | Spatial_RegenBounds;
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

  regen_node_bounds(node, true);
}

ATTR_NO_OPT
void SpatialTree::regen_node_bounds(SpatialNode *node, bool recurse)
{
  return; // XXX
  node->flag &= ~Spatial_RegenBounds;

  node->aabb.reset();

  if (!(node->flag & Spatial_Leaf)) {
    for (int i = 0; i < 2; i++) {
      if (recurse) {
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
    float4 clr(0.0);
    clr[0] = rnd.get_float();
    clr[1] = rnd.get_float();
    clr[2] = rnd.get_float();
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

  printf("pos: %d %f %f %f\n", int(pos), pos[0][0], pos[0][1], pos[0][2]);

  DrawBatch *batch = mgr.createBatch();
  batch->buffers.append(posBuf);
  batch->buffers.append(colorBuf);
  batch->buffers.append(uvBuf);

  DrawCommand *cmd = mgr.createCommand(
      batch, GPUCmdType::DRAW_LINES, nullptr, 0, totalVerts, totalVerts / 2);
  cmd->attrs.append(posBuf);
  cmd->attrs.append(colorBuf);
  cmd->attrs.append(uvBuf);

  return batch;
}

} // namespace sculptcore::spatial
