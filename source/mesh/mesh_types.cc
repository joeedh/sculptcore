#include "mesh_types.h"
#include "mesh.h"

#include "util/assert.h"
#include "util/vector.h"

namespace sculptcore::mesh {
using util::Vector;

void relink_vert_edges(Mesh *m, int vold, int vnew)
{
}

void VertexData::splice(Mesh *m, int v, int vnew, int vtarget)
{
  Vector<int, 32> edges;

  if (vtarget == -1) {
    vtarget = v;
  }

  for (int e : m->e_of_v(v)) {
    edges.append(e);
  }

  for (int e : edges) {
    int side = m->edge_side(e, vtarget);

    m->e.vs[e][side] = vnew;

    int cfirst = m->e.c[e];
    if (cfirst == ELEM_NONE) {
      continue;
    }

    int c = cfirst;
    do {
      if (m->c.v[c] == vtarget) {
        m->c.v[c] = vnew;
      } else if (m->c.v[m->c.next[c]] == vtarget) {
        m->c.v[m->c.next[c]] = vnew;
      }
    } while ((c = m->c.radial_next[c]) != cfirst);
  }
}

void VertexData::move_elem(Mesh *m, int vsrc, int vdst)
{
  Vector<int, 64> edges;
  VertProxy vert(m, vsrc);

  splice(m, vsrc, vdst);
  swap_attrs(vsrc, vdst);

  ElemData::alloc(vdst, false);
  ElemData::release(vsrc);
}

void VertexData::swap_elems(Mesh *m, int v1, int v2)
{
  Vector<int, 32> edges;

  bool free1 = freemap[v1], free2 = freemap[v2];
  if (free1 && free2) {
    return; /* Both elements are freed. */
  }

  if (!free1 && free2) {
    std::swap(v1, v2);
    std::swap(free1, free2);
  }

  if (!free2 && free1) {
    move_elem(m, v2, v1);
    return;
  }

  splice(m, v1, -2);     /* Swap refs to v1 with -2. */
  splice(m, v2, v1);     /* Swap refs to v2 with v1. */
  splice(m, v1, v2, -2); /* Swap refs to -2 with v2. */

  swap_attrs(v1, v2);

  ElemData::on_swap.exec(m, v1, v2);
}

} // namespace sculptcore::mesh
