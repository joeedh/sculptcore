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
 *   3. Each incident triangle (a,b,opp) containing the directed pair
 *      (v0,v1) is replaced by two triangles meeting at vm.
 *
 * Topology change (Euler char preserved):
 *   interior edge (2 incident tris):  dV = +1, dE = +3, dF = +2  -> dchi = 0
 *   boundary edge  (1 incident tri):  dV = +1, dE = +2, dF = +1  -> dchi = 0
 *   wire edge      (0 incident tris): dV = +1, dE = +1, dF = +0  -> dchi = 0
 *
 * Implementation: the split is performed IN PLACE. The input edge keeps its
 * id (relinked to the v0-vm child) and EACH incident face keeps its id (one
 * bisected half is rewritten in place via clear_face_contents/reinit_face,
 * the other half is created with make_face). Genuinely new are the midpoint
 * vertex, the vm-v1 child edge, one vm-opp spoke per incident face, the
 * second-half faces, and all corners. Reusing the face ids means the spatial
 * tree only re-flags the reused leaves (touch_face) instead of churning node
 * ownership (remove_face + add_face on a recycled id), and the meshlog logs
 * the reused edge/faces as Changes (swap on undo) rather than kill+create
 * pairs.
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

/* In-place split. The input edge and each incident face keep their ids, so the
 * reported ids overlap the originals: `created_edges` is the GROSS set of edges
 * at vm (the reused v0-vm child, the new vm-v1 child, and one spoke per face);
 * `created_faces` lists both halves of every incident triangle (the reused id +
 * the new id); `killed_faces` lists the reused incident face ids (rewritten in
 * place, not actually freed). The gross sets preserve the dyntopo frontier
 * contract (addCreated consumes created_edges). */
struct EdgeSplitResult {
  int new_vert = ELEM_NONE;
  litestl::util::Vector<int, 16> created_edges;
  litestl::util::Vector<int, 16> created_faces;
  litestl::util::Vector<int, 8> killed_faces;
};

/* Split `edge` at its midpoint, bisecting every incident triangle. The
 * created/killed element ids are reported through `out` (for the dyntopo
 * driver / meshlog undo). `edge` must be a live edge — the caller is
 * responsible for validating it; this does not. Returns false if the edge
 * is degenerate (v0 == v1) or any incident face is not a triangle. */
