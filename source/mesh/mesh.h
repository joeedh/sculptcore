#pragma once

#include "attribute.h"
#include "litestl/binding/binding.h"
#include "litestl/math/vector.h"
#include "litestl/util/span.h"
#include "litestl/util/string.h"

#include "mesh_base.h"
#include "mesh_callbacks.h"
#include "mesh_enums.h"
#include "mesh_proxy.h"
#include "mesh_topo_cache.h"
#include "mesh_types.h"

#include <algorithm>
#include <concepts>
#include <cstdio>
#include <type_traits>

#include <cfloat>
#include <span>

using namespace litestl;
namespace sculptcore::mesh {

/* A detached attribute layer parked by detachAttr() for undo. Holds the live
 * AttrRef (including its AttrData pointer) out of the element group without
 * freeing it; reattachAttr() moves it back. `ref.data == nullptr` marks an
 * entry whose data has been reattached (the group owns it again).
 *
 * INVARIANT: no topology edit may happen between detach and reattach. The
 * stashed AttrData keeps its detach-time element count (`count`); a make_/kill_
 * on the domain while detached would desync it from the group, so reattachAttr
 * verifies the count is unchanged. The undo flow (detach → immediate reattach,
 * no intervening topo edit) upholds this. */
struct StashedAttr {
  AttrRef ref;
  int domain = 0;
  int count = 0;
};

struct Mesh : public MeshBase {
  Mesh()
  {
  }

  /* Free any still-detached (un-reattached) stash entries' AttrData — they are
   * not in any element group, so nothing else frees them. Reattached entries
   * have ref.data nulled (the group owns them) and are skipped. */
  ~Mesh()
  {
    for (StashedAttr &st : attrStash) {
      if (!st.ref.data) {
        continue;
      }
      detail::type_dispatch(st.ref.type, [&]<typename T>() {
        if constexpr (std::is_same_v<T, bool>) {
          alloc::Delete(static_cast<BoolAttrView *>(st.ref.data));
        } else {
          alloc::Delete(static_cast<AttrData<T> *>(st.ref.data));
        }
      });
    }
  }

  /* Detached-attr stash for undoable removes (see detachAttr/reattachAttr). */
  util::Vector<StashedAttr> attrStash;

  /* Monotonic topology-edit counter. Bumped by every primitive topology
   * mutator (the make_, kill_ and reorder_ families); MeshTopoCache keys its
   * validity on it so any edit forces a rebuild. */
  uint64_t topo_stamp = 1;
  MeshTopoCache topo_cache;

  /* Frozen-topology mode: the live TOPO link columns are dropped and
   * topo_cache.frozen is authoritative. A RAM-saving cache state, never lossy
   * — any topology mutator auto-thaws (rebuilds the live links) first, so the
   * mode is invisible to *mutating* callers. See freezeTopo()/thawTopo().
   *
   * What stays valid while frozen (the sculpt-loop contract):
   *   - geometry attrs (v.co, v.no, ...) — never TOPO, always live.
   *   - the 1-ring CSR (topo_cache.ring1) — serves brush for_neighbor.
   *   - .corner.v — flagged TOPO_KEEP_FROZEN so the per-frame spatial path
   *     (tri bounds/normals/GPU upload, which reads c.v through cached corner
   *     indices) keeps working without a thaw.
   * The pure-iteration links (disk/radial/loop, v.e, e.c, c.next/prev/e/l,
   * l.*, f.l) are gone; any code that walks them must go through a mutator
   * (auto-thaws) or call thawTopo() first (recalc_normals does). */
  bool topo_frozen = false;

  /* Build the frozen snapshot and release the live TOPO pages. Ensures the
   * 1-ring CSR is current first so the brush path stays served while frozen. */
  void freezeTopo();
  /* Re-materialize the TOPO pages and rebuild the live links from the snapshot.
   * Idempotent; a no-op when not frozen. */
  void thawTopo();

  /* Live count of faces with >3 sides. Maintained incrementally by make_face /
   * kill_face (the only per-face create/destroy choke points), so == 0 is an
   * exact "mesh is all-triangles" predicate. Bulk loaders (readMesh) bypass
   * those primitives and must call recountNgons() to resync. dyntopo skips its
   * triangulate prepass when this is 0 (a self-maintained tri mesh never needs
   * it). */
  int64_t n_ngon_faces = 0;
  /* Rescan all live faces and reset n_ngon_faces. O(faces); call once after a
   * bulk build that doesn't route through make_face. */
  void recountNgons();
  /* Live count of n-gon (>3 sided) faces, as an int for the JS binding (the
   * count never approaches INT_MAX). == 0 means the mesh is all-triangles;
   * the UI uses it to offer/guard the triangulate op. */
  int ngonFaceCount() const { return int(n_ngon_faces); }

