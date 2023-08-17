#include "spatial.h"

#include "node.h"

#include "math/vector.h"
#include "util/map.h"
#include "util/vector.h"

#include "mesh/mesh.h"
#include "mesh/mesh_proxy.h"

#include <cmath>

using namespace sculptcore::util;
using namespace sculptcore::math;
using namespace sculptcore::mesh;

static inline float3 calc_eps_float3(float3 size)
{
  float3 eps = size * 0.001f;

  for (int i = 0; i < 3; i++) {
    eps[i] = std::min(eps[i], 0.00001f);
  }

  return eps;
}

namespace sculptcore::spatial {
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

void SpatialTree::add_face_intern(SpatialNode *node, int f, float3 &fcent)
{
  if ((node->flag & Spatial_Leaf) && node_needs_split(node)) {
    split_node(node);
  }

  if (!(node->flag & Spatial_Leaf)) {
    float mindis = FLT_MAX;
    SpatialNode *newnode = nullptr;

    for (int i = 0; i < 2; i++) {
      SpatialNode *c = node->children[i];

      float3 cent = (c->min + c->max) * 0.5;
      float dis = (cent - fcent).length();

      if (dis < mindis) {
        mindis = dis;
        newnode = c;
      }
    }

    add_face_intern(newnode, f, fcent);
    return;
  }

  node->flag |= Spatial_RegenTris | Spatial_RegenBounds;

  FaceProxy face(m, f);
  face_node[face] = node->id;

  for (auto list : face.lists()) {
    for (auto c : list) {
      if (vert_node[c.v()]) {
        node->data->other_verts.add(c.v());
      } else {
        node->data->unique_verts.add(c.v());
        vert_node[c.v()] = node->id;
      }
    }
  }
}

void SpatialTree::split_node(SpatialNode *node)
{
  node->children[0] = alloc_node();
  node->children[1] = alloc_node();

  using namespace sculptcore::math;
  float3 min(FLT_MAX), max(FLT_MIN);
  float3 mean(0.0f);

  for (int v : node->data->unique_verts) {
    min.min(v);
    max.max(v);
    mean += v;

    /* Unassign verts. */
    vert_node[v] = 0;
  }

  mean /= node->data->unique_verts.size();

  float3 size = max - min;
  float3 eps = calc_eps_float3(size);
  int axis = 0;

  for (int i = 0; i < 3; i++) {
    if (size[i] > size[axis]) {
      axis = i;
    }
  }

  min -= eps;
  max += eps;

  for (int i = 0; i < 2; i++) {
    SpatialNode *child = node->children[i];

    child->flag = Spatial_Leaf | Spatial_RegenTris | Spatial_RegenBounds;

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

void SpatialTree::regen_node_bounds(SpatialNode *node, bool recurse)
{
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

} // namespace sculptcore::spatial
