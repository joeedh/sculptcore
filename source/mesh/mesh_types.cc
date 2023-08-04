#include "mesh_types.h"
#include "util/vector.h"

namespace sculptcore::mesh {
void relink_vert_edges(Mesh *m, int vold, int vnew)
{
}

void VertData::swap_elems(int v1, int v2)
{
  Vector<int, 64> edges;

  int v = i ? v2 : v1;
  VertProxy vert(v1);

  int tag = -2;

  /* Relink vertices. */

  /* Replace v1 with -2 */
  for (auto edge : vert.edges()) {
    if (edge.v1() == v1) {
      edge.v1() = tag;
    } else {
      edge.v2() = tag;
    }

    for (auto c : edge.corners()) {
      for (auto c2 : c.list()) {
        if (c2.v() == v1) {
          c2.v() = tag;
        }
      }
    }
  }

  /* Replace v2 with v1. */
  vert = v2;
  for (auto edge : vert.edges()) {
    if (edge.v1() == v2) {
      edge.v1() = v1;
    } else {
      edge.v2() = v1;
    }

    for (auto c : edge.corners()) {
      for (auto c2 : c.list()) {
        if (c2.v() == v2) {
          c2.v() = v1;
        }
      }
    }
  }

  /* Replace -2 with v2. */
  vert = v1;
  for (auto edge : vert.edges()) {
    if (edge.v1() == tag) {
      edge.v1() = v2;
    } else {
      edge.v2() = v2;
    }

    for (auto c : edge.corners()) {
      for (auto c2 : c.list()) {
        if (c2.v() == tag) {
          c2.v() = v2;
        }
      }
    }
  }

  /* Swap element attrs. */
  swap_attrs(v1, v2);
}
} // namespace sculptcore::mesh