  static binding::types::Struct<Mesh> *defineBindings()
  {
    using binding::types::Struct;

    Struct<Mesh> *st = new Struct<Mesh>("sculptcore::mesh::Mesh", sizeof(Mesh));
    BIND_STRUCT_MEMBER(st, v);
    BIND_STRUCT_MEMBER(st, e);
    BIND_STRUCT_MEMBER(st, c);
    BIND_STRUCT_MEMBER(st, f);
    BIND_STRUCT_METHOD(st, recalc_normals, MARGS());
    BIND_STRUCT_METHOD(st, faceGroup, MARGS("face"));
    BIND_STRUCT_METHOD(st, maxFaceGroup, MARGS());
    BIND_STRUCT_METHOD(st, ngonFaceCount, MARGS());
    BIND_STRUCT_METHOD(st, setAttrUse, MARGS("domain", "index", "use"));
    BIND_STRUCT_METHOD(st, addAttr, MARGS("domain", "type", "use"));
    BIND_STRUCT_METHOD(st, removeAttr, MARGS("domain", "index"));
    BIND_STRUCT_METHOD(st, detachAttr, MARGS("domain", "index"));
    BIND_STRUCT_METHOD(st, reattachAttr, MARGS("stashId"));
    BIND_STRUCT_METHOD(st, markSeamPath, MARGS("vStart", "vEnd", "state"));
    BIND_STRUCT_METHOD(st, edgePathEdges, MARGS("vStart", "vEnd", "out"));
    BIND_STRUCT_METHOD(st, edgeSeam, MARGS("e"));
    BIND_STRUCT_METHOD(st, setEdgeSeam, MARGS("e", "state"));
    BIND_STRUCT_METHOD(st, recomputeBoundary, MARGS());
    BIND_STRUCT_METHOD(st, boundaryGraphStats, MARGS("out"));
    BIND_STRUCT_METHOD(st, edgePathCoords, MARGS("vStart", "vEnd", "out"));
    BIND_STRUCT_METHOD(st, generateUVFromSeams, MARGS("marginMilli"));
    BIND_STRUCT_METHOD(st, markAllSeams, MARGS());
    BIND_STRUCT_METHOD(st, fillVertexColorFromPosition, MARGS());
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    return st;
  }

  /* AttrGroup for a TS-side AttrDomain bitflag (VERTEX=1, EDGE=2, CORNER=4,
   * FACE=16; LIST=8 has no AttrGroup here). Mirrors the LiteMesh AttrDomain
   * enum so TS can address any domain's layers by the same int. */
  AttrGroup *attrGroupForDomainFlag(int domain)
  {
    switch (domain) {
    case 1:  return &v.attrs;
    case 2:  return &e.attrs;
    case 4:  return &c.attrs;
    case 16: return &f.attrs;
    }
    return nullptr;
  }

  /* Set the AttrUse (category) of the layer at `index` in `domain`'s group.
   * The category write primitive for the TS attribute manager — `AttrRef.use`
   * is read-only through the native binding proxy, and a layer can only be
   * addressed by index (names don't marshal). `use` is an AttrUse bitflag int
   * (NONE=0, COLOR=2, UV=4, POLYGROUP=8). Out-of-range index is a no-op. */
  void setAttrUse(int domain, int index, int use)
  {
    AttrGroup *grp = attrGroupForDomainFlag(domain);
    if (!grp || index < 0 || index >= int(grp->attrs.size())) {
      return;
    }
    grp->attrs[index].use = AttrUse(use);
  }

  /* Element count for a TS AttrDomain flag (used to value-init a new layer). */
  int elemCountForDomainFlag(int domain)
  {
    switch (domain) {
    case 1:  return v.count;
    case 2:  return e.count;
    case 4:  return c.count;
    case 16: return f.count;
    }
    return 0;
  }

  /* Pick a unique layer name within `grp`: `base` if free, else the first free
   * `base.NNN`. Shared by addAttr and the UV-gen naming — names can't cross the
   * TS binding, so C++ owns them. */
  static util::string uniqueAttrName(AttrGroup *grp, const char *base)
  {
    auto taken = [&](const string &nm) {
      for (AttrRef &a : grp->attrs) {
        if (a.name == nm) return true;
      }
      return false;
    };
    if (!taken(string(base))) {
      return string(base);
    }
    char buf[64];
    for (int i = 1;; i++) {
      snprintf(buf, sizeof(buf), "%s.%03d", base, i);
      if (!taken(string(buf))) {
        return string(buf);
      }
    }
  }

