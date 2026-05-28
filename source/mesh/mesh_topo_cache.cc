#include "mesh_topo_cache.h"

#include "mesh.h"
#include "mesh_iter.h"

namespace sculptcore::mesh {

bool MeshTopoCache::valid(const Mesh &m) const
{
  return ring1_stamp == m.topo_stamp;
}

const VertNbrCSR &MeshTopoCache::ensureRing1(Mesh &m)
{
  if (ring1_stamp == m.topo_stamp) {
    return ring1;
  }

  const int cap = int(m.v.capacity());
  ring1.count = m.v.count;
  ring1.offsets.resize(size_t(cap) + 1);
  ring1.nbr_verts.clear();

  /* Indexed by raw vertex id so free slots yield an empty range and the GPU /
   * C++ consumers can address neighbors by live vertex index directly. Walk
   * the disk cycle (EdgeOfVertIter) — handles non-manifold disks since it
   * follows the cycle, not a face fan. */
  uint32_t off = 0;
  for (int v = 0; v < cap; v++) {
    ring1.offsets[v] = off;
    if (m.v.freemap[v]) {
      continue;
    }
    int e0 = m.v.e[v];
    if (e0 != ELEM_NONE) {
      for (int e : EdgeOfVertIter(&m, v, e0)) {
        int nb = (m.e.vs[e][0] == v) ? m.e.vs[e][1] : m.e.vs[e][0];
        ring1.nbr_verts.append(nb);
        off++;
      }
    }
  }
  ring1.offsets[cap] = off;

  ring1_stamp = m.topo_stamp;
  return ring1;
}

void FrozenTopo::clear()
{
  edge_v0.clear();
  edge_v1.clear();
  vert_edge_off.clear();
  vert_edges.clear();
  edge_corner_off.clear();
  edge_corners.clear();
  face_list_off.clear();
  face_lists.clear();
  list_corner_off.clear();
  list_corners.clear();
  built = false;
}

void FrozenTopo::build(Mesh &m)
{
  clear();

  const int Vc = int(m.v.capacity());
  const int Ec = int(m.e.capacity());
  const int Lc = int(m.l.capacity());
  const int Fc = int(m.f.capacity());

  /* Edge endpoints, indexed by edge slot. */
  edge_v0.resize(Ec);
  edge_v1.resize(Ec);
  for (int e : m.e) {
    edge_v0[e] = m.e.vs[e][0];
    edge_v1[e] = m.e.vs[e][1];
  }

  /* vert -> incident edges in disk order. */
  vert_edge_off.resize(size_t(Vc) + 1);
  {
    uint32_t off = 0;
    for (int v = 0; v < Vc; v++) {
      vert_edge_off[v] = off;
      if (m.v.freemap[v]) {
        continue;
      }
      int e0 = m.v.e[v];
      if (e0 != ELEM_NONE) {
        for (int e : EdgeOfVertIter(&m, v, e0)) {
          vert_edges.append(e);
          off++;
        }
      }
    }
    vert_edge_off[Vc] = off;
  }

  /* edge -> radial corners in radial order (manual walk; CornerOfEdgeIter does
   * not terminate as a range-for). */
  edge_corner_off.resize(size_t(Ec) + 1);
  {
    uint32_t off = 0;
    for (int e = 0; e < Ec; e++) {
      edge_corner_off[e] = off;
      if (m.e.freemap[e]) {
        continue;
      }
      int c0 = m.e.c[e];
      if (c0 != ELEM_NONE) {
        int cc = c0;
        do {
          edge_corners.append(cc);
          off++;
          cc = m.c.radial_next[cc];
        } while (cc != c0);
      }
    }
    edge_corner_off[Ec] = off;
  }

  /* face -> owned lists in f.l / l.next order. */
  face_list_off.resize(size_t(Fc) + 1);
  {
    uint32_t off = 0;
    for (int f = 0; f < Fc; f++) {
      face_list_off[f] = off;
      if (m.f.freemap[f]) {
        continue;
      }
      int li = m.f.l[f];
      while (li != ELEM_NONE) {
        face_lists.append(li);
        off++;
        li = m.l.next[li];
      }
    }
    face_list_off[Fc] = off;
  }

  /* list -> corners in loop order. */
  list_corner_off.resize(size_t(Lc) + 1);
  {
    uint32_t off = 0;
    for (int l = 0; l < Lc; l++) {
      list_corner_off[l] = off;
      if (m.l.freemap[l]) {
        continue;
      }
      int c0 = m.l.c[l];
      if (c0 != ELEM_NONE) {
        int cc = c0;
        do {
          list_corners.append(cc);
          off++;
          cc = m.c.next[cc];
        } while (cc != c0);
      }
    }
    list_corner_off[Lc] = off;
  }

  built = true;
}

void FrozenTopo::rebuildLinks(Mesh &m)
{
  const int Vc = int(m.v.capacity());
  const int Ec = int(m.e.capacity());
  const int Lc = int(m.l.capacity());
  const int Fc = int(m.f.capacity());

  /* 1. Edge endpoints (needed before disk side computation and c.v derivation). */
  for (int e = 0; e < Ec; e++) {
    if (m.e.freemap[e]) {
      continue;
    }
    m.e.vs[e][0] = edge_v0[e];
    m.e.vs[e][1] = edge_v1[e];
  }

  /* 2. c.e: a corner belongs to exactly the edge whose radial run contains it. */
  for (int e = 0; e < Ec; e++) {
    uint32_t a = edge_corner_off[e], b = edge_corner_off[e + 1];
    for (uint32_t i = a; i < b; i++) {
      m.c.e[edge_corners[i]] = e;
    }
  }

  /* 3. Disk cycles: relink each vertex's incident-edge ring in stored order. */
  for (int v = 0; v < Vc; v++) {
    if (m.v.freemap[v]) {
      continue;
    }
    uint32_t a = vert_edge_off[v], b = vert_edge_off[v + 1];
    int n = int(b - a);
    if (n == 0) {
      m.v.e[v] = ELEM_NONE;
      continue;
    }
    m.v.e[v] = vert_edges[a];
    for (int i = 0; i < n; i++) {
      int e = vert_edges[a + i];
      int eprev = vert_edges[a + (i - 1 + n) % n];
      int enext = vert_edges[a + (i + 1) % n];
      int side = (m.e.vs[e][0] == v) ? 0 : 1;
      m.e.disk[e][side * 2] = eprev;
      m.e.disk[e][side * 2 + 1] = enext;
    }
  }

  /* 4. Radial cycles: relink each edge's corner run in stored order. */
  for (int e = 0; e < Ec; e++) {
    if (m.e.freemap[e]) {
      continue;
    }
    uint32_t a = edge_corner_off[e], b = edge_corner_off[e + 1];
    int n = int(b - a);
    if (n == 0) {
      m.e.c[e] = ELEM_NONE;
      continue;
    }
    m.e.c[e] = edge_corners[a];
    for (int i = 0; i < n; i++) {
      int c = edge_corners[a + i];
      m.c.radial_prev[c] = edge_corners[a + (i - 1 + n) % n];
      m.c.radial_next[c] = edge_corners[a + (i + 1) % n];
    }
  }

  /* 5. Faces -> lists. */
  for (int f = 0; f < Fc; f++) {
    if (m.f.freemap[f]) {
      continue;
    }
    uint32_t a = face_list_off[f], b = face_list_off[f + 1];
    int n = int(b - a);
    m.f.l[f] = (n > 0) ? face_lists[a] : ELEM_NONE;
    m.f.list_count[f] = short(n);
    for (int i = 0; i < n; i++) {
      int li = face_lists[a + i];
      m.l.f[li] = f;
      m.l.next[li] = (i + 1 < n) ? face_lists[a + i + 1] : ELEM_NONE;
    }
  }

  /* 6. Lists -> corners (loop links + c.l + l.size). */
  for (int l = 0; l < Lc; l++) {
    if (m.l.freemap[l]) {
      continue;
    }
    uint32_t a = list_corner_off[l], b = list_corner_off[l + 1];
    int n = int(b - a);
    m.l.c[l] = (n > 0) ? list_corners[a] : ELEM_NONE;
    m.l.size[l] = n;
    for (int i = 0; i < n; i++) {
      int c = list_corners[a + i];
      m.c.l[c] = l;
      m.c.prev[c] = list_corners[a + (i - 1 + n) % n];
      m.c.next[c] = list_corners[a + (i + 1) % n];
    }
  }

  /* 7. c.v: corner ci sits at the vertex shared by its edge and the previous
   *    corner's edge (c.e[ci] runs c.v[ci] -> c.v[next], c.e[prev] ends at
   *    c.v[ci]). Derived from e.vs + the loop links rebuilt above. */
  for (int l = 0; l < Lc; l++) {
    if (m.l.freemap[l]) {
      continue;
    }
    uint32_t a = list_corner_off[l], b = list_corner_off[l + 1];
    int n = int(b - a);
    for (int i = 0; i < n; i++) {
      int c = list_corners[a + i];
      int eThis = m.c.e[c];
      int ePrev = m.c.e[list_corners[a + (i - 1 + n) % n]];
      int a0 = m.e.vs[eThis][0], a1 = m.e.vs[eThis][1];
      int shared = (a0 == m.e.vs[ePrev][0] || a0 == m.e.vs[ePrev][1]) ? a0 : a1;
      m.c.v[c] = shared;
    }
  }
}

} // namespace sculptcore::mesh
