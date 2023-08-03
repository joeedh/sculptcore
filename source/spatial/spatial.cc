#include "spatial.h"
#pragma once

#include "node.h"

#include "math/vector.h"
#include "util/map.h"
#include "util/vector.h"

#include "mesh/mesh.h"
#include "mesh/mesh_proxy.h"

using namespace sculptcore::util;
using namespace sculptcore::math;
using namespace sculptcore::mesh;

namespace sculptcore::spatial {
void SpatialTree::add_face_intern(SpatialNode *node, int f, float3 &fcent)
{
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

} // namespace sculptcore::spatial
