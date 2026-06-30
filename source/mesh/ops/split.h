#pragma once

#include "../mesh.h"
#include "../mesh_callbacks.h"
#include "../utils/attr_interp.h"
#include "extrude.h" // ExtrudeResult, fireChange
#include "litestl/util/map.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <span>

/* Box-modeling edge-split family (Milestone 3). Split the selected faces off into
 * a disconnected region: exactly the region extrude minus the bridge. */

namespace sculptcore::mesh::ops {

static inline void splitFacesOff(Mesh &m, MeshCallbacks *cb, ExtrudeResult &out)
{
  using litestl::util::Map;
  using litestl::util::Set;
  using litestl::util::Vector;

  auto *fsel = m.f.select.get_data();

  Vector<int> selFaces;
  math::float3 no(0.0f, 0.0f, 0.0f);
  for (int f : m.f) {
    if (fsel->get(f)) {
      selFaces.append(f);
      no += m.f.no[f];
    }
  }
  if (selFaces.size() == 0) {
    out.ok = false;
    return;
  }

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
      if (fsel->get(m.l.f[m.c.l[c]])) {
        sel++;
      }
      c = m.c.radial_next[c];
    } while (c != c0);
    if (sel == 1) {
      boundary.add(e);
    }
  }

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

  Vector<int> capFaces;
  for (int f : selFaces) {
    AttrRowSnapshot fsnap;
    snapshotAttrRow(m.f.attrs, f, fsnap);

    Vector<int> capVerts;
    Vector<AttrRowSnapshot> csnaps;
    int l = m.f.l[f];
    int c0 = m.l.c[l], c = c0;
    do {
      capVerts.append(vmap.lookup(m.c.v[c]));
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
  }

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
  out.normal = len > 1e-8f ? no / len : math::float3(0.0f, 0.0f, 1.0f);
  out.ok = true;
}

} // namespace sculptcore::mesh::ops
