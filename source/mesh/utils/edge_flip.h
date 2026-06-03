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
#include "attr_interp.h"

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

  /* Snapshot both faces' attr rows and their per-corner rows (keyed by vertex)
   * before the kill, so the rebuilt faces keep their `group` / `uv` / etc.
   * instead of make_face's value-init — otherwise every interior flip would
   * zero the polygroup of the two faces it touches. dyntopo refuses to flip
   * feature edges (seam / sharp / poly-group / UV-chart boundaries), so the two
   * faces share a value across any layer that could differ at a boundary; the
   * f_ab->nf0 / f_ba->nf1 assignment is therefore unambiguous in practice. */
  AttrRowSnapshot snapFab, snapFba;
  snapshotAttrRow(m.f.attrs, f_ab, snapFab);
  snapshotAttrRow(m.f.attrs, f_ba, snapFba);
  struct CornerSnap {
    int vert;
    AttrRowSnapshot snap;
  };
  litestl::util::Vector<CornerSnap, 6> csnaps;
  auto snapCorners = [&](int f) {
    int li = m.f.l[f];
    int lc0 = m.l.c[li], lcc = lc0;
    do {
      int v = m.c.v[lcc];
      bool have = false;
      for (CornerSnap &cs : csnaps) {
        if (cs.vert == v) {
          have = true;
          break;
        }
      }
      if (!have) {
        CornerSnap cs;
        cs.vert = v;
        snapshotAttrRow(m.c.attrs, lcc, cs.snap);
        csnaps.append(std::move(cs));
      }
      lcc = m.c.next[lcc];
    } while (lcc != lc0);
  };
  snapCorners(f_ab);
  snapCorners(f_ba);

  m.kill_face(f_ab, cb);
  m.kill_face(f_ba, cb);
  m.kill_edge(edge, cb); /* now wire */

  int t0[3] = {c, a, d};
  int t1[3] = {d, b, c};
  int nf0 = m.make_face(std::span<int>(t0, 3), cb);
  int nf1 = m.make_face(std::span<int>(t1, 3), cb);

  restoreAttrRow(m.f.attrs, nf0, snapFab);
  restoreAttrRow(m.f.attrs, nf1, snapFba);
  auto restoreCorners = [&](int f) {
    int li = m.f.l[f];
    int lc0 = m.l.c[li], lcc = lc0;
    do {
      int v = m.c.v[lcc];
      for (CornerSnap &cs : csnaps) {
        if (cs.vert == v) {
          restoreAttrRow(m.c.attrs, lcc, cs.snap);
          break;
        }
      }
      lcc = m.c.next[lcc];
    } while (lcc != lc0);
  };
  restoreCorners(nf0);
  restoreCorners(nf1);

  if (out) {
    out->created_faces.append(nf0);
    out->created_faces.append(nf1);
    out->created_edge = m.find_edge(c, d);
  }

  return true;
}

} // namespace sculptcore::mesh