  /* Add a new attribute layer to `domain` with category `use` (AttrUse int) and
   * a unique auto-generated name (base from the category — color/uv/group, else
   * "attr" — with a `.NNN` suffix when taken). Names can't be passed across the
   * binding, so C++ owns naming; the index is returned and TS reads the name
   * back through the AttrRef proxy. Value-inits the layer for determinism.
   * Returns the new layer's index in its group, or -1 on bad domain. */
  int addAttr(int domain, int type, int use)
  {
    AttrGroup *grp = attrGroupForDomainFlag(domain);
    if (!grp) {
      return -1;
    }
    AttrType ty = AttrType(type);
    AttrUse u = AttrUse(use);

    const char *base = "attr";
    if (u & AttrUse::COLOR) base = "color";
    else if (u & AttrUse::UV) base = "uv";
    else if (u & AttrUse::POLYGROUP) base = "group";

    string name = uniqueAttrName(grp, base);

    AttrRef &ref = grp->ensure(ty, name, /*materialize=*/true);
    ref.use = u;

    int n = elemCountForDomainFlag(domain);
    detail::type_dispatch(ty, [&]<typename T>() {
      if constexpr (!std::is_same_v<T, bool>) {
        auto *dd = static_cast<AttrData<T> *>(ref.data);
        for (int i = 0; i < n; i++) dd->set_default(i);
      }
    });

    for (int i = 0; i < int(grp->attrs.size()); i++) {
      if (grp->attrs[i].name == name) return i;
    }
    return int(grp->attrs.size()) - 1;
  }

  /* Remove the attribute layer at `index` in `domain`. Refuses builtins
   * (`.`-prefixed internal layers and the geometry layers positions/normals/
   * select) so the UI can't delete load-bearing data. No-op on bad index. */
  void removeAttr(int domain, int index)
  {
    AttrGroup *grp = attrGroupForDomainFlag(domain);
    if (!grp || index < 0 || index >= int(grp->attrs.size())) {
      return;
    }
    const string &nm = grp->attrs[index].name;
    if (nm.size() > 0 && (nm[0] == '.' || nm == string("positions") ||
                          nm == string("normals") || nm == string("select"))) {
      return;
    }
    grp->remove_attr(index);
  }

  /* Wave 5: mark the shortest edge-path from vStart to vEnd as a seam. Runs
   * shortestEdgePath (Dijkstra over live edges), sets the EDGE_SEAM flag on each
   * edge along the path, and recomputes derived boundary state. Returns the
   * number of edges marked, or -1 when no path exists / the verts are invalid.
   * `state` nonzero sets the seam flag, 0 clears it (the marking tool's undo
   * re-runs the same path with state=0). Defined in mesh.cc (needs mesh_path.h
   * + boundary.h). */
  int markSeamPath(int vStart, int vEnd, int state);

  /* Wave 5 (undo support): fill `out` with the edge indices along the shortest
   * vStart→vEnd path (the edges markSeamPath would flag), so the marking ToolOp
   * can snapshot their prior EDGE_SEAM bits and restore them exactly on undo —
   * rather than blanket-clearing the path (which would also unset seams that
   * pre-existed on overlapping edges). Marshal-safe Vector<int> out-param. */
  void edgePathEdges(int vStart, int vEnd, util::Vector<int> &out);

  /* Read/write a single edge's EDGE_SEAM bit (0/1). setEdgeSeam marks the edge
   * boundary-dirty but does NOT recompute — batch several, then call
   * recomputeBoundary once. Used by the marking ToolOp's undo to restore a
   * snapshot. */
  int edgeSeam(int e);
  void setEdgeSeam(int e, int state);
  void recomputeBoundary();

  /* Boundary polyline-graph stats (integration-test seam): out =
   * [flaggedEdges, graphVerts, non2ValenceVerts, components] over the union of
   * all boundary edge flags. Thaws topo + recomputes derived state first. */
  void boundaryGraphStats(util::Vector<int> &out);

  /* Wave 5: fill `out` with the shortest edge-path vertex positions as flat xyz
   * triples ([vStart..vEnd], 3 floats each), so the marking tool can draw the
   * candidate/marked seam without per-vertex cross-backend reads. `out` is a
   * bound Vector<float> out-param (marshal-safe, like castScreenCircle). */
  void edgePathCoords(int vStart, int vEnd, util::Vector<float> &out);

