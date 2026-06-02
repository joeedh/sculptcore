#pragma once

/* Edge collapse: merges the two endpoints of an edge into a single vertex,
 * welding adjacent geometry. Faces incident to the collapsed edge that
 * become degenerate (e.g. triangles, where two corners would coincide)
 * are removed; their two remaining edges are merged into one shared edge.
 * Larger faces simply lose one corner.
 *
 * The kept vertex is `e.vs[0]` (v_keep); `e.vs[1]` (v_kill) is removed.
 * Optionally the kept vertex's position can be set to a user-provided
 * blended location.
 *
 * Topology change for a typical interior triangle-mesh collapse:
 *   dV = -1, dE = -3, dF = -2  -> dchi = 0 (preserves Euler char).
 * For collapses on a boundary or quad-only mesh the deltas differ but
 * the change in chi remains consistent with the local topology change.
 *
 * No new boundary loops are introduced: when a triangle collapses, its
 * two non-collapsed edges are merged (their radial cycles spliced),
 * so any face that was on the far side of one of those edges remains
 * attached to the surviving merged edge.
 *
 * Implementation strategy: we reconstruct rather than splice cycles by
 * hand. We gather all faces touching either endpoint, kill them, kill
 * the edges incident to v_kill, kill v_kill, then recreate each face
 * with v_kill mapped to v_keep (dropping faces that go degenerate or
 * duplicate). This relies entirely on the public Euler operators in
 * `Mesh`, so the disk/radial cycles stay self-consistent.
 */

#include "../mesh.h"
#include "../mesh_base.h"
#include "../mesh_iter.h"
#include "../mesh_proxy.h"
#include "attr_interp.h"

#include "litestl/math/vector.h"
#include "litestl/util/error.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <optional>
#include <span>

