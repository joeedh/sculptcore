#include "mesh.h"

#include "util/map.h"
#include "util/vector.h"

namespace sculptcore::mesh {

int Mesh::make_vertex(math::float3 co)
{
  int r = v.alloc();

  v.co[r] = co;
  v.e[r] = ELEM_NONE;

  return r;
}

int Mesh::make_edge(int v1, int v2)
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

int Mesh::make_face(std::span<int> verts, std::span<int> edges)
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

int Mesh::make_face(std::span<int> verts)
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

void Mesh::kill_vertex(int v1)
{
  while (v.e[v1]) {
    kill_edge(v.e[v1]);
  }

  v.release(v1);
}

void Mesh::kill_edge(int e1)
{
  while (e.c[e1]) {
    kill_face(l.f[c.l[e.c[e1]]]);
  }

  disk_remove(e1, e.vs[e1][0]);
  disk_remove(e1, e.vs[e1][1]);

  e.release(e1);
}

void Mesh::kill_face(int f1)
{
  int l1 = f.l[f1];
  while (l1 != ELEM_NONE) {
    int next = l.next[l1];

    int c1 = l.c[l1], startc1 = c1;
    int cnext;
    do {
      cnext = c.next[c1];

      radial_remove(c.e[c1], c1);

      c.release(c1);
    } while ((c1 = cnext) != startc1);

    l.release(l1);
    l1 = next;
  }

  f.release(f1);
}

void Mesh::reorder_verts(util::span<int> vmap)
{
  for (int e1 : e) {
    e.vs[e1][0] = vmap[e.vs[e1][0]];
    e.vs[e1][1] = vmap[e.vs[e1][1]];
  }

  for (int c1 : c) {
    c.v[c1] = vmap[c.v[c1]];
  }

  v.attrs.reorder(vmap);
}

} // namespace sculptcore::mesh
