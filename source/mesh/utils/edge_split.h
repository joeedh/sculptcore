#pragma once

/* Edge split: inserts a midpoint vertex on an edge and bisects each
 * incident triangle. This is the M1 primitive of the dynamic-topology
 * plan: the brush refines geometry under the dab by splitting long edges.
 *
 * Precondition: every face incident to the edge is a triangle. Dyntopo
 * keeps the mesh triangulated, so this is asserted rather than handled by
 * triangulating on the fly. A non-triangle incident face is a hard error
 * (returns failure).
 *
 * Behavior:
 *   1. A new vertex `vm` is created at (co[v0] + co[v1]) * 0.5.
 *   2. ALL vertex attributes (not just .co) are interpolated onto vm at
 *      t = 0.5 (lerp of v0/v1), generically over the vertex AttrGroup.
 *      Integer/byte/short attrs are copied from v0 (no meaningful lerp).
 *   3. Each incident triangle (a,b,c) containing the directed pair
 *      (v0,v1) is replaced by two triangles meeting at vm.
 *
 * Topology change (Euler char preserved):
 *   interior edge (2 incident tris):  dV = +1, dE = +3, dF = +2  -> dchi = 0
 *   boundary edge  (1 incident tri):  dV = +1, dE = +2, dF = +1  -> dchi = 0
 *   wire edge      (0 incident tris): dV = +1, dE = +1, dF = +0  -> dchi = 0
 *
 * Implementation strategy mirrors edge_collapse.h: rather than splice the
 * disk/radial cycles by hand, we snapshot the incident triangles, kill
 * them, then rebuild the split triangles through the public Euler
 * operators (`make_vertex` / `make_face`), which keep the cycles
 * self-consistent. make_face(verts) reuses existing edges and creates the
 * new ones, so the resulting edge/face counts follow the deltas above.
 */

#include "../mesh.h"
#include "../mesh_base.h"
#include "../mesh_iter.h"
#include "attr_interp.h"

#include "litestl/math/vector.h"
#include "litestl/util/error.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include "napi/napi_log.h"

#include <span>
#include <type_traits>

