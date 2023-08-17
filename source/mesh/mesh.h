#pragma once

#include "attribute.h"
#include "math/vector.h"
#include "util/string.h"
#include "util/span.h"

#include "mesh_base.h"
#include "mesh_enums.h"
#include "mesh_proxy.h"
#include "mesh_types.h"

#include <algorithm>
#include <concepts>
#include <cstdio>
#include <type_traits>

#include <span>

namespace sculptcore::mesh {

struct Mesh : public MeshBase {
  Mesh()
  {
  }

  int make_vertex(math::float3 co);
  int make_edge(int v1, int v2);
  int make_face(std::span<int> verts, std::span<int> edges);
  int make_face(std::span<int> verts);

  void kill_vertex(int v);
  void kill_edge(int e);
  void kill_face(int f);

  EdgeOfVertIter e_of_v(int v1)
  {
    int e1 = v.e[v1];
    return EdgeOfVertIter(this, v1, e1);
  }

  inline int edge_side(int e1, int v1)
  {
    return v1 == e.vs[e1][0] ? 0 : 1;
  }

  int find_edge(int v1, int v2)
  {
    for (int e1 : e_of_v(v1)) {
      for (int e2 : e_of_v(v2)) {
        if (e1 == e2) {
          return e1;
        }
      }
    }

    return ELEM_NONE;
  }

  void reorder_verts(util::span<int> vertex_map);

private:
  void radial_insert(int e1, int c1)
  {
    if (e.c[e1] == ELEM_NONE) {
      e.c[e1] = c1;
      c.radial_next[c1] = c.radial_prev[c1] = c1;
    } else {
      int c2 = e.c[e1];
      int c2prev = c.radial_prev[c2];

      c.radial_prev[c1] = c2prev;
      c.radial_next[c1] = c2;

      c.radial_next[c2prev] = c1;
      c.radial_prev[c2] = c1;
    }
  }

  void radial_remove(int e1, int c1)
  {
    if (e.c[e1] == c1) {
      e.c[e1] = c.radial_next[c1];
    }
    if (e.c[e1] == c1) {
      e.c[e1] = ELEM_NONE;
    }

    int next = c.radial_next[c1];
    int prev = c.radial_prev[c1];

    c.radial_next[prev] = next;
    c.radial_prev[next] = prev;
  }

  void disk_insert(int e1, int v1)
  {
    int side1 = edge_side(e1, v1);

    if (v.e[v1] == ELEM_NONE) {
      v.e[v1] = e1;

      e.disk[e1][side1 * 2] = e1;
      e.disk[e1][side1 * 2 + 1] = e1;

      return;
    }

    int e2 = v.e[v1];
    int side2 = edge_side(e2, v1);

    int prev = e.disk[e2][side2 * 2];
    int side3 = edge_side(prev, v1);

    e.disk[e2][side2 * 2] = e1; /* e2.prev */

    e.disk[e1][side1 * 2] = prev;   /* e1.prev */
    e.disk[e1][side1 * 2 + 1] = e2; /* e1.next */

    e.disk[prev][side3 * 2 + 1] = e1; /* prev.next */
  }

  void disk_remove(int e1, int v1)
  {
    int side1 = edge_side(e1, v1);

    int prev = e.disk[e1][side1 * 2];
    int next = e.disk[e1][side1 * 2 + 1];

    int sidep = edge_side(prev, v1);
    int siden = edge_side(next, v1);

    e.disk[prev][sidep * 2 + 1] = next; /* prev->next */
    e.disk[next][siden * 2] = prev;     /* next->prev */

    if (e1 == v.e[v1]) {
      v.e[v1] = next;
    }

    if (e1 == v.e[v1]) {
      v.e[v1] = ELEM_NONE;
    }
  }
};
}; // namespace sculptcore::mesh
