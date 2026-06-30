#pragma once

#include "../mesh.h"
#include "../mesh_callbacks.h"
#include "../utils/attr_interp.h"
#include "litestl/util/map.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <span>

/* Box-modeling pattern subdivide (Milestone 4). Each selected face is split into
 * one quad per corner around a new center vert (quad -> 4, tri -> 3, n-gon -> n),
 * sharing an edge-midpoint vert per edge. Edge midpoints on the selection boundary
 * are also inserted into the adjacent UNSELECTED face's loop (it becomes an
 * (n+1)-gon with a colinear vert) so no T-junction is left behind. */

namespace sculptcore::mesh::ops {

/* Subdivide every selected face one level. Appends the created midpoint + center
 * verts to outVerts (for selection / a later smooth). */
static inline void subdivideFaces(Mesh &m,
                                  MeshCallbacks *cb,
                                  litestl::util::Vector<int> &outVerts)
{
  using litestl::util::Map;
  using litestl::util::Set;
  using litestl::util::Vector;

  auto *fsel = m.f.select.get_data();

  Vector<int> selFaces;
  for (int f : m.f) {
    if (fsel->get(f)) {
      selFaces.append(f);
    }
  }
  if (selFaces.size() == 0) {
    return;
  }

  // One midpoint vert per edge of any selected face (deduped). Interp vert attrs.
  Map<int, int> emap;
  Set<int> eset;
  for (int f : selFaces) {
    int l = m.f.l[f], c0 = m.l.c[l], c = c0;
    do {
      eset.add(m.c.e[c]);
      c = m.c.next[c];
    } while (c != c0);
  }
  for (int e : eset) {
    int va = m.e.vs[e][0], vb = m.e.vs[e][1];
    int mid = m.make_vertex((m.v.co[va] + m.v.co[vb]) * 0.5f, cb);
    interpAttrs(m.v.attrs, mid, va, vb, 0.5f);
    emap.insert(e, mid);
    outVerts.append(mid);
  }

  // Every face touching a subdivided edge: selected ones split into quads,
  // unselected neighbors just get the midpoint inserted into their loop.
  Set<int> faceSet;
  for (int e : eset) {
    int c0 = m.e.c[e];
    if (c0 == ELEM_NONE) {
      continue;
    }
    int c = c0;
    do {
      faceSet.add(m.l.f[m.c.l[c]]);
      c = m.c.radial_next[c];
    } while (c != c0);
  }

  Vector<int> oldFaces;
  for (int f : faceSet) {
    oldFaces.append(f);
    bool sel = fsel->get(f);

    AttrRowSnapshot fsnap;
    snapshotAttrRow(m.f.attrs, f, fsnap);

    // Snapshot the loop: verts, per-corner attr rows, and the out-edge midpoint
    // (ELEM_NONE if that edge isn't subdivided).
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

    if (!sel) {
      // Unselected neighbor: rebuild as one face with midpoints inserted.
      Vector<int> nv;
      for (int i = 0; i < n; i++) {
        nv.append(verts[i]);
        if (mids[i] != ELEM_NONE) {
          nv.append(mids[i]);
        }
      }
      int f2 = m.make_face(std::span<int>(nv.data(), nv.size()), cb);
      restoreAttrRow(m.f.attrs, f2, fsnap);
      // Restore the original corners' attrs (the inserted midpoint corners keep
      // their default — a colinear seam vert, no UV island change in practice).
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
      continue;
    }

    // Selected face: center vert + one quad per corner.
    math::float3 cen(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < n; i++) {
      cen += m.v.co[verts[i]];
    }
    cen /= float(n);
    int ctr = m.make_vertex(cen, cb);
    if (n > 0) {
      AttrRowSnapshot vs;
      snapshotAttrRow(m.v.attrs, verts[0], vs);
      restoreAttrRow(m.v.attrs, ctr, vs);
    }
    outVerts.append(ctr);

    for (int i = 0; i < n; i++) {
      int mOut = mids[i];                    // midpoint of edge out of verts[i]
      int mIn = mids[(i - 1 + n) % n];       // midpoint of edge into verts[i]
      if (mOut == ELEM_NONE || mIn == ELEM_NONE) {
        continue; // every selected-face edge is subdivided; defensive
      }
      int quad[4] = {verts[i], mOut, ctr, mIn};
      int f2 = m.make_face(std::span<int>(quad, 4), cb);
      restoreAttrRow(m.f.attrs, f2, fsnap);
      m.f.select.set(f2, true); // keep the subdivided region selected
      // Corner at verts[i] keeps its original attrs.
      int l2 = m.f.l[f2], cc0 = m.l.c[l2], cc = cc0;
      do {
        if (m.c.v[cc] == verts[i]) {
          restoreAttrRow(m.c.attrs, cc, csnaps[i]);
          break;
        }
        cc = m.c.next[cc];
      } while (cc != cc0);
    }
  }

  for (int f : oldFaces) {
    m.kill_face(f, cb);
  }
  // The subdivided edges are now orphaned (rebuilt faces use the half-edges).
  for (int e : eset) {
    if (!m.e.freemap[e] && m.e.c[e] == ELEM_NONE) {
      m.kill_edge(e, cb);
    }
  }
}

} // namespace sculptcore::mesh::ops