  /* Wave 7: generate a per-corner UV map from EDGE_SEAM-bounded charts (the
   * boundary-conditions unwrapper). Owns naming C++-side (a unique "uv[.NNN]"
   * corner layer, like addAttr) since names don't marshal; the created FLOAT2
   * layer is tagged AttrUse::UV by the unwrapper. `marginMilli` is the [0,1] pack
   * margin in thousandths (int so it marshals). Thaws frozen topology (the
   * unwrapper walks live links). Returns the chart count. Defined in mesh.cc
   * (needs uvgen.h). */
  int generateUVFromSeams(int marginMilli);

  /* Test/demo helpers (deterministic, backend-identical). markAllSeams flags
   * every edge EDGE_SEAM so generateUVFromSeams yields a per-face (cuboid) UV
   * map; fillVertexColorFromPosition writes a position->rgb gradient into the
   * first vertex FLOAT4 COLOR layer. Defined in mesh.cc. */
  void markAllSeams();
  void fillVertexColorFromPosition();

  /* Detach the layer at `index` into the stash WITHOUT freeing its data, and
   * return a stash id (reattachAttr undoes it). Unlike removeAttr this preserves
   * the AttrData so a remove can be undone with its contents intact — the
   * undoable-remove path uses this instead of mesh serialization (which is the
   * heavyweight, and currently broken-on-custom-layers, alternative). Refuses
   * builtins and bool layers (the latter live in packed storage). -1 on error. */
  int detachAttr(int domain, int index)
  {
    AttrGroup *grp = attrGroupForDomainFlag(domain);
    if (!grp || index < 0 || index >= int(grp->attrs.size())) {
      return -1;
    }
    const string &nm = grp->attrs[index].name;
    if (nm.size() > 0 && (nm[0] == '.' || nm == string("positions") ||
                          nm == string("normals") || nm == string("select"))) {
      return -1;
    }
    if (grp->attrs[index].type == AttrType::BOOL) {
      return -1;
    }
    StashedAttr st;
    st.ref = grp->attrs[index]; // copies the AttrRef, including its data pointer
    st.domain = domain;
    st.count = elemCountForDomainFlag(domain); // snapshot for the reattach guard
    // Drop the slot without freeing data — the stash now owns the AttrData.
    grp->attrs.remove_at(index, /*swap_end_only=*/false);
    attrStash.append(st);
    return int(attrStash.size()) - 1;
  }

  /* Move a stashed layer back into its element group (undo of detachAttr).
   * Returns the new index, or -1 if the id is invalid / already reattached. */
  int reattachAttr(int stashId)
  {
    if (stashId < 0 || stashId >= int(attrStash.size())) {
      return -1;
    }
    StashedAttr &st = attrStash[stashId];
    if (!st.ref.data) {
      return -1; // already reattached
    }
    AttrGroup *grp = attrGroupForDomainFlag(st.domain);
    if (!grp) {
      return -1;
    }
    // Guard the no-topology-edits-between invariant: the stashed layer was sized
    // for `st.count` elements; reattaching it to a domain that has since grown/
    // shrunk would alias out-of-range elements. Refuse rather than corrupt.
    if (elemCountForDomainFlag(st.domain) != st.count) {
      printf("reattachAttr: domain element count changed while detached "
             "(%d -> %d); refusing to reattach a stale-sized layer\n",
             st.count, elemCountForDomainFlag(st.domain));
      return -1;
    }
    grp->attrs.append(st.ref);
    st.ref.data = nullptr; // the group owns the AttrData again
    return int(grp->attrs.size()) - 1;
  }

  /* Poly-group id of a face (the "group" int attr the polygroup brush writes).
   * 0 = unassigned / attr absent / out of range. Int-only signature so it
   * marshals across both the WASM and native binding backends (string params
   * don't marshal). Used by the polygroup brush's shift-to-extend sampling. */
  int faceGroup(int face)
  {
    if (face < 0 || face >= f.count) {
      return 0;
    }
    if (!f.attrs.has(AttrType::INT, "group")) {
      return 0;
    }
    AttrData<int> *data = f.attrs.find_attribute(AttrType::INT, "group").get_data<int>();
    return data ? (*data)[face] : 0;
  }

  /* Largest poly-group id assigned to any face (0 if none). The paint layer
   * allocates a fresh group as maxFaceGroup()+1, so each new stroke gets an
   * incrementing id without storing a counter (survives reload). */
  int maxFaceGroup()
  {
    if (!f.attrs.has(AttrType::INT, "group")) {
      return 0;
    }
    AttrData<int> *data = f.attrs.find_attribute(AttrType::INT, "group").get_data<int>();
    if (!data) {
      return 0;
    }
    int mx = 0;
    for (int i = 0; i < f.count; i++) {
      int g = (*data)[i];
      if (g > mx) {
        mx = g;
      }
    }
    return mx;
  }

