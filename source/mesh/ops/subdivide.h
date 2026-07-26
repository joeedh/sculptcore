#pragma once

#include "../mesh.h"
#include "../mesh_callbacks.h"
#include "../mesh_iter.h"
#include "../utils/attr_interp.h"
#include "../utils/select_derive.h"
#include "litestl/math/vector.h"
#include "litestl/util/map.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <span>

/* Box-modeling pattern subdivide (Milestone 4, reworked). Edge-based and
 * parameterized by `numCuts` (Blender-style: each selected edge is split into
 * numCuts+1 segments). Every face touched by a cut edge is filled by a pattern
 * keyed off which of its edges are cut:
 *   - quad, all 4 edges cut -> an (numCuts+1)^2 grid of quads,
 *   - quad, 2 opposite edges cut -> a strip of numCuts+1 quads (parallel loop
 *     cuts),
 *   - anything else (partial quad, tris, n-gons) -> a fan triangulation of the
 *     face boundary with the cut points inserted (always valid; convex faces).
 * Unselected neighbors of a cut edge are filled the same way, so no T-junction is
 * left behind. Falls back to all edges of the selected faces when no edge is
 * selected, so it works in face select mode too. */

namespace sculptcore::mesh::ops {

namespace detail {

/* Make a face from `verts`, carry the source face attrs, and restore each
 * original corner's attrs onto the matching new corner (new cut/inner verts keep
 * defaults). */
static inline void subdivMakeFace(Mesh &m,
                                  MeshCallbacks *cb,
                                  std::span<int> verts,
                                  const AttrRowSnapshot &fsnap,
                                  const litestl::util::Vector<int> &origVerts,
                                  const litestl::util::Vector<AttrRowSnapshot> &origSnaps)
{
  int f2 = m.make_face(verts, cb);
  restoreAttrRow(m.f.attrs, f2, fsnap);
  int l2 = m.f.l[f2], c0 = m.l.c[l2], c = c0;
  do {
    int v = m.c.v[c];
    for (int i = 0; i < int(origVerts.size()); i++) {
      if (origVerts[i] == v) {
        restoreAttrRow(m.c.attrs, c, origSnaps[i]);
        break;
      }
    }
    c = m.c.next[c];
  } while (c != c0);
}

} // namespace detail

/* Fill one face given its per-corner cut lists. `verts` are the corner verts in
 * loop order; `edgeCuts[i]` are the cut verts on the edge leaving verts[i]
 * (oriented verts[i] -> verts[i+1]), empty if that edge isn't cut. */
static inline void subdivFillFace(Mesh &m,
                                  MeshCallbacks *cb,
                                  int f,
                                  const litestl::util::Vector<int> &verts,
                                  const litestl::util::Vector<litestl::util::Vector<int>> &edgeCuts,
                                  int numCuts,
                                  const AttrRowSnapshot &fsnap,
                                  const litestl::util::Vector<int> &origVerts,
                                  const litestl::util::Vector<AttrRowSnapshot> &origSnaps)
{
  using litestl::util::Vector;

  int n = int(verts.size());
  int N = numCuts;
  int M = N + 1;

  int cutMask = 0, nCut = 0;
  for (int i = 0; i < n; i++) {
    if (edgeCuts[i].size() > 0) {
      cutMask |= 1 << i;
      nCut++;
    }
  }

  auto mkface = [&](std::span<int> vs) {
    detail::subdivMakeFace(m, cb, vs, fsnap, origVerts, origSnaps);
  };

  // ---- quad, all four edges cut -> grid ----
  if (n == 4 && cutMask == 0b1111) {
    math::float3 A = m.v.co[verts[0]], B = m.v.co[verts[1]];
    math::float3 C = m.v.co[verts[2]], D = m.v.co[verts[3]];

    // grid[i][j], i,j in 0..M. i ~ A-edge -> D-edge, j ~ A-edge -> B-edge.
    Vector<Vector<int>> grid;
    grid.resize(M + 1);
    for (int i = 0; i <= M; i++) {
      grid[i].resize(M + 1);
    }
    grid[0][0] = verts[0];
    grid[0][M] = verts[1];
    grid[M][M] = verts[2];
    grid[M][0] = verts[3];
    for (int j = 1; j < M; j++) {
      grid[0][j] = edgeCuts[0][j - 1];          // A->B
      grid[M][j] = edgeCuts[2][N - j];          // D->C (reverse of C->D)
    }
    for (int i = 1; i < M; i++) {
      grid[i][M] = edgeCuts[1][i - 1];          // B->C
      grid[i][0] = edgeCuts[3][N - i];          // A->D (reverse of D->A)
    }
    for (int i = 1; i < M; i++) {
      for (int j = 1; j < M; j++) {
        float u = float(i) / float(M), v = float(j) / float(M);
        math::float3 co = A * ((1 - u) * (1 - v)) + B * ((1 - u) * v) + C * (u * v) + D * (u * (1 - v));
        int iv = m.make_vertex(co, cb);
        // best-effort attrs: copy corner A's vertex attrs
        AttrRowSnapshot s;
        snapshotAttrRow(m.v.attrs, verts[0], s);
        restoreAttrRow(m.v.attrs, iv, s);
        grid[i][j] = iv;
      }
    }
    for (int i = 0; i < M; i++) {
      for (int j = 0; j < M; j++) {
        int q[4] = {grid[i][j], grid[i][j + 1], grid[i + 1][j + 1], grid[i + 1][j]};
        mkface(std::span<int>(q, 4));
      }
    }
    return;
  }

  // ---- quad, two opposite edges cut -> strip ----
  if (n == 4 && (cutMask == 0b0101 || cutMask == 0b1010)) {
    // Rotate so the cut pair is edges 0 and 2.
    int s = (cutMask == 0b0101) ? 0 : 1;
    int a = verts[s], b = verts[(s + 1) % 4], c = verts[(s + 2) % 4], d = verts[(s + 3) % 4];
    const Vector<int> &eAB = edgeCuts[s];           // a->b
    const Vector<int> &eCD = edgeCuts[(s + 2) % 4]; // c->d
    // top a..b, bottom d..c (= reverse of c->d).
    Vector<int> top, bottom;
    top.append(a);
    for (int k = 0; k < N; k++) {
      top.append(eAB[k]);
    }
    top.append(b);
    bottom.append(d);
    for (int k = N - 1; k >= 0; k--) {
      bottom.append(eCD[k]);
    }
    bottom.append(c);
    for (int k = 0; k < M; k++) {
      int q[4] = {top[k], top[k + 1], bottom[k + 1], bottom[k]};
      mkface(std::span<int>(q, 4));
    }
    return;
  }

  // ---- single-cut (numCuts==1) partial patterns, ported from the TS mesh
  //      splitEdgesSmart2 pattern table. The face is rotated so its cut edges
  //      land in the canonical positions, then a fixed fill is emitted. ----
  if (N == 1 && (n == 3 || n == 4)) {
    auto ri = [&](int i, int r) { return (i + r) % n; };
    auto findRot = [&](int canon) -> int {
      for (int r = 0; r < n; r++) {
        int mk = 0;
        for (int i = 0; i < n; i++) {
          if (edgeCuts[ri(i, r)].size() > 0) {
            mk |= 1 << i;
          }
        }
        if (mk == canon) {
          return r;
        }
      }
      return -1;
    };
    auto C = [&](int i, int r) { return verts[ri(i, r)]; };
    auto Mid = [&](int i, int r) { return edgeCuts[ri(i, r)][0]; };
    auto face3 = [&](int a, int b, int c) {
      int t[3] = {a, b, c};
      mkface(std::span<int>(t, 3));
    };
    auto face4 = [&](int a, int b, int c, int d) {
      int q[4] = {a, b, c, d};
      mkface(std::span<int>(q, 4));
    };
    int r;
    if (n == 3) {
      if ((r = findRot(0b001)) >= 0) { // tri, one edge cut
        face3(C(0, r), Mid(0, r), C(2, r));
        face3(Mid(0, r), C(1, r), C(2, r));
        return;
      }
      if ((r = findRot(0b011)) >= 0) { // tri, two edges cut
        face3(C(0, r), Mid(0, r), Mid(1, r));
        face3(Mid(0, r), C(1, r), Mid(1, r));
        face3(Mid(1, r), C(2, r), C(0, r));
        return;
      }
      if ((r = findRot(0b111)) >= 0) { // tri, all three -> 4 tris
        int m0 = Mid(0, r), m1 = Mid(1, r), m2 = Mid(2, r);
        face3(m2, C(0, r), m0);
        face3(m0, C(1, r), m1);
        face3(m1, C(2, r), m2);
        face3(m0, m1, m2);
        return;
      }
    } else { // n == 4 (all-4 grid + 2-opposite strip already returned above)
      if ((r = findRot(0b0001)) >= 0) { // quad, one edge cut
        face3(C(0, r), Mid(0, r), C(2, r));
        face3(Mid(0, r), C(1, r), C(2, r));
        face3(C(0, r), C(2, r), C(3, r));
        return;
      }
      if ((r = findRot(0b0011)) >= 0) { // quad, two adjacent edges cut
        int m0 = Mid(0, r), m1 = Mid(1, r);
        math::float3 cc = (m.v.co[m0] + m.v.co[m1]) * 0.5f;
        int vc = m.make_vertex(cc, cb);
        interpAttrs(m.v.attrs, vc, m0, m1, 0.5f, &m);
        face4(C(0, r), m0, vc, C(3, r));
        face4(m0, C(1, r), m1, vc);
        face4(m1, C(2, r), C(3, r), vc);
        return;
      }
      if ((r = findRot(0b0111)) >= 0) { // quad, three edges cut
        int m0 = Mid(0, r), m1 = Mid(1, r), m2 = Mid(2, r);
        face3(m0, C(1, r), m1);
        face4(m0, m1, C(2, r), m2);
        face4(m2, C(3, r), C(0, r), m0);
        return;
      }
    }
  }

  // Boundary loop with the cut points inserted (used by both remaining cases).
  Vector<int> bnd;
  for (int i = 0; i < n; i++) {
    bnd.append(verts[i]);
    for (int cv : edgeCuts[i]) {
      bnd.append(cv);
    }
  }
  int bn = int(bnd.size());

  if (nCut == n) {
    // Fully cut (tri / n-gon) -> fan-triangulate the boundary.
    for (int i = 1; i + 1 < bn; i++) {
      int t[3] = {bnd[0], bnd[i], bnd[i + 1]};
      mkface(std::span<int>(t, 3));
    }
  } else {
    // Partially cut (a boundary neighbor) -> keep one face with the cut points
    // inserted (no T-junction, no stray triangles fanning a barely-touched face).
    mkface(std::span<int>(bnd.data(), bnd.size()));
  }
}

/* Subdivide the selected edges (or, if none, the edges of the selected faces)
 * with `numCuts` cuts each. Appends the created cut verts to outVerts. */
static inline void subdivideEdges(Mesh &m,
                                  MeshCallbacks *cb,
                                  int numCuts,
                                  litestl::util::Vector<int> &outVerts,
                                  bool preferOpDomain = true)
{
  using litestl::util::Map;
  using litestl::util::Set;
  using litestl::util::Vector;

  if (numCuts < 1) {
    numCuts = 1;
  }

  // Explicit edges, or edges derived from a vert-only selection (selectFlush).
  Set<int> eset = resolveEdgeSelection(m, preferOpDomain);
  if (eset.size() == 0) {
    // Fall back to every edge of the selected faces (face select mode),
    // explicit or derived.
    Set<int> fset2 = resolveFaceSelection(m, preferOpDomain);
    for (int f : m.f) {
      if (!fset2.contains(f)) {
        continue;
      }
      int l = m.f.l[f], c0 = m.l.c[l], c = c0;
      do {
        eset.add(m.c.e[c]);
        c = m.c.next[c];
      } while (c != c0);
    }
  }
  if (eset.size() == 0) {
    return;
  }

  // Cut verts per edge, stored v0 -> v1.
  Map<int, Vector<int>> ecuts;
  for (int e : eset) {
    int va = m.e.vs[e][0], vb = m.e.vs[e][1];
    Vector<int> cuts;
    for (int k = 1; k <= numCuts; k++) {
      float t = float(k) / float(numCuts + 1);
      math::float3 co = m.v.co[va] * (1.0f - t) + m.v.co[vb] * t;
      int cv = m.make_vertex(co, cb);
      interpAttrs(m.v.attrs, cv, va, vb, t, &m);
      cuts.append(cv);
      outVerts.append(cv);
    }
    ecuts.insert(e, std::move(cuts));
  }

  // Faces touched by any cut edge.
  Set<int> fset;
  for (int e : eset) {
    int c0 = m.e.c[e];
    if (c0 == ELEM_NONE) {
      continue;
    }
    int c = c0;
    do {
      fset.add(m.l.f[m.c.l[c]]);
      c = m.c.radial_next[c];
    } while (c != c0);
  }

  Vector<int> oldFaces;
  for (int f : fset) {
    oldFaces.append(f);

    AttrRowSnapshot fsnap;
    snapshotAttrRow(m.f.attrs, f, fsnap);

    Vector<int> verts;
    Vector<Vector<int>> edgeCuts;
    Vector<int> origVerts;
    Vector<AttrRowSnapshot> origSnaps;

    int l = m.f.l[f], c0 = m.l.c[l], c = c0;
    do {
      int v = m.c.v[c];
      verts.append(v);
      origVerts.append(v);
      AttrRowSnapshot cs;
      snapshotAttrRow(m.c.attrs, c, cs);
      origSnaps.append(std::move(cs));

      int e = m.c.e[c];
      Vector<int> *cuts = ecuts.lookup_ptr(e);
      Vector<int> oriented;
      if (cuts) {
        if (m.e.vs[e][0] == v) {
          for (int cv : *cuts) {
            oriented.append(cv); // edge stored v0->v1 == loop dir
          }
        } else {
          for (int i = int(cuts->size()) - 1; i >= 0; i--) {
            oriented.append((*cuts)[i]); // reverse
          }
        }
      }
      edgeCuts.append(std::move(oriented));

      c = m.c.next[c];
    } while (c != c0);

    subdivFillFace(m, cb, f, verts, edgeCuts, numCuts, fsnap, origVerts, origSnaps);
  }

  for (int f : oldFaces) {
    m.kill_face(f, cb);
  }
  for (int e : eset) {
    if (!m.e.freemap[e] && m.e.c[e] == ELEM_NONE) {
      m.kill_edge(e, cb);
    }
  }

  // Leave the new cut verts selected.
  auto *vselw = m.v.select.get_data();
  for (int cv : outVerts) {
    if (!m.v.freemap[cv]) {
      vselw->set(cv, true);
    }
  }
}

} // namespace sculptcore::mesh::ops
