#pragma once

#include "../mesh.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

/* Box-modeling loop/boundary walk utilities (Milestone 1 of
 * documentation/plans/boxModelingTools.md). Multi-loop (holed-face) aware: face
 * iteration always walks every list (f.l -> l.next), and the radial scans key off
 * corners so a face's hole loops feed in naturally. These are the shared
 * primitives the extrude / inset / loop-cut macro-ops build on. */

namespace sculptcore::mesh {

/* The other radial face of edge `e` besides `f` (ELEM_NONE if `e` is a boundary
 * edge or non-manifold — only well-defined when `e` has exactly two faces). */
static inline int otherFaceOfEdge(Mesh &m, int e, int f)
{
  int c0 = m.e.c[e];
  if (c0 == ELEM_NONE) {
    return ELEM_NONE;
  }
  int found = ELEM_NONE, n = 0, c = c0;
  do {
    int ff = m.l.f[m.c.l[c]];
    n++;
    if (ff != f) {
      found = ff;
    }
    c = m.c.radial_next[c];
  } while (c != c0);
  return n == 2 ? found : ELEM_NONE;
}

/* The edge of quad face `f` opposite to edge `e` (sharing no vertex). ELEM_NONE
 * if `f` is not a quad or `e` is not one of its edges. */
static inline int oppositeEdgeInQuad(Mesh &m, int f, int e)
{
  int l = m.f.l[f];
  if (m.l.size[l] != 4) {
    return ELEM_NONE;
  }
  int c0 = m.l.c[l], c = c0;
  do {
    if (m.c.e[c] == e) {
      return m.c.e[m.c.next[m.c.next[c]]];
    }
    c = m.c.next[c];
  } while (c != c0);
  return ELEM_NONE;
}

/* Boundary edges of the current selected-face region: an edge with exactly one
 * selected radial face. Covers interior, mesh-boundary, and single-face cases.
 * Appends edge indices to `out`. The macro-ops (extrude/inset) bridge along this
 * set; a later pass orders it into loops. */
static inline void regionBoundaryEdges(Mesh &m, litestl::util::Vector<int> &out)
{
  auto *fsel = m.f.select.get_data();
  for (int e : m.e) {
    int c0 = m.e.c[e];
    if (c0 == ELEM_NONE) {
      continue;
    }
    int sel = 0, c = c0;
    do {
      int f = m.l.f[m.c.l[c]];
      if (fsel->get(f)) {
        sel++;
      }
      c = m.c.radial_next[c];
    } while (c != c0);
    if (sel == 1) {
      out.append(e);
    }
  }
}

/* Walk the edge ring through `eStart`: the chain of "parallel" edges across a
 * strip of quads (each edge's opposite edge in the adjacent quad), in both
 * directions. Stops at non-quads, boundaries, or when the ring closes. Appends
 * the ring (including `eStart`) to `out`. This is the set loop-cut splits. */
static inline void walkEdgeRing(Mesh &m, int eStart, litestl::util::Vector<int> &out)
{
  int c0 = m.e.c[eStart];
  if (c0 == ELEM_NONE) {
    return;
  }
  litestl::util::Set<int> seen;
  out.append(eStart);
  seen.add(eStart);

  int startFaces[2];
  int nsf = 0;
  {
    int c = c0;
    do {
      if (nsf < 2) {
        startFaces[nsf++] = m.l.f[m.c.l[c]];
      }
      c = m.c.radial_next[c];
    } while (c != c0);
  }

  for (int d = 0; d < nsf; d++) {
    int cur = eStart, f = startFaces[d];
    while (true) {
      int opp = oppositeEdgeInQuad(m, f, cur);
      if (opp == ELEM_NONE || seen.contains(opp)) {
        break;
      }
      seen.add(opp);
      out.append(opp);
      int nf = otherFaceOfEdge(m, opp, f);
      if (nf == ELEM_NONE) {
        break;
      }
      cur = opp;
      f = nf;
    }
  }
}

/* Walk the face loop (strip of quads) crossed by the edge ring through `eStart`.
 * Appends face indices to `out`. */
static inline void walkFaceLoop(Mesh &m, int eStart, litestl::util::Vector<int> &out)
{
  int c0 = m.e.c[eStart];
  if (c0 == ELEM_NONE) {
    return;
  }
  litestl::util::Set<int> seen;
  int startFaces[2];
  int nsf = 0;
  {
    int c = c0;
    do {
      if (nsf < 2) {
        startFaces[nsf++] = m.l.f[m.c.l[c]];
      }
      c = m.c.radial_next[c];
    } while (c != c0);
  }

  for (int d = 0; d < nsf; d++) {
    int cur = eStart, f = startFaces[d];
    while (f != ELEM_NONE && !seen.contains(f) && m.l.size[m.f.l[f]] == 4) {
      seen.add(f);
      out.append(f);
      int opp = oppositeEdgeInQuad(m, f, cur);
      if (opp == ELEM_NONE) {
        break;
      }
      int nf = otherFaceOfEdge(m, opp, f);
      cur = opp;
      f = nf;
    }
  }
}

/* Gather every vertex touched by any selected element (selected verts, both ends
 * of selected edges, all corners of selected faces incl. hole loops), deduped.
 * The transform bridge's "movable" set for a grab of the current selection. */
static inline void gatherMovableVerts(Mesh &m, litestl::util::Vector<int> &out)
{
  auto *vsel = m.v.select.get_data();
  auto *esel = m.e.select.get_data();
  auto *fsel = m.f.select.get_data();
  litestl::util::Set<int> seen;

  for (int vi : m.v) {
    if (vsel->get(vi)) {
      seen.add(vi);
    }
  }
  for (int ei : m.e) {
    if (esel->get(ei)) {
      seen.add(m.e.vs[ei][0]);
      seen.add(m.e.vs[ei][1]);
    }
  }
  for (int fi : m.f) {
    if (!fsel->get(fi)) {
      continue;
    }
    for (int l = m.f.l[fi]; l != ELEM_NONE; l = m.l.next[l]) {
      int cc0 = m.l.c[l], cc = cc0;
      do {
        seen.add(m.c.v[cc]);
        cc = m.c.next[cc];
      } while (cc != cc0);
    }
  }
  for (int vi : seen) {
    out.append(vi);
  }
}

} // namespace sculptcore::mesh
