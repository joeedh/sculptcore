#pragma once

#include "../mesh.h"
#include "../mesh_callbacks.h"
#include "../mesh_iter.h"
#include "../utils/attr_interp.h"
#include "../utils/select_derive.h"
#include "litestl/util/map.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <span>

/* Box-modeling vertex bevel (Milestone 3, the edge-split family's bevel/chamfer).
 * Each selected interior-manifold vertex is replaced by a bevel face: one offset
 * vert per incident edge (sliding along that edge), capped by an n-gon. Like the
 * inset it emits per offset vert a base position + tangent so the SAME parametric
 * modal drives `co = base + width·tangent` for a single topology+positions undo
 * step. Boundary / non-manifold verts are skipped. Adjacent selected verts are
 * beveled independently (one may invalidate the other — typical first cut). */

namespace sculptcore::mesh::ops {

/* Bevel one interior-manifold vertex `v`. Appends the new offset verts +
 * base/tangent to the out arrays. No-op if v is on a boundary / non-manifold. */
static inline void bevelOneVert(Mesh &m,
                                MeshCallbacks *cb,
                                int v,
                                litestl::util::Vector<int> &outVerts,
                                litestl::util::Vector<float> &outBase,
                                litestl::util::Vector<float> &outTangent)
{
  using litestl::util::Map;
  using litestl::util::Set;
  using litestl::util::Vector;

  if (m.v.freemap[v] || m.v.e[v] == ELEM_NONE) {
    return;
  }

  // v's edges in disk order + the other endpoint of each. Require every edge to
  // be 2-face manifold (interior vert) — else skip.
  Vector<int> edges, others;
  for (int e : m.e_of_v(v)) {
    int n = 0, c0 = m.e.c[e], c = c0;
    if (c0 == ELEM_NONE) {
      return;
    }
    do {
      n++;
      c = m.c.radial_next[c];
    } while (c != c0);
    if (n != 2) {
      return; // boundary / non-manifold
    }
    edges.append(e);
    others.append(m.e.vs[e][0] == v ? m.e.vs[e][1] : m.e.vs[e][0]);
  }
  int N = int(edges.size());
  if (N < 3) {
    return;
  }

  math::float3 vco = m.v.co[v];

  // Offset vert per edge (at v, offset 0); map edge -> offset vert.
  Map<int, int> e2p;
  Vector<int> pis;
  for (int i = 0; i < N; i++) {
    AttrRowSnapshot s;
    snapshotAttrRow(m.v.attrs, v, s);
    int p = m.make_vertex(vco, cb);
    restoreAttrRow(m.v.attrs, p, s);
    e2p.insert(edges[i], p);
    pis.append(p);

    math::float3 d = m.v.co[others[i]] - vco;
    float dl = d.length();
    math::float3 t = dl > 1e-9f ? d / dl : math::float3(0.0f, 0.0f, 0.0f);
    outVerts.append(p);
    outBase.append(vco[0]);
    outBase.append(vco[1]);
    outBase.append(vco[2]);
    outTangent.append(t[0]);
    outTangent.append(t[1]);
    outTangent.append(t[2]);
  }

  // Faces around v (one corner each). Rebuild each replacing its v-corner with
  // (offset-on-incoming, offset-on-outgoing) — the cut edge of the bevel.
  Set<int> faceSet;
  for (int e : edges) {
    int c0 = m.e.c[e], c = c0;
    do {
      faceSet.add(m.l.f[m.c.l[c]]);
      c = m.c.radial_next[c];
    } while (c != c0);
  }

  Vector<int> oldFaces;
  for (int f : faceSet) {
    oldFaces.append(f);

    AttrRowSnapshot fsnap;
    snapshotAttrRow(m.f.attrs, f, fsnap);

    // Walk the loop; at the v-corner emit p_in then p_out.
    Vector<int> newVerts;
    Vector<AttrRowSnapshot> csnaps;
    int l = m.f.l[f];
    int c0 = m.l.c[l], c = c0;
    do {
      if (m.c.v[c] == v) {
        int eIn = m.c.e[m.c.prev[c]];
        int eOut = m.c.e[c];
        int pIn = e2p.lookup(eIn);
        int pOut = e2p.lookup(eOut);
        AttrRowSnapshot cs;
        snapshotAttrRow(m.c.attrs, c, cs);
        newVerts.append(pIn);
        csnaps.append(cs);
        newVerts.append(pOut);
        csnaps.append(std::move(cs));
      } else {
        newVerts.append(m.c.v[c]);
        AttrRowSnapshot cs;
        snapshotAttrRow(m.c.attrs, c, cs);
        csnaps.append(std::move(cs));
      }
      c = m.c.next[c];
    } while (c != c0);

    int f2 = m.make_face(std::span<int>(newVerts.data(), newVerts.size()), cb);
    restoreAttrRow(m.f.attrs, f2, fsnap);
    {
      int l2 = m.f.l[f2], cc0 = m.l.c[l2], cc = cc0, i = 0;
      do {
        if (i < int(csnaps.size())) {
          restoreAttrRow(m.c.attrs, cc, csnaps[i]);
        }
        i++;
        cc = m.c.next[cc];
      } while (cc != cc0);
    }
  }

  // The bevel cap n-gon: offset verts sorted CCW around v's normal (so the cap
  // shares the cut edges with opposite winding to the rebuilt side faces).
  math::float3 nrm = m.v.no[v];
  float nl = nrm.length();
  if (nl > 1e-9f) {
    nrm /= nl;
  } else {
    nrm = math::float3(0.0f, 0.0f, 1.0f);
  }
  math::float3 xax = std::fabs(nrm[0]) < 0.9f ? math::float3(1.0f, 0.0f, 0.0f)
                                              : math::float3(0.0f, 1.0f, 0.0f);
  xax = xax - nrm * xax.dot(nrm);
  float xl = xax.length();
  xax = xl > 1e-9f ? xax / xl : math::float3(1.0f, 0.0f, 0.0f);
  math::float3 yax = nrm.cross(xax);

  Vector<float> ang;
  for (int i = 0; i < N; i++) {
    math::float3 d = m.v.co[others[i]] - vco;
    ang.append(std::atan2(d.dot(yax), d.dot(xax)));
  }
  Vector<int> order;
  for (int i = 0; i < N; i++) {
    order.append(i);
  }
  for (int i = 1; i < N; i++) {
    int key = order[i];
    float ka = ang[key];
    int j = i - 1;
    while (j >= 0 && ang[order[j]] > ka) {
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = key;
  }
  Vector<int> ngon;
  // Reverse the CCW order so the cap winds opposite the side faces (manifold).
  for (int i = N - 1; i >= 0; i--) {
    ngon.append(pis[order[i]]);
  }
  m.make_face(std::span<int>(ngon.data(), ngon.size()), cb);

  // Tear down: old faces, then v (kills its now-orphaned original edges).
  for (int f : oldFaces) {
    m.kill_face(f, cb);
  }
  m.kill_vertex(v, cb);

  auto *vselw = m.v.select.get_data();
  for (int p : pis) {
    vselw->set(p, true);
  }
}

/* Bevel every selected vertex (explicit, or derived from edges/faces when the
 * vert domain is empty — selectFlush). */
static inline void bevelVerts(Mesh &m,
                              MeshCallbacks *cb,
                              litestl::util::Vector<int> &outVerts,
                              litestl::util::Vector<float> &outBase,
                              litestl::util::Vector<float> &outTangent,
                              bool preferOpDomain = true)
{
  using litestl::util::Vector;
  litestl::util::Set<int> selSet = resolveVertSelection(m, preferOpDomain);
  Vector<int> sel;
  for (int v : m.v) {
    if (selSet.contains(v)) {
      sel.append(v);
    }
  }
  for (int v : sel) {
    bevelOneVert(m, cb, v, outVerts, outBase, outTangent);
  }
}

} // namespace sculptcore::mesh::ops
