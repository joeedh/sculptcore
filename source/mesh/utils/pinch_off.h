#pragma once

/** Pinch-off: cuts a closed triangle mesh along a 3-cycle a, b, c that is not a face.
 *
 * Dyntopo uses this on a tube thinned below the detail size. The link condition refuses every
 * collapse of such a tube's 3-vertex rings, so without a cut it survives as a thin string.
 *
 * Orient the cycle a→b→c. Side L holds the faces that contain a directed ring edge (a→b, b→c or
 * c→a) and the faces reached from them around the ring vertices without crossing a ring edge.
 * Side R holds the faces that contain a reversed ring edge, reached the same way. The cut gives
 * side R its own copies a', b', c' of the ring vertices and closes each side with one triangle:
 * (a, c, b) on L and (a', b', c') on R. Both sides stay closed and consistently oriented.
 *
 * Topology change: dV = +3, dE = +3, dF = +2, so the Euler characteristic rises by 2. The cut
 * either separates a component in two or removes one handle.
 *
 * Every R face keeps its id and is rewritten in place (clear_face_contents / reinit_face), and each
 * spoke from a ring vertex into side R keeps its id and is re-pointed at the copy
 * (relink_edge_verts), which is how splitEdge rewires. The meshlog therefore records Changes for
 * them, and the spatial tree re-flags their leaves and claims the copies through touch_face. */

#include "../mesh.h"
#include "../mesh_base.h"
#include "../mesh_iter.h"
#include "attr_interp.h"

#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <span>

