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
 * Implementation: the flip is performed IN PLACE — the shared edge and both
 * faces (and all six corners) keep their ids; only their topology pointers are
 * rewired. This avoids the kill+recreate of the previous version (no attr-row
 * snapshot/restore of the rebuilt faces, no make_face find_edge walks) and,
 * crucially, fires no face create/kill events, so the spatial tree only re-flags
 * the two reused leaves (touch_face) instead of churning its node ownership.
 *
 * The corner remap is chosen so every corner keeps its EDGE unchanged, which
 * means NO radial surgery is needed (radial cycles are keyed by corner id):
 * naming the f_ab corners p0(v=a,e=ab) p1(v=b) p2(v=c) and the f_ba corners
 * q0(v=b,e=ba) q1(v=a) q2(v=d), the new faces are f_ab=(c,a,d) via p2->q1->p0
 * and f_ba=(d,b,c) via q2->p1->q0. Only p0/q0 change vertex (a->d, b->c), only
 * q1/p1 change face, and the shared edge's endpoints move a,b -> c,d.
 */

#include "../mesh.h"
#include "../mesh_base.h"
#include "../mesh_iter.h"
#include "attr_interp.h"

#include "litestl/util/error.h"
#include "litestl/util/function.h"
#include "litestl/util/vector.h"

#include <span>

namespace sculptcore::mesh {

using litestl::util::SuccessOrError;

/* In-place flip: the edge and both faces keep their ids. created_* and killed_*
 * therefore report the SAME (reused) ids — `edge` now holds the c-d diagonal,
 * and {f_ab, f_ba} are the two rewired faces. */
struct EdgeFlipResult {
  int created_edge = ELEM_NONE; /* the new c-d diagonal (== the input edge id) */
  int killed_edge = ELEM_NONE;  /* the old a-b edge (== the input edge id) */
  litestl::util::Vector<int, 8> created_faces;
  litestl::util::Vector<int, 8> killed_faces;
};

/* Flip `edge`. Returns false (leaving the mesh untouched) for an invalid/free
 * edge, a non-interior or non-triangle edge, coincident apexes, or a pre-
 * existing c-d edge. */
static inline SuccessOrError<"edge_flip", "failed to flip edge">
flipEdge(Mesh &m, int edge, EdgeFlipResult *out = nullptr, MeshCallbacks *cb = nullptr)
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

  /* Walk the radial cycle: require exactly two triangle faces and recover, for
   * each side, the apex and the six corners (which way the edge winds in each
   * face). p* are the a->b face's corners, q* the b->a face's. */
  int c = ELEM_NONE, d = ELEM_NONE;
  int f_ab = ELEM_NONE, f_ba = ELEM_NONE;
  int p0 = ELEM_NONE, p1 = ELEM_NONE, p2 = ELEM_NONE;
  int q0 = ELEM_NONE, q1 = ELEM_NONE, q2 = ELEM_NONE;
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
        p0 = cc;  /* v=a, e=edge */
        p1 = cn;  /* v=b */
        p2 = cnn; /* v=c */
      } else if (va == b && vb == a) {
        d = apex;
        f_ba = m.l.f[li];
        q0 = cc;  /* v=b, e=edge */
        q1 = cn;  /* v=a */
        q2 = cnn; /* v=d */
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

  int list_ab = m.f.l[f_ab];
  int list_ba = m.f.l[f_ba];

  /* p0 ends up at vertex d, q0 at vertex c. Carry the destination vertex's
   * corner attrs (uv etc.) onto them: q2 is the existing corner at d, p2 the
   * one at c. dyntopo refuses to flip feature / uv-chart-boundary edges, so the
   * per-vertex corner value is consistent across the two faces. snapshot before
   * any mutation (restoreAttrRow copies value attrs only, skipping TOPO). */
  AttrRowSnapshot snapD, snapC;
  snapshotAttrRow(m.c.attrs, q2, snapD);
  snapshotAttrRow(m.c.attrs, p2, snapC);

  /* Fire all Change events whose element's TOPO row we are about to rewrite
   * BEFORE the rewrite, so the meshlog captures the pre-flip state (its onChange
   * snapshots at first touch and swaps on undo). Corners and lists must be
   * pre-state; the edge + verts are handled inside relink_edge_verts; the two
   * faces fire AFTER (their row is unchanged, and the tree's touch_face must see
   * the new verts). */
  if (cb) {
    auto fire = [](const litestl::util::function<void(int)> &fn, int i) {
      if (fn)
        fn(i);
    };
    fire(cb->onCornerChange, p0);
    fire(cb->onCornerChange, p1);
    fire(cb->onCornerChange, p2);
    fire(cb->onCornerChange, q0);
    fire(cb->onCornerChange, q1);
    fire(cb->onCornerChange, q2);
    fire(cb->onListChange, list_ab);
    fire(cb->onListChange, list_ba);
  }

  /* Move the shared edge's endpoints a,b -> c,d (disk surgery + edge/vert
   * callbacks, with make_edge/kill_edge discipline). Corners keep referencing
   * it, so its radial cycle is untouched. */
  m.relink_edge_verts(edge, c, d, cb);

  /* Vertex remap of the two edge-corners. */
  m.c.v[p0] = d;
  m.c.v[q0] = c;

  /* Face remap of the two non-edge corners that swap sides. */
  m.c.l[q1] = list_ab;
  m.c.l[p1] = list_ba;

  /* Relink the two triangle loops: f_ab = p2(c)->q1(a)->p0(d). */
  m.c.next[p2] = q1;
  m.c.prev[q1] = p2;
  m.c.next[q1] = p0;
  m.c.prev[p0] = q1;
  m.c.next[p0] = p2;
  m.c.prev[p2] = p0;
  /* f_ba = q2(d)->p1(b)->q0(c). */
  m.c.next[q2] = p1;
  m.c.prev[p1] = q2;
  m.c.next[p1] = q0;
  m.c.prev[q0] = p1;
  m.c.next[q0] = q2;
  m.c.prev[q2] = q0;

  /* List heads must name a corner still in the (rewired) list. */
  m.l.c[list_ab] = p2;
  m.l.c[list_ba] = q2;

  /* Corner value attrs follow the destination vertex. */
  restoreAttrRow(m.c.attrs, p0, snapD);
  restoreAttrRow(m.c.attrs, q0, snapC);

  /* Faces survive in place; fire after the rewire so the spatial tree's
   * touch_face reads the new vertex set (the row itself is unchanged, so the
   * meshlog snapshot is order-insensitive here). */
  if (cb) {
    auto fire = [](const litestl::util::function<void(int)> &fn, int i) {
      if (fn)
        fn(i);
    };
    fire(cb->onFaceChange, f_ab);
    fire(cb->onFaceChange, f_ba);
  }

  if (out) {
    out->created_edge = edge;
    out->killed_edge = edge;
    out->created_faces.append(f_ab);
    out->created_faces.append(f_ba);
    out->killed_faces.append(f_ab);
    out->killed_faces.append(f_ba);
  }

  return true;
}

} // namespace sculptcore::mesh
