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

#include <span>
#include <type_traits>

namespace sculptcore::mesh {

using litestl::util::SuccessOrError;

struct EdgeSplitResult {
  int new_vert = ELEM_NONE;
  litestl::util::Vector<int> created_edges;
  litestl::util::Vector<int> created_faces;
  litestl::util::Vector<int> killed_faces;
};

/* Split `edge` at its midpoint, bisecting every incident triangle. The
 * created/killed element ids are reported through `out` (for the dyntopo
 * driver / meshlog undo). Returns false if the edge index is invalid or
 * any incident face is not a triangle. */
static inline SuccessOrError<"edge_split", "failed to split edge">
splitEdge(Mesh &m, int edge, EdgeSplitResult *out = nullptr,
          MeshCallbacks *cb = nullptr)
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

  /* Gather incident faces (dedup across the radial cycle) and snapshot
   * their vertex sequences. Require triangles. */
  Vector<int, 8> incidentFaces;
  Vector<Vector<int, 4>, 8> faceVerts;
  Set<int> faceSet;

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
        Vector<int, 4> seq;
        int lc0 = m.l.c[li];
        int lcc = lc0;
        do {
          seq.append(m.c.v[lcc]);
          lcc = m.c.next[lcc];
        } while (lcc != lc0);
        incidentFaces.append(fi);
        faceVerts.append(std::move(seq));
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

  /* Snapshot edge counts to detect newly created edges/faces for `out`. */
  int eCapBefore = int(m.e.capacity());
  BoolVector<> eLiveBefore;
  if (out) {
    eLiveBefore.resize(eCapBefore);
    for (int i = 0; i < eCapBefore; i++) {
      eLiveBefore.set(i, !m.e.freemap[i]);
    }
  }

  if (wire) {
    /* Wire edge: replace v0-v1 with v0-vm and vm-v1. */
    if (!m.e.freemap[edge]) {
      m.kill_edge(edge, cb);
    }
    int e0 = m.make_edge(v0, vm, cb);
    int e1 = m.make_edge(vm, v1, cb);
    if (out) {
      out->created_edges.append(e0);
      out->created_edges.append(e1);
    }
    return true;
  }

  /* Rebuild each incident triangle as two triangles through vm. For a
   * triangle whose vertex loop contains the consecutive pair (v0,v1) or
   * (v1,v0), insert vm between them; the resulting 4-vertex loop fans
   * into two triangles preserving the original winding. */
  for (auto &seq : faceVerts) {
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

    int a = seq[splitI];            /* one endpoint of split edge */
    int b = seq[(splitI + 1) % n];  /* other endpoint */
    int opp = seq[(splitI + 2) % n]; /* apex */

    /* Winding a -> b -> opp. Bisect: (a, vm, opp) and (vm, b, opp). */
    int tri0[3] = {a, vm, opp};
    int tri1[3] = {vm, b, opp};
    int f0 = m.make_face(std::span<int>(tri0, 3), cb);
    int f1 = m.make_face(std::span<int>(tri1, 3), cb);
    if (out) {
      out->created_faces.append(f0);
      out->created_faces.append(f1);
    }
  }

  if (out) {
    int eCapAfter = int(m.e.capacity());
    for (int i = 0; i < eCapAfter; i++) {
      bool wasLive = (i < eCapBefore) && eLiveBefore[i];
      if (!m.e.freemap[i] && !wasLive) {
        out->created_edges.append(i);
      }
    }
  }

  return true;
}

} // namespace sculptcore::mesh
