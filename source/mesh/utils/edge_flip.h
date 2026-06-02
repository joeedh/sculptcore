#pragma once

/* Edge flip (2-2 flip): replaces the shared edge of two triangles with the
 * other diagonal of their quad. For triangles (a,b,c) and (b,a,d) sharing edge
 * a-b (apexes c and d), the flip removes a-b and adds c-d, yielding (c,a,d) and
 * (d,b,c) — preserving winding/orientation.
 *
 * Counts are unchanged (dV=dE=dF=0); only connectivity changes. The flip is
 * refused (mesh untouched) when the edge is not an interior manifold edge with
 * exactly two triangle faces, when the apexes coincide, or when an edge c-d
 * already exists (the flip would create a duplicate / non-manifold edge).
 * Geometric validity (quad convexity) is the caller's call — the remesher flips
 * by a Delaunay / valence criterion; this operator does the topological flip.
 *
 * Implementation mirrors edge_split.h: reconstruct via the public Euler
 * operators rather than splicing cycles by hand.
 */

#include "../mesh.h"
#include "../mesh_base.h"
#include "../mesh_iter.h"

#include "litestl/util/error.h"
#include "litestl/util/vector.h"

#include <span>

namespace sculptcore::mesh {

using litestl::util::SuccessOrError;

struct EdgeFlipResult {
  int created_edge = ELEM_NONE; /* the new c-d diagonal */
  int killed_edge = ELEM_NONE;  /* the old a-b edge */
  litestl::util::Vector<int> created_faces;
  litestl::util::Vector<int> killed_faces;
};

/* Flip `edge`. Returns false (leaving the mesh untouched) for an invalid/free
 * edge, a non-interior or non-triangle edge, coincident apexes, or a pre-
 * existing c-d edge. */
static inline SuccessOrError<"edge_flip", "failed to flip edge">
flipEdge(Mesh &m, int edge, EdgeFlipResult *out = nullptr,
         MeshCallbacks *cb = nullptr)
{
  using namespace litestl::util;

  if (edge < 0 || edge >= int(m.e.capacity()) || m.e.freemap[edge]) {
    return false;
  }

  int a = m.e.vs[edge][0];
  int b = m.e.vs[edge][1];
  if (a == b) {
    return false;
  }

  /* Walk the radial cycle: require exactly two triangle faces and recover the
   * apex on each side (which way the edge winds in each face). */
  int c = ELEM_NONE, d = ELEM_NONE; /* apex of the a->b face / the b->a face */
  int f_ab = ELEM_NONE, f_ba = ELEM_NONE;
  int nfaces = 0;
  int c0 = m.e.c[edge];
  if (c0 == ELEM_NONE) {
    return false; /* wire edge */
  }
  {
    int cc = c0;
    do {
      int li = m.c.l[cc];
      if (m.l.size[li] != 3 || m.f.list_count[m.l.f[li]] != 1) {
        return false; /* non-triangle incident face */
      }
      int cn = m.c.next[cc];
      int cnn = m.c.next[cn];
      int va = m.c.v[cc], vb = m.c.v[cn], apex = m.c.v[cnn];
      if (va == a && vb == b) {
        c = apex;
        f_ab = m.l.f[li];
      } else if (va == b && vb == a) {
        d = apex;
        f_ba = m.l.f[li];
      }
      nfaces++;
      cc = m.c.radial_next[cc];
    } while (cc != c0);
  }

  if (nfaces != 2 || c == ELEM_NONE || d == ELEM_NONE || c == d) {
    return false; /* boundary / non-manifold / degenerate */
  }
  /* A c-d edge already present would be duplicated by the flip. */
  if (m.find_edge(c, d) != ELEM_NONE) {
    return false;
  }

  if (out) {
    out->killed_edge = edge;
    out->killed_faces.append(f_ab);
    out->killed_faces.append(f_ba);
  }

  m.kill_face(f_ab, cb);
  m.kill_face(f_ba, cb);
  m.kill_edge(edge, cb); /* now wire */

  int t0[3] = {c, a, d};
  int t1[3] = {d, b, c};
  int nf0 = m.make_face(std::span<int>(t0, 3), cb);
  int nf1 = m.make_face(std::span<int>(t1, 3), cb);

  if (out) {
    out->created_faces.append(nf0);
    out->created_faces.append(nf1);
    out->created_edge = m.find_edge(c, d);
  }

  return true;
}

} // namespace sculptcore::mesh
