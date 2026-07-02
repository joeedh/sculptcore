#pragma once

#include "../mesh.h"
#include "../mesh_callbacks.h"
#include "../utils/attr_interp.h"
#include "../utils/select_derive.h"
#include "litestl/util/map.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <span>

/* Box-modeling extrude macro-operators (Milestone 2 of
 * documentation/plans/boxModelingTools.md). Pure Euler-op compositions over the
 * sculptcore mesh; the caller passes MeshCallbacks (from the MeshLog) so spatial
 * + meshlog stay in sync, and each op leaves the new movable region selected
 * (and active) so a chained transform grabs exactly it. Geometry is built at
 * zero offset (duplicates coincide with the source) — the transform supplies the
 * offset, giving a single topology+positions undo step. */

namespace sculptcore::mesh::ops {

struct ExtrudeResult {
  bool ok = false;
  math::float3 normal = math::float3(0.0f, 0.0f, 1.0f);
};

/* Fire a per-element change callback if present (the file-local `fire` helper in
 * mesh.cc isn't visible here). */
static inline void fireChange(const litestl::util::function<void(int)> &cb, int idx)
{
  if (cb) {
    cb(idx);
  }
}

/* Extrude the selected face region: lift the selected faces onto duplicated
 * verts and bridge the region boundary with side quads (the merged/connected
 * extrude). Single-outer-loop faces (holed faces are a follow-up). Leaves the
 * cap faces + their (duplicate) verts selected; `out.normal` = the averaged,
 * normalized face normal for the transform's default constraint axis. */
static inline void extrudeRegion(Mesh &m,
                                 MeshCallbacks *cb,
                                 ExtrudeResult &out,
                                 bool preferOpDomain = true)
{
  using litestl::util::Map;
  using litestl::util::Set;
  using litestl::util::Vector;

  // Explicit face selection, derived from verts/edges when empty (selectFlush).
  // Iterate in mesh order so element creation order is backend-deterministic.
  Set<int> selSet = resolveFaceSelection(m, preferOpDomain);

  Vector<int> selFaces;
  math::float3 no(0.0f, 0.0f, 0.0f);
  for (int f : m.f) {
    if (selSet.contains(f)) {
      selFaces.append(f);
      no += m.f.no[f];
    }
  }
  if (selFaces.size() == 0) {
    out.ok = false;
    return;
  }

  // Verts + edges of the region, and the boundary edges (exactly one selected
  // radial face). Walk only the outer loop for now.
  Set<int> vset, eset, boundary;
  for (int f : selFaces) {
    int l = m.f.l[f];
    int c0 = m.l.c[l], c = c0;
    do {
      vset.add(m.c.v[c]);
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
    }
  }

  // Duplicate every region vert (zero offset); carry its attrs.
  Map<int, int> vmap;
  Vector<int> dupVerts;
  for (int v : vset) {
    AttrRowSnapshot s;
    snapshotAttrRow(m.v.attrs, v, s);
    int v2 = m.make_vertex(m.v.co[v], cb);
    restoreAttrRow(m.v.attrs, v2, s);
    vmap.insert(v, v2);
    dupVerts.append(v2);
  }

  // Cap faces (selected faces rebuilt on the duplicates) + boundary side quads.
  Vector<int> capFaces;
  for (int f : selFaces) {
    AttrRowSnapshot fsnap;
    snapshotAttrRow(m.f.attrs, f, fsnap);

    Vector<int> origVerts, capVerts;
    Vector<AttrRowSnapshot> csnaps;
    int l = m.f.l[f];
    int c0 = m.l.c[l], c = c0;
    do {
      int v = m.c.v[c];
      origVerts.append(v);
      capVerts.append(vmap.lookup(v));
      AttrRowSnapshot cs;
      snapshotAttrRow(m.c.attrs, c, cs);
      csnaps.append(std::move(cs));
      c = m.c.next[c];
    } while (c != c0);

    int f2 = m.make_face(std::span<int>(capVerts.data(), capVerts.size()), cb);
    restoreAttrRow(m.f.attrs, f2, fsnap);
    {
      int l2 = m.f.l[f2], cc0 = m.l.c[l2], cc = cc0, i = 0;
      do {
        restoreAttrRow(m.c.attrs, cc, csnaps[i++]);
        cc = m.c.next[cc];
      } while (cc != cc0);
    }
    capFaces.append(f2);

    int n = int(origVerts.size());
    for (int i = 0; i < n; i++) {
      int va = origVerts[i], vb = origVerts[(i + 1) % n];
      int e = m.find_edge(va, vb);
      if (e != ELEM_NONE && boundary.contains(e)) {
        // Side quad: bottom edge va->vb, top edge vb'->va' (outward winding,
        // matching the mesh-addon's [l1.v, l1.next.v, l2.next.v, l2.v]).
        int quad[4] = {va, vb, vmap.lookup(vb), vmap.lookup(va)};
        m.make_face(std::span<int>(quad, 4), cb);
      }
    }
  }

  // Tear down the originals: faces, then interior (non-boundary) edges that lost
  // all faces, then verts orphaned by that (boundary verts keep their unselected
  // faces and survive).
  for (int f : selFaces) {
    m.kill_face(f, cb);
  }
  for (int e : eset) {
    if (!boundary.contains(e) && !m.e.freemap[e] && m.e.c[e] == ELEM_NONE) {
      m.kill_edge(e, cb);
    }
  }
  for (int v : vset) {
    if (!m.v.freemap[v] && m.v.e[v] == ELEM_NONE) {
      m.kill_vertex(v, cb);
    }
  }

  // Move the selection to the new cap: deselect any surviving source verts,
  // select the duplicates + cap faces (created -> captured at finalizeStep).
  auto *vselw = m.v.select.get_data();
  for (int v : vset) {
    if (!m.v.freemap[v] && vselw->get(v)) {
      fireChange(cb ? cb->onVertChange : litestl::util::function<void(int)>(), v);
      vselw->set(v, false);
    }
  }
  for (int v : dupVerts) {
    vselw->set(v, true);
  }
  for (int f2 : capFaces) {
    m.f.select.set(f2, true);
  }

  float len = no.length();
  if (len > 1e-8f) {
    no /= len;
  } else {
    no = math::float3(0.0f, 0.0f, 1.0f);
  }
  out.normal = no;
  out.ok = true;
}

/* Extrude each selected face individually (no boundary merge): every selected
 * face gets its OWN duplicated verts + a full ring of side quads, so adjacent
 * selected faces split apart. Simpler than the region extrude — every original
 * vert/edge stays bridged, so nothing is orphaned. */
static inline void extrudeIndividual(Mesh &m,
                                     MeshCallbacks *cb,
                                     ExtrudeResult &out,
                                     bool preferOpDomain = true)
{
  using litestl::util::Set;
  using litestl::util::Vector;

  Set<int> selSet = resolveFaceSelection(m, preferOpDomain);

  Vector<int> selFaces;
  math::float3 no(0.0f, 0.0f, 0.0f);
  for (int f : m.f) {
    if (selSet.contains(f)) {
      selFaces.append(f);
      no += m.f.no[f];
    }
  }
  if (selFaces.size() == 0) {
    out.ok = false;
    return;
  }

  Set<int> origVertSet;
  Vector<int> dupVerts, capFaces;
  for (int f : selFaces) {
    AttrRowSnapshot fsnap;
    snapshotAttrRow(m.f.attrs, f, fsnap);

    Vector<int> origVerts, capVerts;
    Vector<AttrRowSnapshot> csnaps;
    int l = m.f.l[f];
    int c0 = m.l.c[l], c = c0;
    do {
      int v = m.c.v[c];
      origVerts.append(v);
      origVertSet.add(v);
      AttrRowSnapshot vs;
      snapshotAttrRow(m.v.attrs, v, vs);
      int v2 = m.make_vertex(m.v.co[v], cb);
      restoreAttrRow(m.v.attrs, v2, vs);
      capVerts.append(v2);
      dupVerts.append(v2);
      AttrRowSnapshot cs;
      snapshotAttrRow(m.c.attrs, c, cs);
      csnaps.append(std::move(cs));
      c = m.c.next[c];
    } while (c != c0);

    int f2 = m.make_face(std::span<int>(capVerts.data(), capVerts.size()), cb);
    restoreAttrRow(m.f.attrs, f2, fsnap);
    {
      int l2 = m.f.l[f2], cc0 = m.l.c[l2], cc = cc0, i = 0;
      do {
        restoreAttrRow(m.c.attrs, cc, csnaps[i++]);
        cc = m.c.next[cc];
      } while (cc != cc0);
    }
    capFaces.append(f2);

    int n = int(origVerts.size());
    for (int i = 0; i < n; i++) {
      int quad[4] = {origVerts[i], origVerts[(i + 1) % n], capVerts[(i + 1) % n], capVerts[i]};
      m.make_face(std::span<int>(quad, 4), cb);
    }
  }

  for (int f : selFaces) {
    m.kill_face(f, cb);
  }

  // Select the new caps; deselect any surviving source verts.
  auto *vselw = m.v.select.get_data();
  for (int v : origVertSet) {
    if (!m.v.freemap[v] && vselw->get(v)) {
      fireChange(cb ? cb->onVertChange : litestl::util::function<void(int)>(), v);
      vselw->set(v, false);
    }
  }
  for (int v : dupVerts) {
    vselw->set(v, true);
  }
  for (int f2 : capFaces) {
    m.f.select.set(f2, true);
  }

  float len = no.length();
  out.normal = len > 1e-8f ? no / len : math::float3(0.0f, 0.0f, 1.0f);
  out.ok = true;
}

/* Extrude selected verts as wires: duplicate each, connect with an edge; the
 * duplicate is the movable selection. The simplest "T" tool. */
static inline void extrudeWireVerts(Mesh &m,
                                    MeshCallbacks *cb,
                                    ExtrudeResult &out,
                                    bool preferOpDomain = true)
{
  using litestl::util::Set;
  using litestl::util::Vector;
  auto *vsel = m.v.select.get_data();

  Set<int> selSet = resolveVertSelection(m, preferOpDomain);

  Vector<int> sel;
  for (int v : m.v) {
    if (selSet.contains(v)) {
      sel.append(v);
    }
  }
  if (sel.size() == 0) {
    out.ok = false;
    return;
  }

  for (int v : sel) {
    AttrRowSnapshot s;
    snapshotAttrRow(m.v.attrs, v, s);
    int v2 = m.make_vertex(m.v.co[v], cb);
    restoreAttrRow(m.v.attrs, v2, s);
    m.make_edge(v, v2, cb);

    fireChange(cb ? cb->onVertChange : litestl::util::function<void(int)>(), v);
    vsel->set(v, false);
    vsel->set(v2, true);
  }

  out.normal = math::float3(0.0f, 0.0f, 1.0f);
  out.ok = true;
}

} // namespace sculptcore::mesh::ops