static inline SuccessOrError<"edge_split", "failed to split edge">
splitEdge(Mesh &m, int edge, EdgeSplitResult *out = nullptr, MeshCallbacks *cb = nullptr)
{
  using namespace litestl::util;

  int v0 = m.e.vs[edge][0];
  int v1 = m.e.vs[edge][1];
  if (v0 == v1) {
    return false;
  }

  /* Snapshot the parent edge's attrs (boundary source flags sharp/seam/projected,
   * etc.) so the new vm-v1 child can inherit them — make_edge creates a fresh,
   * value-initialized edge otherwise, which would silently drop a seam. The
   * reused v0-vm child IS the input edge, so it keeps these attrs natively. */
  AttrRowSnapshot edgeSnap;
  snapshotAttrRow(m.e.attrs, edge, edgeSnap);

  /* Per-incident-face capture: the two endpoints in winding order (a,b with
   * {a,b}=={v0,v1}), the apex opp, the two non-split edges (e_b_opp = b->opp,
   * e_opp_a = opp->a), the face attr row (e.g. poly-group `group`), and each
   * corner's attr row (e.g. uv) keyed by vertex — so the rebuilt halves keep
   * them and the new midpoint corner interpolates. */
  struct FaceSnap {
    int f = ELEM_NONE;
    int a = ELEM_NONE, b = ELEM_NONE, opp = ELEM_NONE;
    int e_b_opp = ELEM_NONE, e_opp_a = ELEM_NONE;
    AttrRowSnapshot face;
    Vector<int, 4> cverts;
    Vector<AttrRowSnapshot, 4> csnaps;
  };

  Vector<FaceSnap, 4> faceSnaps;
  Set<int, 8> faceSet;

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
        FaceSnap fs;
        fs.f = fi;
        snapshotAttrRow(m.f.attrs, fi, fs.face);

        /* `cc` is the corner of this face on the split edge: c.v[cc] = a,
         * c.e[cc] = edge, and the loop winds a -> b -> opp. */
        int cA = cc;
        int cB = m.c.next[cA];
        int cOpp = m.c.next[cB];
        fs.a = m.c.v[cA];
        fs.b = m.c.v[cB];
        fs.opp = m.c.v[cOpp];
        fs.e_b_opp = m.c.e[cB];
        fs.e_opp_a = m.c.e[cOpp];

        int lcc = cA;
        do {
          fs.cverts.append(m.c.v[lcc]);
          AttrRowSnapshot cs;
          snapshotAttrRow(m.c.attrs, lcc, cs);
          fs.csnaps.append(std::move(cs));
          lcc = m.c.next[lcc];
        } while (lcc != cA);

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

  bool wire = faceSnaps.isEmpty();

  /* Tear the incident faces' contents down first (keeping their ids), so the
   * split edge becomes a wire edge before we relink it. After this loop the
   * face ids in faceSnaps[*].f are live but empty (f.l == ELEM_NONE). */
  for (FaceSnap &fs : faceSnaps) {
    m.clear_face_contents(fs.f, cb);
  }

  /* Reuse the input edge as the v0-vm child (id + attrs preserved), and create
   * the vm-v1 child (inheriting the parent edge's boundary flags). For a wire
   * edge there are no faces, but the same two children are the whole job. */
  m.relink_edge_verts(edge, v0, vm, cb);
  int e_new = m.make_edge(vm, v1, cb);
  restoreAttrRow(m.e.attrs, e_new, edgeSnap);

  if (wire) {
    if (out) {
      /* Gross: both children meet at vm. */
      for (int ei : EdgeOfVertIter(&m, vm, m.v.e[vm])) {
        out->created_edges.append(ei);
      }
    }
    return true;
  }

  /* Rebuild each incident triangle as two triangles through vm. T0 reuses the
   * incident face id; T1 is created. edge(a,vm)/edge(vm,b) are the two children
   * (which one is the reused `edge` vs `e_new` depends on the face's winding). */
  auto snapForVert = [&](FaceSnap &fs, int vert) -> const AttrRowSnapshot * {
    for (int i = 0; i < int(fs.cverts.size()); i++) {
      if (fs.cverts[i] == vert) {
        return &fs.csnaps[i];
      }
    }
    return nullptr;
  };
  auto cornerOf = [&m](int f, int vert) -> int {
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

  for (FaceSnap &fs : faceSnaps) {
    int a = fs.a, b = fs.b, opp = fs.opp;
    int e_a_vm = (a == v0) ? edge : e_new;  /* child edge a->vm */
    int e_vm_b = (b == v0) ? edge : e_new;  /* child edge vm->b */
    int spoke = m.make_edge(vm, opp, cb);

    /* T0 = (a, vm, opp) reuses the incident face id. */
    int t0v[3] = {a, vm, opp};
    int t0e[3] = {e_a_vm, spoke, fs.e_opp_a};
    m.reinit_face(fs.f, std::span<int>(t0v, 3), std::span<int>(t0e, 3), cb);
    int f0 = fs.f;

    /* T1 = (vm, b, opp) is a new face. */
    int t1v[3] = {vm, b, opp};
    int t1e[3] = {e_vm_b, fs.e_b_opp, spoke};
    int f1 = m.make_face(std::span<int>(t1v, 3), std::span<int>(t1e, 3), cb);

    /* Carry face attrs onto both halves, the original endpoint/apex corner
     * attrs onto the matching corners, and interpolate the midpoint corner from
     * the split edge's two endpoint corners. */
    restoreAttrRow(m.f.attrs, f0, fs.face);
    restoreAttrRow(m.f.attrs, f1, fs.face);

    const AttrRowSnapshot *snA = snapForVert(fs, a), *snB = snapForVert(fs, b),
                          *snO = snapForVert(fs, opp);
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
      out->killed_faces.append(fs.f); /* reused id (rewritten, not freed) */
      out->created_faces.append(f0);
      out->created_faces.append(f1);
    }
  }

  /* Every edge incident to the new midpoint is part of the split (vm did not
   * exist before): the reused v0-vm child, the new vm-v1 child, and the spokes.
   * Gather them locally — O(valence), and GROSS so dyntopo's frontier matches
   * the documented contract. */
  if (out && m.v.e[vm] != ELEM_NONE) {
    for (int ei : EdgeOfVertIter(&m, vm, m.v.e[vm])) {
      out->created_edges.append(ei);
    }
  }

  return true;
}

} // namespace sculptcore::mesh