namespace sculptcore::mesh {

/** Why pinchSeparatingTriangle refused. The mesh is untouched in every case. */
enum class PinchRefusal {
  None,
  /** A ring vertex is freed or repeated, or a ring edge does not exist. */
  Missing,
  /** a, b, c is already a face. */
  IsFace,
  /** A ring edge or spoke does not have exactly two triangle faces, or a ring vertex's fan is not
   * one closed disk. */
  NonManifold,
  /** The two sides overlap or leave a face out, so the local orientation is inconsistent. */
  Inconsistent,
};

struct PinchResult {
  PinchRefusal refusal = PinchRefusal::None;
  /** The copies a', b', c', in the order of the input vertices. */
  int copies[3] = {ELEM_NONE, ELEM_NONE, ELEM_NONE};
  /** The cap on the original vertices, (a, c, b). */
  int cap_l = ELEM_NONE;
  /** The cap on the copies, (a', b', c'). */
  int cap_r = ELEM_NONE;
  /** The three twin ring edges plus every re-pointed spoke. */
  litestl::util::Vector<int, 16> created_edges;
};

namespace detail_pinch {

static inline int findEdge(Mesh &m, int v0, int v1)
{
  if (m.v.e[v0] == ELEM_NONE) {
    return ELEM_NONE;
  }
  for (int e : EdgeOfVertIter(&m, v0, m.v.e[v0])) {
    int o = (m.e.vs[e][0] == v0) ? m.e.vs[e][1] : m.e.vs[e][0];
    if (o == v1) {
      return e;
    }
  }
  return ELEM_NONE;
}

/** Returns the corner of a triangle face on `edge` that runs from `from` to `to`, or ELEM_NONE. */
static inline int directedCorner(Mesh &m, int edge, int from, int to)
{
  int c0 = m.e.c[edge];
  if (c0 == ELEM_NONE) {
    return ELEM_NONE;
  }
  int cc = c0;
  do {
    if (m.c.v[cc] == from && m.c.v[m.c.next[cc]] == to) {
      return cc;
    }
    cc = m.c.radial_next[cc];
  } while (cc != c0);
  return ELEM_NONE;
}

static inline int radialCount(Mesh &m, int edge)
{
  int c0 = m.e.c[edge];
  if (c0 == ELEM_NONE) {
    return 0;
  }
  int n = 0, cc = c0;
  do {
    n++;
    cc = m.c.radial_next[cc];
  } while (cc != c0);
  return n;
}

static inline int cornerOf(Mesh &m, int f, int vert)
{
  int c0 = m.l.c[m.f.l[f]], cc = c0;
  do {
    if (m.c.v[cc] == vert) {
      return cc;
    }
    cc = m.c.next[cc];
  } while (cc != c0);
  return ELEM_NONE;
}

} // namespace detail_pinch

/** Cuts the mesh along the 3-cycle a, b, c and caps both sides. Returns false and leaves the mesh
 * untouched when the cycle is a face, is incomplete, or its neighbourhood is not a closed,
 * consistently oriented triangle manifold; `out->refusal` says which. */
static inline bool pinchSeparatingTriangle(
    Mesh &m, int a, int b, int c, PinchResult *out = nullptr, MeshCallbacks *cb = nullptr)
{
  using namespace litestl::util;
  using namespace detail_pinch;

  auto refuse = [&](PinchRefusal why) {
    if (out) {
      out->refusal = why;
    }
    return false;
  };

  const int ring[3] = {a, b, c};
  for (int i = 0; i < 3; i++) {
    int v = ring[i];
    if (v < 0 || v >= int(m.v.capacity()) || m.v.freemap[v] || v == ring[(i + 1) % 3]) {
      return refuse(PinchRefusal::Missing);
    }
  }
  auto ringIndex = [&](int v) { return v == a ? 0 : v == b ? 1 : v == c ? 2 : -1; };

  // ringEdge[i] joins ring[i] to ring[i + 1].
  int ringEdge[3];
  for (int i = 0; i < 3; i++) {
    ringEdge[i] = findEdge(m, ring[i], ring[(i + 1) % 3]);
    if (ringEdge[i] == ELEM_NONE) {
      return refuse(PinchRefusal::Missing);
    }
  }

  // Every face touching a ring vertex, with the manifold checks on the way.
  Vector<int, 32> fan;
  Set<int, 32> fanSet;
  for (int v : ring) {
    for (int e : EdgeOfVertIter(&m, v, m.v.e[v])) {
      if (radialCount(m, e) != 2) {
        return refuse(PinchRefusal::NonManifold);
      }
      int c0 = m.e.c[e], cc = c0;
      do {
        int li = m.c.l[cc];
        int f = m.l.f[li];
        if (m.l.size[li] != 3 || m.f.list_count[f] != 1) {
          return refuse(PinchRefusal::NonManifold);
        }
        if (fanSet.add(f)) {
          fan.append(f);
        }
        cc = m.c.radial_next[cc];
      } while (cc != c0);
    }
  }
  for (int f : fan) {
    int hits = 0;
    int c0 = m.l.c[m.f.l[f]], cc = c0;
    do {
      hits += ringIndex(m.c.v[cc]) >= 0;
      cc = m.c.next[cc];
    } while (cc != c0);
    if (hits == 3) {
      return refuse(PinchRefusal::IsFace);
    }
  }

  // Seed each side with the faces holding the directed (L) or reversed (R) ring edges.
  int seedL[3], seedR[3];
  for (int i = 0; i < 3; i++) {
    int from = ring[i], to = ring[(i + 1) % 3];
    int cl = directedCorner(m, ringEdge[i], from, to);
    int cr = directedCorner(m, ringEdge[i], to, from);
    if (cl == ELEM_NONE || cr == ELEM_NONE) {
      return refuse(PinchRefusal::Inconsistent);
    }
    seedL[i] = m.l.f[m.c.l[cl]];
    seedR[i] = m.l.f[m.c.l[cr]];
  }

  // Flood a side through the spokes (edges with exactly one ring endpoint), never across a
  // ring edge, staying on faces that touch the ring.
  auto flood = [&](const int seeds[3], Set<int, 32> &side) {
    Vector<int, 32> stack;
    for (int i = 0; i < 3; i++) {
      if (side.add(seeds[i])) {
        stack.append(seeds[i]);
      }
    }
    while (!stack.isEmpty()) {
      int f = stack.pop_back();
      int c0 = m.l.c[m.f.l[f]], cc = c0;
      do {
        int e = m.c.e[cc];
        int ends = (ringIndex(m.e.vs[e][0]) >= 0) + (ringIndex(m.e.vs[e][1]) >= 0);
        if (ends == 1) {
          int other = m.l.f[m.c.l[m.c.radial_next[cc]]];
          if (fanSet.contains(other) && side.add(other)) {
            stack.append(other);
          }
        }
        cc = m.c.next[cc];
      } while (cc != c0);
    }
  };
  Set<int, 32> sideL, sideR;
  flood(seedL, sideL);
  flood(seedR, sideR);
  if (int(sideL.size()) + int(sideR.size()) != int(fan.size())) {
    return refuse(PinchRefusal::Inconsistent); // overlap, or a fan split in two
  }
  for (int f : fan) {
    if (sideL.contains(f) == sideR.contains(f)) {
      return refuse(PinchRefusal::Inconsistent);
    }
  }

  // Snapshot each R face's corner rows and its original edges, keyed by corner position.
  struct FaceSnap {
    int f;
    int verts[3];
    int edges[3];
    AttrRowSnapshot corners[3];
  };
  Vector<FaceSnap, 16> rfaces;
  Vector<int, 16> spokes;
  Set<int, 16> spokeSet;
  for (int f : fan) {
    if (!sideR.contains(f)) {
      continue;
    }
    FaceSnap fs;
    fs.f = f;
    int c0 = m.l.c[m.f.l[f]], cc = c0;
    for (int i = 0; i < 3; i++) {
      fs.verts[i] = m.c.v[cc];
      fs.edges[i] = m.c.e[cc];
      snapshotAttrRow(m.c.attrs, cc, fs.corners[i]);
      int e = fs.edges[i];
      int ends = (ringIndex(m.e.vs[e][0]) >= 0) + (ringIndex(m.e.vs[e][1]) >= 0);
      if (ends == 1 && spokeSet.add(e)) {
        spokes.append(e);
      }
      cc = m.c.next[cc];
    }
    rfaces.append(std::move(fs));
  }

  // Validation is complete; the mesh is mutated from here on.
  int copy[3];
  for (int i = 0; i < 3; i++) {
    copy[i] = m.make_vertex(m.v.co[ring[i]], cb, ring[i]);
    interpAttrs(m.v.attrs, copy[i], ring[i], ring[i], 0.0f, &m);
  }
  auto mapped = [&](int v) {
    int i = ringIndex(v);
    return i >= 0 ? copy[i] : v;
  };

  for (FaceSnap &fs : rfaces) {
    m.clear_face_contents(fs.f, cb);
  }

  // Every spoke into R is now wire; re-point its ring end at the copy.
  for (int e : spokes) {
    int v0 = m.e.vs[e][0], v1 = m.e.vs[e][1];
    m.relink_edge_verts(e, mapped(v0), mapped(v1), cb);
  }

  int twin[3];
  for (int i = 0; i < 3; i++) {
    twin[i] = m.make_edge(copy[i], copy[(i + 1) % 3], cb, ringEdge[i]);
    AttrRowSnapshot es;
    snapshotAttrRow(m.e.attrs, ringEdge[i], es);
    restoreAttrRow(m.e.attrs, twin[i], es);
  }
  auto twinOf = [&](int e) {
    for (int i = 0; i < 3; i++) {
      if (ringEdge[i] == e) {
        return twin[i];
      }
    }
    return e;
  };

  for (FaceSnap &fs : rfaces) {
    int nv[3], ne[3];
    for (int i = 0; i < 3; i++) {
      nv[i] = mapped(fs.verts[i]);
      ne[i] = twinOf(fs.edges[i]);
    }
    m.reinit_face(fs.f, std::span<int>(nv, 3), std::span<int>(ne, 3), cb);
    int c0 = m.l.c[m.f.l[fs.f]], cc = c0;
    for (int i = 0; i < 3; i++) {
      restoreAttrRow(m.c.attrs, cc, fs.corners[i]);
      cc = m.c.next[cc];
    }
  }

  // Caps come last, so each one anchors to verts the spatial tree already owns.
  auto makeCap = [&](const int verts[3], const int edges[3], const int srcFace[3], int f_attr) {
    int v3[3] = {verts[0], verts[1], verts[2]};
    int e3[3] = {edges[0], edges[1], edges[2]};
    int f = m.make_face(std::span<int>(v3, 3), std::span<int>(e3, 3), cb, f_attr);
    AttrRowSnapshot fsnap;
    snapshotAttrRow(m.f.attrs, f_attr, fsnap);
    restoreAttrRow(m.f.attrs, f, fsnap);
    for (int i = 0; i < 3; i++) {
      int dst = cornerOf(m, f, verts[i]);
      int src = cornerOf(m, srcFace[i], verts[i]);
      if (dst != ELEM_NONE && src != ELEM_NONE) {
        AttrRowSnapshot csnap;
        snapshotAttrRow(m.c.attrs, src, csnap);
        restoreAttrRow(m.c.attrs, dst, csnap);
      }
    }
    return f;
  };
  // L cap (a, c, b): edges a→c, c→b, b→a. Corner rows come from the L face at each vertex.
  const int lv[3] = {a, c, b};
  const int le[3] = {ringEdge[2], ringEdge[1], ringEdge[0]};
  const int lsrc[3] = {seedL[0], seedL[1], seedL[0]};
  int capL = makeCap(lv, le, lsrc, seedL[0]);
  // R cap (a', b', c'): edges a'→b', b'→c', c'→a'.
  const int rv[3] = {copy[0], copy[1], copy[2]};
  const int re[3] = {twin[0], twin[1], twin[2]};
  const int rsrc[3] = {seedR[0], seedR[0], seedR[1]};
  int capR = makeCap(rv, re, rsrc, seedR[0]);

  if (out) {
    out->refusal = PinchRefusal::None;
    for (int i = 0; i < 3; i++) {
      out->copies[i] = copy[i];
      out->created_edges.append(twin[i]);
    }
    out->cap_l = capL;
    out->cap_r = capR;
    for (int e : spokes) {
      out->created_edges.append(e);
    }
  }
  return true;
}

} // namespace sculptcore::mesh