  void calcAABB(math::float3 &min, math::float3 &max)
  {
    min = math::float3(FLT_MAX);
    max = math::float3(FLT_MIN);
    for (int i = 0; i < v.count; i++) {
      min.min(v.co[i]);
      max.max(v.co[i]);
    }
  }

  int make_vertex(math::float3 co, MeshCallbacks *cb = nullptr);
  int make_edge(int v1, int v2, MeshCallbacks *cb = nullptr);
  int make_face(std::span<int> verts, std::span<int> edges, MeshCallbacks *cb = nullptr);
  int make_face(std::span<int> verts, MeshCallbacks *cb = nullptr);

  void kill_vertex(int v, MeshCallbacks *cb = nullptr);
  void kill_edge(int e, MeshCallbacks *cb = nullptr);
  void kill_face(int f, MeshCallbacks *cb = nullptr);

  EdgeOfVertIter e_of_v(int v1)
  {
    int e1 = v.e[v1];
    return EdgeOfVertIter(this, v1, e1);
  }

  inline int edge_side(int e1, int v1)
  {
    return v1 == e.vs[e1][0] ? 0 : 1;
  }

  int find_edge(int v1, int v2)
  {
    for (int e1 : e_of_v(v1)) {
      for (int e2 : e_of_v(v2)) {
        if (e1 == e2) {
          return e1;
        }
      }
    }

    return ELEM_NONE;
  }

  /* Reorder one element domain in place. Each map is map[old] = new and must
   * be a full bijection over that domain's storage capacity (free slots
   * included — SpatialTree::computeLocalityMaps produces such maps). Every
   * reference field pointing into the reordered domain is remapped, then the
   * domain's own storage is permuted; the domains are independent so the five
   * may be applied in any order. */
  void reorder_verts(util::span<int> vertex_map);
  void reorder_edges(util::span<int> edge_map);
  void reorder_corners(util::span<int> corner_map);
  void reorder_lists(util::span<int> list_map);
  void reorder_faces(util::span<int> face_map);

  void recalc_normals();
private:
  void radial_insert(int e1, int c1)
  {
    if (e.c[e1] == ELEM_NONE) {
      e.c[e1] = c1;
      c.radial_next[c1] = c.radial_prev[c1] = c1;
    } else {
      int c2 = e.c[e1];
      int c2prev = c.radial_prev[c2];

      c.radial_prev[c1] = c2prev;
      c.radial_next[c1] = c2;

      c.radial_next[c2prev] = c1;
      c.radial_prev[c2] = c1;
    }
  }

  void radial_remove(int e1, int c1)
  {
    if (e.c[e1] == c1) {
      e.c[e1] = c.radial_next[c1];
    }
    if (e.c[e1] == c1) {
      e.c[e1] = ELEM_NONE;
    }

    int next = c.radial_next[c1];
    int prev = c.radial_prev[c1];

    c.radial_next[prev] = next;
    c.radial_prev[next] = prev;
  }

  void disk_insert(int e1, int v1)
  {
    int side1 = edge_side(e1, v1);

    if (v.e[v1] == ELEM_NONE) {
      v.e[v1] = e1;

      e.disk[e1][side1 * 2] = e1;
      e.disk[e1][side1 * 2 + 1] = e1;

      return;
    }

    int e2 = v.e[v1];
    int side2 = edge_side(e2, v1);

    int prev = e.disk[e2][side2 * 2];
    int side3 = edge_side(prev, v1);

    e.disk[e2][side2 * 2] = e1; /* e2.prev */

    e.disk[e1][side1 * 2] = prev;   /* e1.prev */
    e.disk[e1][side1 * 2 + 1] = e2; /* e1.next */

    e.disk[prev][side3 * 2 + 1] = e1; /* prev.next */
  }

  void disk_remove(int e1, int v1)
  {
    int side1 = edge_side(e1, v1);

    int prev = e.disk[e1][side1 * 2];
    int next = e.disk[e1][side1 * 2 + 1];

    int sidep = edge_side(prev, v1);
    int siden = edge_side(next, v1);

    e.disk[prev][sidep * 2 + 1] = next; /* prev->next */
    e.disk[next][siden * 2] = prev;     /* next->prev */

    if (e1 == v.e[v1]) {
      v.e[v1] = next;
    }

    if (e1 == v.e[v1]) {
      v.e[v1] = ELEM_NONE;
    }
  }
};
}; // namespace sculptcore::mesh
