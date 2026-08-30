#pragma once

/* Destructive symmetrize: make a mesh perfectly symmetric across an axis plane
 * (x/y/z = 0) by topology surgery, the way Blender's symmetrize works.
 *
 *   1. Triangulate (splitEdge needs triangles; quad generators get fanned).
 *   2. Bisect every edge that strictly crosses the plane, snapping the inserted
 *      vertex exactly onto it — so afterwards no face straddles the plane.
 *   3. Delete the discarded half (faces, then orphaned verts).
 *   4. Mirror the kept half across the plane and weld the seam by SHARING the
 *      on-plane verts (the mirrored faces reuse them, never duplicate them), so
 *      the result is watertight with no fuzzy merge-by-distance pass.
 *
 * Mirroring reflects positions and reverses face winding (a reflection flips
 * orientation), and carries vertex / per-corner / edge attributes (UVs, seams,
 * sharp) onto the mirrored copy. Normals + boundary state are recomputed.
 */

#include "../mesh.h"
#include "../mesh_base.h"
#include "attr_interp.h"
#include "edge_split.h"
#include "triangulate.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <span>
#include <utility>

namespace sculptcore::mesh {

/* keepSign: +1 keeps the positive half (mirrors it onto the negative side), -1
 * keeps the negative half. threshold snaps near-plane verts exactly onto the
 * plane (they become the shared seam). Returns false (mesh untouched) only for a
 * bad axis. */
static inline bool symmetrizeMesh(Mesh &m, int axis, int keepSign, float threshold)
{
  using namespace litestl::util;
  using litestl::math::float3;

  if (axis < 0 || axis > 2) {
    return false;
  }
  keepSign = keepSign >= 0 ? 1 : -1;
  threshold = threshold < 0.0f ? 0.0f : threshold;

  m.thawTopo();
  triangulateMesh(m);

  auto side = [&](int vi) -> int {
    float s = m.v.co[vi][axis];
    if (std::abs(s) <= threshold) {
      return 0;
    }
    return s > 0.0f ? 1 : -1;
  };

  // --- 1. Bisect every edge that strictly crosses the plane. ---
  {
    Vector<int> edges;
    for (int ei : m.e) {
      edges.append(ei);
    }
    for (int ei : edges) {
      if (m.e.freemap[ei]) {
        continue; // already consumed/rewritten by a prior split
      }
      int a = m.e.vs[ei][0], b = m.e.vs[ei][1];
      if (side(a) * side(b) >= 0) {
        continue; // same side, or one endpoint already lies on the plane
      }
      float ca = m.v.co[a][axis], cb = m.v.co[b][axis];
      float t = ca / (ca - cb); // crossing parameter in (0,1)
      EdgeSplitResult res;
      if (!splitEdge(m, ei, &res)) {
        continue; // non-triangle incident face: leave the edge uncut
      }
      float3 p = m.v.co[a] + (m.v.co[b] - m.v.co[a]) * t;
      p[axis] = 0.0f;
      m.v.co[res.new_vert] = p;
    }
  }

  // --- 2. Delete the discarded half. ---
  {
    Vector<int> killF;
    for (int fi : m.f) {
      int li = m.f.l[fi], c0 = m.l.c[li], cc = c0;
      do {
        if (side(m.c.v[cc]) == -keepSign) {
          killF.append(fi);
          break;
        }
        cc = m.c.next[cc];
      } while (cc != c0);
    }
    for (int fi : killF) {
      m.kill_face(fi);
    }

    Vector<int> killV;
    for (int vi : m.v) {
      if (side(vi) == -keepSign) {
        killV.append(vi);
      }
    }
    for (int vi : killV) {
      m.kill_vertex(vi);
    }

    // Drop verts left isolated by the cut (e.g. an on-plane vert ringed only by
    // discarded faces) so they don't survive as stray points.
    Vector<int> killIso;
    for (int vi : m.v) {
      if (m.v.e[vi] == ELEM_NONE) {
        killIso.append(vi);
      }
    }
    for (int vi : killIso) {
      m.kill_vertex(vi);
    }
  }

  // --- 3. Mirror the kept half across the plane. ---
  // mirror[v] = the vert v reflects to: itself for on-plane verts (the weld), a
  // fresh reflected vert otherwise. resize() zero-fills, so seed with ELEM_NONE.
  Vector<int> mirror;
  auto growMirror = [&](int n) {
    int old = int(mirror.size());
    if (n <= old) {
      return;
    }
    mirror.resize(n);
    for (int i = old; i < n; i++) {
      mirror[i] = ELEM_NONE;
    }
  };
  growMirror(m.v.capacity());

  {
    Vector<int> kept;
    for (int vi : m.v) {
      kept.append(vi);
    }
    for (int vi : kept) {
      if (side(vi) == 0) {
        m.v.co[vi][axis] = 0.0f; // snap exactly onto the plane
        mirror[vi] = vi;         // The original and mirror share this vertex, welding them at the plane
        continue;
      }
      int nv = m.make_vertex(m.v.co[vi]);
      growMirror(nv + 1);
      // Copy vi's attrs onto nv FIRST: position is itself a vertex attr (.vert.co),
      // so restoreAttrRow would overwrite a pre-set reflected position. Reflect the
      // axis component afterwards.
      AttrRowSnapshot snap;
      snapshotAttrRow(m.v.attrs, vi, snap);
      restoreAttrRow(m.v.attrs, nv, snap);
      m.v.co[nv][axis] = -m.v.co[nv][axis];
      mirror[vi] = nv;
    }
  }

  // Snapshot the kept edges' feature flags (seam/sharp) before mirrored edges
  // appear, so they can be copied onto the mirror edges afterwards.
  struct EdgeSnap {
    int a, b;
    AttrRowSnapshot snap;
  };
  Vector<EdgeSnap> edgeSnaps;
  for (int ei : m.e) {
    EdgeSnap es;
    es.a = m.e.vs[ei][0];
    es.b = m.e.vs[ei][1];
    snapshotAttrRow(m.e.attrs, ei, es.snap);
    edgeSnaps.append(std::move(es));
  }

  {
    Vector<int> keptF;
    for (int fi : m.f) {
      keptF.append(fi);
    }
    Vector<int> fv, mv;
    Vector<AttrRowSnapshot> csnaps;
    for (int fi : keptF) {
      fv.clear();
      csnaps.clear();
      int li = m.f.l[fi], c0 = m.l.c[li], cc = c0;
      do {
        fv.append(m.c.v[cc]);
        AttrRowSnapshot cs;
        snapshotAttrRow(m.c.attrs, cc, cs);
        csnaps.append(std::move(cs));
        cc = m.c.next[cc];
      } while (cc != c0);

      // A face entirely on the plane maps to itself under mirroring, so it must not be duplicated.
      bool allShared = true;
      for (int vi : fv) {
        if (mirror[vi] != vi) {
          allShared = false;
          break;
        }
      }
      if (allShared) {
        continue;
      }

      mv.clear();
      for (int k = int(fv.size()) - 1; k >= 0; k--) {
        mv.append(mirror[fv[k]]); // reverse winding (reflection flips orientation)
      }
      int nf = m.make_face(std::span<int>(mv.data(), mv.size()));
      if (nf == ELEM_NONE) {
        continue;
      }

      AttrRowSnapshot fsnap;
      snapshotAttrRow(m.f.attrs, fi, fsnap);
      restoreAttrRow(m.f.attrs, nf, fsnap);

      // New-face corner k corresponds to source corner (n-1-k) (reversed order).
      int nli = m.f.l[nf], nc0 = m.l.c[nli], ncc = nc0, k = 0;
      do {
        int srcIdx = int(fv.size()) - 1 - k;
        if (srcIdx >= 0 && srcIdx < int(csnaps.size())) {
          restoreAttrRow(m.c.attrs, ncc, csnaps[srcIdx]);
        }
        ncc = m.c.next[ncc];
        k++;
      } while (ncc != nc0);
    }
  }

  // Carry seam/sharp onto the mirrored edges (on-plane seam edges map to self).
  for (EdgeSnap &es : edgeSnaps) {
    int ma = es.a < int(mirror.size()) ? mirror[es.a] : ELEM_NONE;
    int mb = es.b < int(mirror.size()) ? mirror[es.b] : ELEM_NONE;
    if (ma == ELEM_NONE || mb == ELEM_NONE || (ma == es.a && mb == es.b)) {
      continue;
    }
    int me = m.find_edge(ma, mb);
    if (me != ELEM_NONE) {
      restoreAttrRow(m.e.attrs, me, es.snap);
    }
  }

  m.recalc_normals();
  m.recomputeBoundary();
  return true;
}

} // namespace sculptcore::mesh
