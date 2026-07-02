#pragma once

#include "../mesh.h"
#include "../mesh_callbacks.h"
#include "../utils/attr_interp.h"
#include "../utils/select_derive.h"
#include "extrude.h" // fireChange
#include "litestl/util/map.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <span>

/* Box-modeling inset macro-operator (Milestone 3 of
 * documentation/plans/boxModelingTools.md). Builds the inset ring at ZERO offset
 * (coincident with the region boundary) and emits, per movable (inset) vert, an
 * in-plane inward tangent — the parametric modal drives `co = base + width·tangent`
 * (and an optional depth along the normal), giving a single topology+positions
 * undo step (the op holds the MeshLog step open across the drag). */

namespace sculptcore::mesh::ops {

/* Inset the selected face region: duplicate the region-boundary verts inward as
 * an inset ring, rebuild the selected faces onto (inset boundary + kept interior)
 * verts, and bridge the boundary with in-plane border quads. Outputs the inset
 * vert indices, their base (boundary) positions, and per-vert inward tangents
 * (all flat). Single-outer-loop faces. */
static inline void insetRegion(Mesh &m,
                               MeshCallbacks *cb,
                               litestl::util::Vector<int> &insetVertsOut,
                               litestl::util::Vector<float> &baseCoOut,
                               litestl::util::Vector<float> &tangentOut,
                               bool preferOpDomain = true)
{
  using litestl::util::Map;
  using litestl::util::Set;
  using litestl::util::Vector;

  Set<int> selSet = resolveFaceSelection(m, preferOpDomain);

  Vector<int> selFaces;
  for (int f : m.f) {
    if (selSet.contains(f)) {
      selFaces.append(f);
    }
  }
  if (selFaces.size() == 0) {
    return;
  }

  Set<int> eset, boundary, bverts;
  for (int f : selFaces) {
    int l = m.f.l[f];
    int c0 = m.l.c[l], c = c0;
    do {
      eset.add(m.c.e[c]);
      c = m.c.next[c];
    } while (c != c0);
  }
  for (int e : eset) {
    int c0 = m.e.c[e];
    if (c0 == ELEM_NONE) {
      continue;
    }
    int sel = 0, c = c0;
    do {
      if (selSet.contains(m.l.f[m.c.l[c]])) {
        sel++;
      }
      c = m.c.radial_next[c];
    } while (c != c0);
    if (sel == 1) {
      boundary.add(e);
      bverts.add(m.e.vs[e][0]);
      bverts.add(m.e.vs[e][1]);
    }
  }

  // Duplicate the boundary verts (the inset ring); interior verts stay. Compute
  // each boundary vert's in-plane inward tangent: for each of its boundary edges,
  // the in-plane perpendicular (edge x selected-face-normal) oriented toward that
  // face's interior, averaged. This is the true "shrink the boundary" direction —
  // unlike an edge bisector it doesn't collapse on a curved boundary.
  Map<int, int> vmap;
  for (int v : bverts) {
    int v2 = m.make_vertex(m.v.co[v], cb);
    AttrRowSnapshot s;
    snapshotAttrRow(m.v.attrs, v, s);
    restoreAttrRow(m.v.attrs, v2, s);
    vmap.insert(v, v2);

    math::float3 vco = m.v.co[v];
    math::float3 acc(0.0f, 0.0f, 0.0f);
    for (int e : m.e_of_v(v)) {
      if (!boundary.contains(e)) {
        continue;
      }
      int other = m.e.vs[e][0] == v ? m.e.vs[e][1] : m.e.vs[e][0];

      int selF = ELEM_NONE;
      int c0 = m.e.c[e], c = c0;
      do {
        int ff = m.l.f[m.c.l[c]];
        if (selSet.contains(ff)) {
          selF = ff;
          break;
        }
        c = m.c.radial_next[c];
      } while (c != c0);
      if (selF == ELEM_NONE) {
        continue;
      }

      math::float3 d = m.v.co[other] - vco;
      float dl = d.length();
      if (dl < 1e-9f) {
        continue;
      }
      d /= dl;
      math::float3 perp = m.f.no[selF].cross(d);
      float pl = perp.length();
      if (pl < 1e-9f) {
        continue;
      }
      perp /= pl;
      // Orient toward the selected face's interior (its centroid).
      math::float3 cen(0.0f, 0.0f, 0.0f);
      int cnt = 0, fl = m.f.l[selF], fc0 = m.l.c[fl], fc = fc0;
      do {
        cen += m.v.co[m.c.v[fc]];
        cnt++;
        fc = m.c.next[fc];
      } while (fc != fc0);
      cen /= float(cnt);
      if ((cen - vco).dot(perp) < 0.0f) {
        perp = perp * -1.0f;
      }
      acc += perp;
    }
    float al = acc.length();
    math::float3 t = al > 1e-9f ? acc / al : math::float3(0.0f, 0.0f, 0.0f);

    insetVertsOut.append(v2);
    baseCoOut.append(vco[0]);
    baseCoOut.append(vco[1]);
    baseCoOut.append(vco[2]);
    tangentOut.append(t[0]);
    tangentOut.append(t[1]);
    tangentOut.append(t[2]);
  }

  // Rebuild each selected face onto (inset boundary | kept interior) verts.
  for (int f : selFaces) {
    AttrRowSnapshot fsnap;
    snapshotAttrRow(m.f.attrs, f, fsnap);

    Vector<int> origVerts, newVerts;
    Vector<AttrRowSnapshot> csnaps;
    int l = m.f.l[f];
    int c0 = m.l.c[l], c = c0;
    do {
      int v = m.c.v[c];
      origVerts.append(v);
      int *iv = vmap.lookup_ptr(v);
      newVerts.append(iv ? *iv : v);
      AttrRowSnapshot cs;
      snapshotAttrRow(m.c.attrs, c, cs);
      csnaps.append(std::move(cs));
      c = m.c.next[c];
    } while (c != c0);

    int f2 = m.make_face(std::span<int>(newVerts.data(), newVerts.size()), cb);
    restoreAttrRow(m.f.attrs, f2, fsnap);
    {
      int l2 = m.f.l[f2], cc0 = m.l.c[l2], cc = cc0, i = 0;
      do {
        restoreAttrRow(m.c.attrs, cc, csnaps[i++]);
        cc = m.c.next[cc];
      } while (cc != cc0);
    }

    int n = int(origVerts.size());
    for (int i = 0; i < n; i++) {
      int va = origVerts[i], vb = origVerts[(i + 1) % n];
      int e = m.find_edge(va, vb);
      if (e != ELEM_NONE && boundary.contains(e)) {
        int *iva = vmap.lookup_ptr(va);
        int *ivb = vmap.lookup_ptr(vb);
        if (iva && ivb) {
          // In-plane border quad va -> vb -> vb' -> va'.
          int quad[4] = {va, vb, *ivb, *iva};
          m.make_face(std::span<int>(quad, 4), cb);
        }
      }
    }
  }

  for (int f : selFaces) {
    m.kill_face(f, cb);
  }

  // Select the inset ring; deselect the source boundary verts. Interior verts and
  // boundary verts stay (used by the cap / border / outside faces) — no orphans.
  auto *vselw = m.v.select.get_data();
  for (int v : bverts) {
    if (!m.v.freemap[v] && vselw->get(v)) {
      fireChange(cb ? cb->onVertChange : litestl::util::function<void(int)>(), v);
      vselw->set(v, false);
    }
  }
  for (int iv : insetVertsOut) {
    vselw->set(iv, true);
  }
}

} // namespace sculptcore::mesh::ops
