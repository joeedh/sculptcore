#pragma once

#include "../mesh.h"
#include "../mesh_callbacks.h"
#include "../utils/attr_interp.h"
#include "../utils/modeling_walk.h" // walkEdgeRing / walkFaceLoop
#include "litestl/math/vector.h"
#include "litestl/util/map.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <span>

/* Box-modeling loop cut (Milestone 4). A single midpoint cut across the quad strip
 * through a seed edge: each ring edge gets a midpoint vert, each loop quad is split
 * into two quads by the new cut edge, and any non-loop face bordering a ring edge
 * gets the midpoint inserted into its loop (no T-junction at an open strip end).
 * The new loop verts are left selected (grab/slide them afterward). */

namespace sculptcore::mesh::ops {

// Shared implementation lives in utils/modeling_walk.h; re-exposed for ops:: callers.
using sculptcore::mesh::faceEdgeNearestPoint;

/* Loop-cut the quad strip through `seedEdge`. Appends the created midpoint verts
 * (the new loop) to outVerts and leaves them selected. */
static inline void loopCut(Mesh &m,
                           MeshCallbacks *cb,
                           int seedEdge,
                           litestl::util::Vector<int> &outVerts)
{
  using litestl::util::Map;
  using litestl::util::Set;
  using litestl::util::Vector;

  if (seedEdge == ELEM_NONE || m.e.freemap[seedEdge]) {
    return;
  }

  Vector<int> ring, faceLoop;
  walkEdgeRing(m, seedEdge, ring);
  walkFaceLoop(m, seedEdge, faceLoop);
  if (ring.size() == 0 || faceLoop.size() == 0) {
    return;
  }

  Set<int> ringSet, loopSet;
  for (int e : ring) {
    ringSet.add(e);
  }
  for (int f : faceLoop) {
    loopSet.add(f);
  }

  // Midpoint vert per ring edge.
  Map<int, int> emap;
  for (int e : ring) {
    int va = m.e.vs[e][0], vb = m.e.vs[e][1];
    int mid = m.make_vertex((m.v.co[va] + m.v.co[vb]) * 0.5f, cb);
    interpAttrs(m.v.attrs, mid, va, vb, 0.5f);
    emap.insert(e, mid);
    outVerts.append(mid);
  }

  // Split each loop quad into two quads along the cut.
  for (int f : faceLoop) {
    if (m.f.freemap[f] || m.l.size[m.f.l[f]] != 4) {
      continue;
    }
    int l = m.f.l[f], c0 = m.l.c[l], cA = ELEM_NONE, c = c0;
    do {
      if (ringSet.contains(m.c.e[c])) {
        cA = c;
        break;
      }
      c = m.c.next[c];
    } while (c != c0);
    if (cA == ELEM_NONE) {
      continue;
    }
    int cB = m.c.next[cA], cC = m.c.next[cB], cD = m.c.next[cC];
    int eAB = m.c.e[cA], eCD = m.c.e[cC];
    if (!ringSet.contains(eAB) || !ringSet.contains(eCD)) {
      continue; // need the opposite ring-edge pair
    }
    bool selF = m.f.select.get_data()->get(f);
    int a = m.c.v[cA], b = m.c.v[cB], cc = m.c.v[cC], d = m.c.v[cD];
    int m1 = emap.lookup(eAB), m2 = emap.lookup(eCD);

    AttrRowSnapshot fsnap;
    snapshotAttrRow(m.f.attrs, f, fsnap);
    AttrRowSnapshot sA, sB, sC, sD;
    snapshotAttrRow(m.c.attrs, cA, sA);
    snapshotAttrRow(m.c.attrs, cB, sB);
    snapshotAttrRow(m.c.attrs, cC, sC);
    snapshotAttrRow(m.c.attrs, cD, sD);

    int q1[4] = {a, m1, m2, d};
    int q2[4] = {m1, b, cc, m2};
    int f1 = m.make_face(std::span<int>(q1, 4), cb);
    int f2 = m.make_face(std::span<int>(q2, 4), cb);
    restoreAttrRow(m.f.attrs, f1, fsnap);
    restoreAttrRow(m.f.attrs, f2, fsnap);
    auto restoreCorner = [&](int nf, int vert, AttrRowSnapshot &s) {
      int nl = m.f.l[nf], nc0 = m.l.c[nl], nc = nc0;
      do {
        if (m.c.v[nc] == vert) {
          restoreAttrRow(m.c.attrs, nc, s);
          return;
        }
        nc = m.c.next[nc];
      } while (nc != nc0);
    };
    restoreCorner(f1, a, sA);
    restoreCorner(f1, d, sD);
    restoreCorner(f2, b, sB);
    restoreCorner(f2, cc, sC);
    if (selF) {
      m.f.select.set(f1, true);
      m.f.select.set(f2, true);
    }
  }

  // Non-loop faces bordering a ring edge: rebuild with the midpoint inserted so
  // the cut doesn't leave a T-junction at an open strip end.
  Set<int> nbrSet;
  for (int e : ring) {
    int c0 = m.e.c[e];
    if (c0 == ELEM_NONE) {
      continue;
    }
    int c = c0;
    do {
      int nf = m.l.f[m.c.l[c]];
      if (!loopSet.contains(nf)) {
        nbrSet.add(nf);
      }
      c = m.c.radial_next[c];
    } while (c != c0);
  }
  Vector<int> nbrFaces;
  for (int f : nbrSet) {
    nbrFaces.append(f);
  }
  for (int f : nbrFaces) {
    if (m.f.freemap[f]) {
      continue;
    }
    AttrRowSnapshot fsnap;
    snapshotAttrRow(m.f.attrs, f, fsnap);
    Vector<int> verts;
    Vector<int> mids;
    Vector<AttrRowSnapshot> csnaps;
    int l = m.f.l[f], c0 = m.l.c[l], c = c0;
    do {
      verts.append(m.c.v[c]);
      int *mp = emap.lookup_ptr(m.c.e[c]);
      mids.append(mp ? *mp : ELEM_NONE);
      AttrRowSnapshot cs;
      snapshotAttrRow(m.c.attrs, c, cs);
      csnaps.append(std::move(cs));
      c = m.c.next[c];
    } while (c != c0);
    int n = int(verts.size());
    Vector<int> nv;
    for (int i = 0; i < n; i++) {
      nv.append(verts[i]);
      if (mids[i] != ELEM_NONE) {
        nv.append(mids[i]);
      }
    }
    bool selF = m.f.select.get_data()->get(f);
    int f2 = m.make_face(std::span<int>(nv.data(), nv.size()), cb);
    restoreAttrRow(m.f.attrs, f2, fsnap);
    int l2 = m.f.l[f2], cc0 = m.l.c[l2], cc = cc0;
    do {
      for (int i = 0; i < n; i++) {
        if (m.c.v[cc] == verts[i]) {
          restoreAttrRow(m.c.attrs, cc, csnaps[i]);
          break;
        }
      }
      cc = m.c.next[cc];
    } while (cc != cc0);
    if (selF) {
      m.f.select.set(f2, true);
    }
  }

  // Tear down originals; the old ring edges are now orphaned.
  for (int f : faceLoop) {
    if (!m.f.freemap[f]) {
      m.kill_face(f, cb);
    }
  }
  for (int f : nbrFaces) {
    if (!m.f.freemap[f]) {
      m.kill_face(f, cb);
    }
  }
  for (int e : ring) {
    if (!m.e.freemap[e] && m.e.c[e] == ELEM_NONE) {
      m.kill_edge(e, cb);
    }
  }

  auto *vselw = m.v.select.get_data();
  for (int mid : outVerts) {
    if (!m.v.freemap[mid]) {
      vselw->set(mid, true);
    }
  }
}

} // namespace sculptcore::mesh::ops