namespace sculptcore::mesh {

using litestl::util::SuccessOrError;

/* Created / killed element ids, for the dyntopo driver and the meshlog. */
struct EdgeCollapseResult {
  int v_keep = ELEM_NONE;
  int killed_vert = ELEM_NONE;
  litestl::util::Vector<int> created_faces;
  litestl::util::Vector<int> created_edges; /* edges incident to v_keep that are new */
  litestl::util::Vector<int> killed_faces;
  litestl::util::Vector<int> killed_edges;
};

namespace detail_collapse {

/* Number of faces on the radial cycle of `edge` (0 = wire, 1 = boundary,
 * 2 = interior manifold, >2 = non-manifold). */
static inline int edgeRadialFaceCount(Mesh &m, int edge)
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

static inline int64_t faceKey(const litestl::util::Vector<int, 8> &verts)
{
  /* Order-invariant key: rotate so smallest first, then pick the lexicographically
   * smaller of forward/reverse. This dedupes faces regardless of starting corner
   * or winding. */
  int n = int(verts.size());
  if (n == 0) return 0;
  int min_i = 0;
  for (int i = 1; i < n; i++) {
    if (verts[i] < verts[min_i]) min_i = i;
  }
  litestl::util::Vector<int, 8> fwd, rev;
  for (int i = 0; i < n; i++) fwd.append(verts[(min_i + i) % n]);
  rev.append(fwd[0]);
  for (int i = n - 1; i >= 1; i--) rev.append(fwd[i]);
  bool useFwd = true;
  for (int i = 1; i < n; i++) {
    if (fwd[i] != rev[i]) {
      useFwd = fwd[i] < rev[i];
      break;
    }
  }
  uint64_t h = 1469598103934665603ull;
  const auto &use = useFwd ? fwd : rev;
  for (int v : use) {
    h ^= uint64_t(uint32_t(v));
    h *= 1099511628211ull;
  }
  return int64_t(h);
}

} /* namespace detail_collapse */

/* Collapse `edge`. The vertex at `e.vs[edge][0]` is kept (its position
 * optionally replaced by `merged_co`); the vertex at `e.vs[edge][1]`
 * is removed. Returns false if the edge index is invalid. */
static inline SuccessOrError<"edge_collapse", "failed to collapse edge">
collapseEdge(Mesh &m, int edge,
             std::optional<litestl::math::float3> merged_co = std::nullopt,
             float blend = 0.0f, EdgeCollapseResult *out = nullptr,
             MeshCallbacks *cb = nullptr)
{
  using namespace litestl;
  using namespace litestl::util;

  if (edge < 0 || edge >= int(m.e.capacity()) || m.e.freemap[edge]) {
    return false;
  }

  int v_keep = m.e.vs[edge][0];
  int v_kill = m.e.vs[edge][1];
  if (out) {
    out->v_keep = v_keep;
    out->killed_vert = v_kill;
  }
  if (v_keep == v_kill) {
    /* Self-loop: just remove. */
    if (out) {
      out->killed_vert = ELEM_NONE;
      out->killed_edges.append(edge);
    }
    m.kill_edge(edge, cb);
    return true;
  }

  /* Link condition: an excess of vertices common to the one-rings of v_keep
   * and v_kill (more than the number of faces on the edge) means a triangle
   * not incident to the edge would fold onto itself, producing a non-manifold
   * result. Refuse such collapses (leaving the mesh untouched). */
  {
    int faceCount = detail_collapse::edgeRadialFaceCount(m, edge);
    Set<int> nbrKeep;
    if (m.v.e[v_keep] != ELEM_NONE) {
      for (int e2 : EdgeOfVertIter(&m, v_keep, m.v.e[v_keep])) {
        int o = (m.e.vs[e2][0] == v_keep) ? m.e.vs[e2][1] : m.e.vs[e2][0];
        nbrKeep.add(o);
      }
    }
    int common = 0;
    if (m.v.e[v_kill] != ELEM_NONE) {
      for (int e2 : EdgeOfVertIter(&m, v_kill, m.v.e[v_kill])) {
        int o = (m.e.vs[e2][0] == v_kill) ? m.e.vs[e2][1] : m.e.vs[e2][0];
        if (o == v_keep) {
          continue;
        }
        if (nbrKeep.contains(o)) {
          common++;
        }
      }
    }
    if (common > faceCount) {
      return false;
    }
  }

  /* Optionally blend the survivor's attributes toward v_kill before the merge
   * (0 = keep v_keep unchanged, 0.5 = midpoint). Reads v_kill, so it must run
   * before v_kill is killed below. Position is overridden by merged_co if set. */
  if (blend > 0.0f) {
    interpAttrs(m.v.attrs, v_keep, v_keep, v_kill, blend);
  }

  /* Snapshot the attrs of every edge incident to either endpoint (keyed by the
   * far vertex) so the merged/recreated edges can re-inherit them — collapse
   * kills v_kill's edges and rebuilds, which would otherwise drop boundary source
   * flags (seam/sharp) on a feature curve being coarsened. */
  Vector<int, 16> edgeFlagOther;
  Vector<AttrRowSnapshot, 16> edgeFlagSnap;
  auto recordEdgeFlags = [&](int vi) {
    if (m.v.e[vi] == ELEM_NONE) return;
    for (int ei : EdgeOfVertIter(&m, vi, m.v.e[vi])) {
      int o = (m.e.vs[ei][0] == vi) ? m.e.vs[ei][1] : m.e.vs[ei][0];
      if (o == v_kill || o == v_keep) continue; /* the collapsed/cross edge */
      AttrRowSnapshot s;
      snapshotAttrRow(m.e.attrs, ei, s);
      edgeFlagOther.append(o);
      edgeFlagSnap.append(std::move(s));
    }
  };
  recordEdgeFlags(v_keep);
  recordEdgeFlags(v_kill);

  /* 1. Gather all faces touching either endpoint, recording their vertex
   *    sequences. Use a set to avoid adding the same face twice (a face
   *    can touch both endpoints). */
  Vector<int> facesToRebuild;
  Set<int> faceSet;

  auto gatherFaces = [&](int vi) {
    if (m.v.e[vi] == ELEM_NONE) return;
    for (int ei : EdgeOfVertIter(&m, vi, m.v.e[vi])) {
      int c0 = m.e.c[ei];
      if (c0 == ELEM_NONE) continue;
      int cc = c0;
      do {
        int li = m.c.l[cc];
        int fi = m.l.f[li];
        if (faceSet.add(fi)) {
          facesToRebuild.append(fi);
        }
        cc = m.c.radial_next[cc];
      } while (cc != c0);
    }
  };
  /* Only faces touching v_kill need rebuilding — faces touching only
   * v_keep are unchanged by the merge. */
  gatherFaces(v_kill);

  /* Snapshot vertex sequences (outer list only — we don't recreate holes) plus
   * each face's attr row and per-corner rows (keyed by vertex), so the rebuilt
   * faces keep their `group` / `uv` / etc. instead of make_face's value-init. */
  struct FaceSnap {
    AttrRowSnapshot face;
    Vector<int, 8> cverts;
    Vector<AttrRowSnapshot, 8> csnaps;
  };
  Vector<Vector<int, 8>> faceVerts;
  Vector<FaceSnap> faceSnaps;
  for (int fi : facesToRebuild) {
    Vector<int, 8> seq;
    FaceSnap fs;
    snapshotAttrRow(m.f.attrs, fi, fs.face);
    int li = m.f.l[fi];
    int c0 = m.l.c[li];
    int cc = c0;
    do {
      seq.append(m.c.v[cc]);
      fs.cverts.append(m.c.v[cc]);
      AttrRowSnapshot cs;
      snapshotAttrRow(m.c.attrs, cc, cs);
      fs.csnaps.append(std::move(cs));
      cc = m.c.next[cc];
    } while (cc != c0);
    faceVerts.append(std::move(seq));
    faceSnaps.append(std::move(fs));
  }

  /* 2. Kill all gathered faces. */
  for (int fi : facesToRebuild) {
    if (out) {
      out->killed_faces.append(fi);
    }
    m.kill_face(fi, cb);
  }

  /* 3. Kill all edges incident to v_kill (including `edge` itself, which
   *    is now wire). Walking the disk while killing requires care: snapshot
   *    first. */
  Vector<int> edgesToKill;
  if (m.v.e[v_kill] != ELEM_NONE) {
    for (int ei : EdgeOfVertIter(&m, v_kill, m.v.e[v_kill])) {
      edgesToKill.append(ei);
    }
  }
  /* Also any wire edges incident to v_keep that go to v_kill (already
   * captured above since both endpoints are walked). */
  for (int ei : edgesToKill) {
    /* Edge may already be gone if collapse reduced something earlier;
     * guard with freemap. */
    if (!m.e.freemap[ei]) {
      if (out) {
        out->killed_edges.append(ei);
      }
      m.kill_edge(ei, cb);
    }
  }

  /* 4. Kill v_kill (now isolated). Its edges are already gone, so kill_vertex
   *    just fires onVertKill (for the meshlog) and releases. */
  if (!m.v.freemap[v_kill]) {
    m.kill_vertex(v_kill, cb);
  }

  /* 5. Optional: update kept vertex position. */
  if (merged_co.has_value()) {
    m.v.co[v_keep] = merged_co.value();
  }

  /* Snapshot v_keep's neighbors (by the other endpoint, so slot reuse can't
   * fool it) so created_edges can be reported by an O(valence) diff rather than
   * an O(total edges) freemap scan. All new edges of a collapse are incident to
   * v_keep. */
  Set<int> keepBefore;
  if (out && m.v.e[v_keep] != ELEM_NONE) {
    for (int ei : EdgeOfVertIter(&m, v_keep, m.v.e[v_keep])) {
      keepBefore.add((m.e.vs[ei][0] == v_keep) ? m.e.vs[ei][1] : m.e.vs[ei][0]);
    }
  }

  /* 6. Remap face sequences (v_kill -> v_keep), drop degenerates and
   *    duplicates, then rebuild. */
  Set<int64_t, 64> rebuiltKeys;
  for (int fidx = 0; fidx < int(faceVerts.size()); fidx++) {
    auto &seq = faceVerts[fidx];
    FaceSnap &fs = faceSnaps[fidx];
    Vector<int, 8> remapped;
    for (int v : seq) {
      int rv = (v == v_kill) ? v_keep : v;
      /* Skip consecutive duplicates. */
      if (!remapped.isEmpty() && remapped[remapped.size() - 1] == rv) continue;
      remapped.append(rv);
    }
    /* Wrap-around duplicate. */
    while (remapped.size() >= 2 && remapped[0] == remapped[remapped.size() - 1]) {
      remapped.pop_back();
    }
    /* Also dedupe non-adjacent repeats (a quad with v_keep already adjacent
     * to v_kill on opposite corners would yield a degenerate). */
    {
      Set<int> seen;
      bool repeat = false;
      for (int v : remapped) {
        if (!seen.add(v)) {
          repeat = true;
          break;
        }
      }
      if (repeat) continue;
    }
    if (remapped.size() < 3) continue;

    int64_t key = detail_collapse::faceKey(remapped);
    if (!rebuiltKeys.add(key)) continue;

    int f = m.make_face(std::span<int>(remapped.data(), remapped.size()), cb);

    /* Carry the original face's attrs + corners. A rebuilt corner now at v_keep
     * was originally v_kill (faces with both endpoints went degenerate above),
     * so look its snapshot up by the original vertex. */
    restoreAttrRow(m.f.attrs, f, fs.face);
    int li = m.f.l[f];
    int cc0 = m.l.c[li], cc = cc0;
    do {
      int nv = m.c.v[cc];
      int orig = (nv == v_keep) ? v_kill : nv;
      for (int i = 0; i < int(fs.cverts.size()); i++) {
        if (fs.cverts[i] == orig) {
          restoreAttrRow(m.c.attrs, cc, fs.csnaps[i]);
          break;
        }
      }
      cc = m.c.next[cc];
    } while (cc != cc0);

    if (out) {
      out->created_faces.append(f);
    }
  }

  /* Re-apply the snapshotted edge attrs (boundary source flags) onto v_keep's
   * edges, matched by the far vertex — restores flags onto merged/recreated
   * feature-curve edges. */
  if (m.v.e[v_keep] != ELEM_NONE) {
    for (int ei : EdgeOfVertIter(&m, v_keep, m.v.e[v_keep])) {
      int o = (m.e.vs[ei][0] == v_keep) ? m.e.vs[ei][1] : m.e.vs[ei][0];
      for (int k = 0; k < int(edgeFlagOther.size()); k++) {
        if (edgeFlagOther[k] == o) {
          restoreAttrRow(m.e.attrs, ei, edgeFlagSnap[k]);
          break;
        }
      }
    }
  }

  /* Report edges that became live during the rebuild (the merged/new edges
   * incident to v_keep). */
  if (out && m.v.e[v_keep] != ELEM_NONE) {
    for (int ei : EdgeOfVertIter(&m, v_keep, m.v.e[v_keep])) {
      int o = (m.e.vs[ei][0] == v_keep) ? m.e.vs[ei][1] : m.e.vs[ei][0];
      if (!keepBefore.contains(o)) {
        out->created_edges.append(ei);
      }
    }
  }

  return true;
}

} /* namespace sculptcore::mesh */