namespace sculptcore::mesh {

using litestl::util::SuccessOrError;

struct EdgeSplitResult {
  int new_vert = ELEM_NONE;
  litestl::util::Vector<int, 16> created_edges;
  litestl::util::Vector<int, 16> created_faces;
  litestl::util::Vector<int, 8> killed_faces;
};

static void printEdgeSplitStats(const EdgeSplitResult &res)
{
  sc_napi_logf("split %d faces; created %d edges, %d faces",
               res.killed_faces.size(),
               res.created_edges.size(),
               res.created_faces.size());
}

/* Split `edge` at its midpoint, bisecting every incident triangle. The
 * created/killed element ids are reported through `out` (for the dyntopo
 * driver / meshlog undo). Returns false if the edge index is invalid or
 * any incident face is not a triangle. */
static inline SuccessOrError<"edge_split", "failed to split edge">
splitEdge(Mesh &m, int edge, EdgeSplitResult *out = nullptr, MeshCallbacks *cb = nullptr)
{
  using namespace litestl::util;

  if (edge < 0 || edge >= int(m.e.capacity()) || m.e.freemap[edge]) {
    return false;
  }

  int v0 = m.e.vs[edge][0];
  int v1 = m.e.vs[edge][1];
  if (v0 == v1) {
    return false;
  }

  /* Snapshot the parent edge's attrs (boundary source flags sharp/seam/projected,
   * etc.) so the two child edges can inherit them — make_face creates fresh,
   * value-initialized edges otherwise, which would silently drop a seam. */
  AttrRowSnapshot edgeSnap;
  snapshotAttrRow(m.e.attrs, edge, edgeSnap);

  /* Per-incident-face captured attrs: the face row (e.g. poly-group `group`) and
   * each corner's row (e.g. `uv`), so the rebuilt triangles keep them and the new
   * midpoint corner interpolates. */
  struct FaceSnap {
    AttrRowSnapshot face;
    Vector<int, 4> cverts;
    Vector<AttrRowSnapshot, 4> csnaps;
  };

  /* Gather incident faces (dedup across the radial cycle) and snapshot
   * their vertex sequences + attrs. Require triangles. */
  Vector<int, 4> incidentFaces;
  Vector<Vector<int, 4>, 4> faceVerts;
  Vector<FaceSnap, 4> faceSnaps;
  Set<int, 8> faceSet;
  Vector<int, 4> seq;

  int c0 = m.e.c[edge];
  if (c0 != ELEM_NONE) {
    int cc = c0;
    do {
      int li = m.c.l[cc];
      int fi = m.l.f[li];
      if (faceSet.add(fi)) {
        if (m.l.size[li] != 3 || m.f.list_count[fi] != 1) {
          return false; /* non-triangle incident face */
        }
        seq.clear();
        FaceSnap fs;
        snapshotAttrRow(m.f.attrs, fi, fs.face);
        int lc0 = m.l.c[li];
        int lcc = lc0;
        do {
          seq.append(m.c.v[lcc]);
          fs.cverts.append(m.c.v[lcc]);
          AttrRowSnapshot cs;
          snapshotAttrRow(m.c.attrs, lcc, cs);
          fs.csnaps.append(std::move(cs));
          lcc = m.c.next[lcc];
        } while (lcc != lc0);
        incidentFaces.append(fi);
        faceVerts.append(std::move(seq));
        faceSnaps.append(std::move(fs));
      }
      cc = m.c.radial_next[cc];
    } while (cc != c0);
  }

  /* Create the midpoint vertex and interpolate all vertex attrs. */
  int vm = m.make_vertex((m.v.co[v0] + m.v.co[v1]) * 0.5f, cb);
  interpAttrs(m.v.attrs, vm, v0, v1, 0.5f);

  if (out) {
    out->new_vert = vm;
  }

  /* Kill the incident triangles. The edge itself becomes wire once its
   * last incident face is gone; killing the faces is enough — make_face
   * below recreates the needed edges (including vm's). */
  for (int fi : incidentFaces) {
    if (out) {
      out->killed_faces.append(fi);
    }
    m.kill_face(fi, cb);
  }

  bool wire = incidentFaces.isEmpty();

  /* The original v0-v1 edge is no longer used by any rebuilt triangle
   * (they route through vm). Remove it so it doesn't linger as a wire
   * edge and so the edge-count delta matches the documented Euler change.
   * In the wire case the edge is killed in the branch below. */
  if (!wire && !m.e.freemap[edge]) {
    m.kill_edge(edge, cb);
  }

  if (wire) {
    /* Wire edge: replace v0-v1 with v0-vm and vm-v1. */
    if (!m.e.freemap[edge]) {
      m.kill_edge(edge, cb);
    }
    int e0 = m.make_edge(v0, vm, cb);
    int e1 = m.make_edge(vm, v1, cb);
    restoreAttrRow(m.e.attrs, e0, edgeSnap); /* inherit parent edge flags */
    restoreAttrRow(m.e.attrs, e1, edgeSnap);
    if (out) {
      out->created_edges.append(e0);
      out->created_edges.append(e1);
    }
    return true;
  }

  /* Locate the corner of face `f` whose vertex is `vert` (ELEM_NONE if absent). */
  auto cornerOf = [&](int f, int vert) -> int {
    int li = m.f.l[f];
    int lc0 = m.l.c[li], lcc = lc0;
    do {
      if (m.c.v[lcc] == vert) {
        return lcc;
      }
      lcc = m.c.next[lcc];
    } while (lcc != lc0);
    return ELEM_NONE;
  };

  /* Rebuild each incident triangle as two triangles through vm. For a
   * triangle whose vertex loop contains the consecutive pair (v0,v1) or
   * (v1,v0), insert vm between them; the resulting 4-vertex loop fans
   * into two triangles preserving the original winding. */
  for (int fidx = 0; fidx < int(faceVerts.size()); fidx++) {
    auto &seq = faceVerts[fidx];
    FaceSnap &fs = faceSnaps[fidx];
    int n = int(seq.size()); /* == 3 */
    /* Find the index i where (seq[i], seq[i+1]) is the split edge. */
    int splitI = -1;
    for (int i = 0; i < n; i++) {
      int a = seq[i];
      int b = seq[(i + 1) % n];
      if ((a == v0 && b == v1) || (a == v1 && b == v0)) {
        splitI = i;
        break;
      }
    }
    if (splitI < 0) {
      continue; /* shouldn't happen */
    }

    int a = seq[splitI];             /* one endpoint of split edge */
    int b = seq[(splitI + 1) % n];   /* other endpoint */
    int opp = seq[(splitI + 2) % n]; /* apex */

    /* Winding a -> b -> opp. Bisect: (a, vm, opp) and (vm, b, opp). */
    int tri0[3] = {a, vm, opp};
    int tri1[3] = {vm, b, opp};
    int f0 = m.make_face(std::span<int>(tri0, 3), cb);
    int f1 = m.make_face(std::span<int>(tri1, 3), cb);

    /* Carry the original face's attrs onto both halves, the original corners
     * onto the matching corners, and interpolate the midpoint corner from the
     * split edge's two endpoint corners. */
    auto snapForVert = [&](int vert) -> const AttrRowSnapshot * {
      for (int i = 0; i < int(fs.cverts.size()); i++) {
        if (fs.cverts[i] == vert) {
          return &fs.csnaps[i];
        }
      }
      return nullptr;
    };
    restoreAttrRow(m.f.attrs, f0, fs.face);
    restoreAttrRow(m.f.attrs, f1, fs.face);
    const AttrRowSnapshot *snA = snapForVert(a), *snB = snapForVert(b),
                          *snO = snapForVert(opp);
    int cA = cornerOf(f0, a), cVm0 = cornerOf(f0, vm), cO0 = cornerOf(f0, opp);
    int cVm1 = cornerOf(f1, vm), cB = cornerOf(f1, b), cO1 = cornerOf(f1, opp);
    if (snA && cA != ELEM_NONE)
      restoreAttrRow(m.c.attrs, cA, *snA);
    if (snO && cO0 != ELEM_NONE)
      restoreAttrRow(m.c.attrs, cO0, *snO);
    if (snB && cB != ELEM_NONE)
      restoreAttrRow(m.c.attrs, cB, *snB);
    if (snO && cO1 != ELEM_NONE)
      restoreAttrRow(m.c.attrs, cO1, *snO);
    if (snA && snB && cVm0 != ELEM_NONE)
      interpAttrRows(m.c.attrs, cVm0, *snA, *snB, 0.5f);
    if (snA && snB && cVm1 != ELEM_NONE)
      interpAttrRows(m.c.attrs, cVm1, *snA, *snB, 0.5f);

    if (out) {
      out->created_faces.append(f0);
      out->created_faces.append(f1);
    }
  }

  /* The two child edges (v0-vm, vm-v1) inherit the parent edge's attrs (boundary
   * source flags). The spoke edges vm-opp are genuinely new (no flag). */
  if (m.v.e[vm] != ELEM_NONE) {
    for (int ei : EdgeOfVertIter(&m, vm, m.v.e[vm])) {
      int o = (m.e.vs[ei][0] == vm) ? m.e.vs[ei][1] : m.e.vs[ei][0];
      if (o == v0 || o == v1) {
        restoreAttrRow(m.e.attrs, ei, edgeSnap);
      }
    }
  }

  /* Every edge incident to the new midpoint is new (vm did not exist before),
   * so gather them locally — O(valence), not an O(total edges) freemap diff. */
  if (out && m.v.e[vm] != ELEM_NONE) {
    for (int ei : EdgeOfVertIter(&m, vm, m.v.e[vm])) {
      out->created_edges.append(ei);
    }
  }

  return true;
}

} // namespace sculptcore::mesh
