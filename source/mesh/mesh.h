#pragma once

#include "attribute.h"
#include "math/vector.h"
#include "util/string.h"

#include "mesh_base.h"
#include "mesh_enums.h"
#include "mesh_types.h"
#include "mesh_proxy.h"

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

  int make_vertex(math::float3 co)
  {
    int r = v.alloc();

    v.co[r] = co;
    v.e[r] = ELEM_NONE;

    return r;
  }

  EdgeOfVertIter e_of_v(int v1)
  {
    int e1 = v.e[v1];
    return EdgeOfVertIter(this, v1, e1);
  }

  int make_edge(int v1, int v2)
  {
    int r = e.alloc();

    e.c[r] = ELEM_NONE;

    e.vs[r][0] = v1;
    e.vs[r][1] = v2;

    e.disk[r] = math::int4(ELEM_NONE);

    disk_insert(r, v1);
    disk_insert(r, v2);

    return r;
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

  int make_face(std::span<int> verts, std::span<int> edges)
  {
    int f1 = f.alloc();
    int list = l.alloc();

    int vlen = verts.size();

    f.l[f1] = list;
    f.list_count[f1] = 1;
    l.size[list] = vlen;
    l.f[list] = f1;
    l.next[list] = ELEM_NONE;
    
    util::Vector<int, 8> corners;
    for (int i = 0; i < vlen; i++) {
      int c1 = c.alloc();
      
      c.v[c1] = verts[i];
      c.e[c1] = edges[i];
      c.l[c1] = list;

      radial_insert(edges[i], c1);
      corners.append(i);
    }

    l.c[list] = corners[0];

    for (int i = 0; i < vlen; i++) {
      int l1 = corners[(i - 1 + vlen) % vlen];
      int l2 = corners[i];
      int l3 = corners[(i + 1) % vlen];

      c.prev[l2] = l1;
      c.next[l2] = l3;
    }

    return f1;
  }

  void radial_insert(int e1, int c1)
  {
    if (e.c[e1] == ELEM_NONE) {
    } else {
    }
  }

  int make_face(std::span<int> verts)
  {
    util::Vector<int, 6> edges;

    int vlen = verts.size();
    for (int i = 0; i < vlen; i++) {
      int v1 = verts[i], v2 = verts[(i + 1) % vlen];
      int e1 = find_edge(v1, v2);

      if (e1 == ELEM_NONE) {
        e1 = make_edge(v1, v2);
      }

      edges.append(e1);
    }

    return make_face(verts, edges);
  }

private:
  void disk_insert(int e1, int v1)
  {
    int side1 = e.vs[e1][0] == v1 ? 0 : 1;

    if (v.e[v1] == ELEM_NONE) {
      v.e[v1] = e1;

      e.disk[e1][side1 * 2] = e1;
      e.disk[e1][side1 * 2] = e1;

      return;
    }

    int e2 = v.e[v1];
    int side2 = e.vs[e2][0] == v1 ? 0 : 1;

    int prev = e.disk[e2][side2 * 2];
    int side3 = e.vs[prev][0] == v1 ? 0 : 1;

    e.disk[e2][side2 * 2] = e1; /* e2.prev */

    e.disk[e1][side1 * 2] = prev;   /* e1.prev */
    e.disk[e1][side1 * 2 + 1] = e2; /* e1.next */

    e.disk[prev][side3 * 2 + 1] = e1; /* prev.next */
  }
};
}; // namespace sculptcore::mesh
