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
#include "sculpt_layers.h"

#include <functional>
#include <string>
#include <vector>

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
  int ngonFaceCount() const
  {
    return int(n_ngon_faces);
  }

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
    BIND_STRUCT_METHOD(st, sculptLayerAdd, MARGS());
    BIND_STRUCT_METHOD(st, sculptLayerCount, MARGS());
    BIND_STRUCT_METHOD(st, sculptLayerAttrIndex, MARGS("li"));
    BIND_STRUCT_METHOD(st, sculptLayerWeight, MARGS("li"));
    BIND_STRUCT_METHOD(st, sculptLayerEnabled, MARGS("li"));
    BIND_STRUCT_METHOD(st, sculptLayerFrozen, MARGS("li"));
    BIND_STRUCT_METHOD(st, sculptLayerEditTarget, MARGS());
    BIND_STRUCT_METHOD(st, sculptLayerFlattenAll, MARGS());
    BIND_STRUCT_METHOD(st, sculptLayerPruneSettingsOnly, MARGS());
    BIND_STRUCT_METHOD(st, isTopoLocked, MARGS());
    BIND_STRUCT_METHOD(st, removeAttr, MARGS("domain", "index"));
    BIND_STRUCT_METHOD(st, detachAttr, MARGS("domain", "index"));
    BIND_STRUCT_METHOD(st, reattachAttr, MARGS("stashId"));
    BIND_STRUCT_METHOD(st, markSeamPath, MARGS("vStart", "vEnd", "state"));
    BIND_STRUCT_METHOD(st, edgePathEdges, MARGS("vStart", "vEnd", "out"));
    BIND_STRUCT_METHOD(st, edgeSeam, MARGS("e"));
    BIND_STRUCT_METHOD(st, setEdgeSeam, MARGS("e", "state"));
    BIND_STRUCT_METHOD(st, markEdgePath, MARGS("vStart", "vEnd", "kind", "state"));
    BIND_STRUCT_METHOD(st, edgeFlagKind, MARGS("e", "kind"));
    BIND_STRUCT_METHOD(st, setEdgeFlagKind, MARGS("e", "kind", "state"));
    BIND_STRUCT_METHOD(st, markSharpByAngle, MARGS("angle", "state"));
    BIND_STRUCT_METHOD(st, repairLogCount, MARGS());
    BIND_STRUCT_METHOD(st, clearRepairLog, MARGS());
    BIND_STRUCT_METHOD(st, repairMesh, MARGS());
    BIND_STRUCT_METHOD(st, featureVerts, MARGS("kind", "outIdx", "outCo"));
    BIND_STRUCT_METHOD(st, recomputeBoundary, MARGS());
    BIND_STRUCT_METHOD(st, boundaryGraphStats, MARGS("out"));
    BIND_STRUCT_METHOD(st, edgePathCoords, MARGS("vStart", "vEnd", "out"));
    BIND_STRUCT_METHOD(st, generateUVFromSeams, MARGS("marginMilli"));
    BIND_STRUCT_METHOD(st, markAllSeams, MARGS());
    BIND_STRUCT_METHOD(st, fillVertexColorFromPosition, MARGS());
    BIND_STRUCT_METHOD(st, vertexColor, MARGS("vert", "out"));
    BIND_STRUCT_METHOD(st, dumpVertCo, MARGS("out"));
    BIND_STRUCT_METHOD(st, dumpFrameNormals, MARGS("out"));
    BIND_STRUCT_METHOD(st, dumpFrameTangents, MARGS("out"));
    BIND_STRUCT_METHOD(st, setVertCo, MARGS("idx", "x", "y", "z"));
    BIND_STRUCT_METHOD(st, symmetrize, MARGS("axis", "sign", "threshold"));
    BIND_STRUCT_METHOD(st, selectedCount, MARGS("domain"));
    BIND_STRUCT_METHOD(st, elemSelected, MARGS("domain", "idx"));
    BIND_STRUCT_METHOD(st, selectedElems, MARGS("domain", "out"));
    BIND_STRUCT_METHOD(st, gatherVertCos, MARGS("idx", "out"));
    BIND_STRUCT_METHOD(st, selectionBoundaryEdges, MARGS("out"));
    BIND_STRUCT_METHOD(st, movableVerts, MARGS("out"));
    BIND_STRUCT_METHOD(st, edgeRing, MARGS("e", "out"));
    BIND_STRUCT_METHOD(st, faceLoop, MARGS("e", "out"));
    BIND_STRUCT_METHOD(st, edgeLoop, MARGS("e", "out"));
    BIND_STRUCT_METHOD(st, faceEdgeNearest, MARGS("f", "p"));
    BIND_STRUCT_METHOD(st, faceEdgeList, MARGS("f", "outEdges", "outCoords"));
    BIND_STRUCT_METHOD(st, faceVertList, MARGS("f", "outVerts", "outCoords"));
    BIND_STRUCT_METHOD(st, loopCutPreviewCoords, MARGS("seedEdge", "out"));
    BIND_STRUCT_METHOD(st, calcAABB, MARGS("minOut", "maxOut"));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    return st;
  }

  /* AttrGroup for a TS-side AttrDomain bitflag (VERTEX=1, EDGE=2, CORNER=4,
   * FACE=16; LIST=8 has no AttrGroup here). Mirrors the LiteMesh AttrDomain
   * enum so TS can address any domain's layers by the same int. */
  AttrGroup *attrGroupForDomainFlag(int domain)
  {
    switch (domain) {
    case 1:
      return &v.attrs;
    case 2:
      return &e.attrs;
    case 4:
      return &c.attrs;
    case 16:
      return &f.attrs;
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
    case 1:
      return v.count;
    case 2:
      return e.count;
    case 4:
      return c.count;
    case 16:
      return f.count;
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
        if (a.name == nm)
          return true;
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
    if (u & AttrUse::COLOR)
      base = "color";
    else if (u & AttrUse::UV)
      base = "uv";
    else if (u & AttrUse::POLYGROUP)
      base = "group";

    string name = uniqueAttrName(grp, base);

    AttrRef &ref = grp->ensure(ty, name, /*materialize=*/true);
    ref.use = u;

    int n = elemCountForDomainFlag(domain);
    detail::type_dispatch(ty, [&]<typename T>() {
      if constexpr (!std::is_same_v<T, bool>) {
        auto *dd = static_cast<AttrData<T> *>(ref.data);
        for (int i = 0; i < n; i++)
          dd->set_default(i);
      }
    });

    for (int i = 0; i < int(grp->attrs.size()); i++) {
      if (grp->attrs[i].name == name)
        return i;
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
                          nm == string("normals") || nm == string("select")))
    {
      return;
    }
    grp->remove_attr(index);
  }

  /* Sculpt-layer settings sidecar, index-parallel with nothing — ordered by
   * stack position (composition order). Keyed to VERTEX FLOAT3 attrs by name;
   * the evaluator lives in source/displace/ (mesh can't depend on it). */
  util::Vector<SculptLayerSettings> sculptLayers;

  /* Edit-target layer (V2 implicit-active model): settings index of the layer
   * sculpting records into, -1 = none. Runtime state (not serialized); the
   * target's column is assumed stale — fold before reading it. */
  int activeEditLayer = -1;

  /* Runtime topology lock (NOT serialized): set on multires level meshes by
   * subdiv::Multires::materialize. On a locked base the VDM clamp is a true
   * ceiling — promotion is gated off (sculpt-layers-design §8, plan X1); the
   * app additionally gates dyntopo. */
  bool topoLocked = false;

  int isTopoLocked() const
  {
    return topoLocked ? 1 : 0;
  }

  /* Settings-record index for the layer attr named `name`, or -1. */
  int findSculptLayer(const string &name) const
  {
    for (int i = 0; i < int(sculptLayers.size()); i++) {
      if (sculptLayers[i].name == name) {
        return i;
      }
    }
    return -1;
  }

  /* Create a sculpt layer: a VERTEX FLOAT3 attr (unique name from `base`),
   * tagged AttrUse::SCULPT_LAYER and zero-initialized, plus an appended
   * settings record. Returns the settings index (== stack position). C++
   * entry point — names don't marshal; TS goes through sculptLayerAdd(). */
  int addSculptLayerNamed(const char *base)
  {
    string name = uniqueAttrName(&v.attrs, base);
    AttrRef &ref = v.attrs.ensure(AttrType::FLOAT3, name, /*materialize=*/true);
    ref.use = ref.use | AttrUse::SCULPT_LAYER;

    auto *dd = static_cast<AttrData<math::float3> *>(ref.data);
    int cap = int(v.capacity());
    for (int i = 0; i < cap; i++) {
      dd->set_default(i);
    }

    SculptLayerSettings st;
    st.name = name;
    sculptLayers.append(std::move(st));
    return int(sculptLayers.size()) - 1;
  }

  /* Bound (marshal-safe) sculpt-layer surface. Mutations that must keep
   * evaluated positions current (weight/enable changes, removal) are NOT
   * bound here — they go through the displace compositor's API. */
  int sculptLayerAdd()
  {
    return addSculptLayerNamed("slayer");
  }
  int sculptLayerCount() const
  {
    return int(sculptLayers.size());
  }
  /* Index of layer `li`'s attribute in v.attrs (for BrushAttrLayerOverride
   * redirection), or -1. */
  int sculptLayerAttrIndex(int li)
  {
    if (li < 0 || li >= int(sculptLayers.size())) {
      return -1;
    }
    for (int i = 0; i < int(v.attrs.attrs.size()); i++) {
      if (v.attrs.attrs[i].type == AttrType::FLOAT3 &&
          v.attrs.attrs[i].name == sculptLayers[li].name)
      {
        return i;
      }
    }
    return -1;
  }

  /* Marshal-safe per-layer reads for the layer-stack UI. Mutations go through
   * the displace C-API (Mesh_layerSet*), which keeps evaluated v.co current. */
  float sculptLayerWeight(int li) const
  {
    return li >= 0 && li < int(sculptLayers.size()) ? sculptLayers[li].weight : 0.0f;
  }
  int sculptLayerEnabled(int li) const
  {
    return li >= 0 && li < int(sculptLayers.size()) && sculptLayers[li].enabled ? 1 : 0;
  }
  int sculptLayerFrozen(int li) const
  {
    return li >= 0 && li < int(sculptLayers.size()) && sculptLayers[li].frozen ? 1 : 0;
  }
  int sculptLayerEditTarget() const
  {
    return activeEditLayer;
  }

  /** Fold the edit target's delta column from evaluated positions:
   * d(v) = co(v) − rest(v) over @p verts (empty span = every live vert).
   * Idempotent and semantically a no-op (co and stack evaluation unchanged);
   * no-op when no edit target / no rest snapshot exists. Target switching
   * lives in displace::setActiveEditLayer — this core is mesh-side so
   * serialization can fold without a mesh→displace dependency. */
  void foldActiveSculptLayer(std::span<const int> verts = {});

  /** Bake the evaluated surface and discard the stack: co is already the
   * composite, so drop every settings row, layer column, and the rest
   * snapshot without adjusting positions. Clears the edit target. Used when
   * enabling multires on a mesh with vertex-column layers (V2 flatten). */
  void sculptLayerFlattenAll();

  /** Drop settings rows that have no matching VERTEX FLOAT3 column — the
   * channel-backed rows left on a cage after its multires stack (which owned
   * the channels) is deleted. Clears the edit target if its row goes. */
  void sculptLayerPruneSettingsOnly();

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

  /* Generalized edge-feature marking (kind: 0 = EDGE_SEAM, 1 = EDGE_SHARP), so
   * one interactive marking tool serves both seams and sharp edges. markEdgePath
   * is markSeamPath with a selectable flag; the seam variants above delegate here
   * with kind=0. setEdgeFlagKind marks dirty without recomputing (batch, then
   * recomputeBoundary). Defined in mesh.cc. */
  int markEdgePath(int vStart, int vEnd, int kind, int state);
  int edgeFlagKind(int e, int kind);
  void setEdgeFlagKind(int e, int kind, int state);

  /** Set EDGE_SHARP = `state` on every manifold edge whose dihedral angle (the
   * angle between its two face normals) exceeds `angle` radians. Additive — edges
   * at or below the threshold are left untouched. Returns the number of edges
   * changed. Defined in mesh.cc. */
  int markSharpByAngle(float angle, int state);

  /* Fill outIdx with the indices of every vertex incident to an edge carrying
   * the `kind` flag (0 seam / 1 sharp) and outCo with their xyz positions (3
   * floats each, index-aligned), so the marking tool can project them to screen
   * and snap the path endpoint onto an existing feature vertex. Marshal-safe
   * Vector out-params (like castScreenCircle). Defined in mesh.cc. */
  void featureVerts(int kind, util::Vector<int> &outIdx, util::Vector<float> &outCo);

  /* Boundary polyline-graph stats (integration-test seam): out =
   * [flaggedEdges, graphVerts, non2ValenceVerts, components] over the union of
   * all boundary edge flags. Thaws topo + recomputes derived state first. */
  void boundaryGraphStats(util::Vector<int> &out);

  /* Wave 5: fill `out` with the shortest edge-path vertex positions as flat xyz
   * triples ([vStart..vEnd], 3 floats each), so the marking tool can draw the
   * candidate/marked seam without per-vertex cross-backend reads. `out` is a
   * bound Vector<float> out-param (marshal-safe, like castScreenCircle). */
  void edgePathCoords(int vStart, int vEnd, util::Vector<float> &out);

  /* Dump every live vert's (idx,x,y,z) as flat float quadruples into `out` (a
   * marshal-safe bound Vector<float> out-param). The TS symmetrize op reads
   * positions index-aligned through this; pair with setVertCo to write back. */
  void dumpVertCo(util::Vector<float> &out);

  /* Dense xyz dump of the F3 frame attrs (.frames.v.normal / .tangent) in
   * live-vert iteration order (id order on dense meshes); empty when absent.
   * Fixed-name methods because strings can't cross the generic method
   * binding — the X3 frame-field export for the tessellated tier. */
  void dumpFrameNormals(util::Vector<float> &out);
  void dumpFrameTangents(util::Vector<float> &out);

  /* Set the position of vert `idx` (a live vert index, as emitted by dumpVertCo).
   * Per-vertex scalar setter — the only marshal-safe vertex-write seam (a bound
   * Vector can't be filled from TS). Out-of-range index is a no-op. */
  void setVertCo(int idx, float x, float y, float z);

  /* Destructive symmetrize across the `axis` (0=x,1=y,2=z) plane: bisect, keep
   * the `sign` half (+1 positive, -1 negative), mirror it, weld the seam so the
   * result is watertight. `threshold` snaps near-plane verts onto the plane.
   * Backed by symmetrizeMesh (utils/symmetrize.h); defined in mesh.cc. */
  void symmetrize(int axis, int sign, float threshold);

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

  /* Append the RGBA of vertex `vert` from the first vertex FLOAT4 COLOR layer
   * into `out` (4 floats). Falls back to opaque white when there is no color
   * layer or the index is out of range. Backs the color brush's ctrl-click
   * eyedropper; out-param keeps it marshal-safe like dumpVertCo. */
  void vertexColor(int vert, util::Vector<float> &out);

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
                          nm == string("normals") || nm == string("select")))
    {
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
             st.count,
             elemCountForDomainFlag(st.domain));
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

  void calcAABB(math::float3 *min, math::float3 *max)
  {
    *min = math::float3(FLT_MAX);
    *max = math::float3(-FLT_MAX);
    for (int i = 0; i < v.count; i++) {
      min->min(v.co[i]);
      max->max(v.co[i]);
    }
  }

  /* @p hint is an existing element of the same domain near which the new
   * element should be allocated in DRAM (its page is preferred), so dyntopo
   * keeps new geometry spatially local. ELEM_NONE = no preference. */
  int make_vertex(math::float3 co, MeshCallbacks *cb = nullptr, int hint = ELEM_NONE);
  int make_edge(int v1, int v2, MeshCallbacks *cb = nullptr, int hint = ELEM_NONE);
  int make_face(std::span<int> verts,
                std::span<int> edges,
                MeshCallbacks *cb = nullptr,
                int hint = ELEM_NONE);
  int make_face(std::span<int> verts, MeshCallbacks *cb = nullptr, int hint = ELEM_NONE);

  void kill_vertex(int v, MeshCallbacks *cb = nullptr);
  void kill_edge(int e, MeshCallbacks *cb = nullptr);
  void kill_face(int f, MeshCallbacks *cb = nullptr);

  /** Validate the mesh data structure (edge vert refs, vertex disk cycles, edge
   * radial cycles, face corner loops) and repair what it can: kill unrepairable
   * faces/edges, then rebuild every disk cycle from the (authoritative) edge
   * endpoints and every radial cycle + corner edge from the face corners. Logs
   * each problem to stderr, appends it to `repairLog`, and invokes `log` if set.
   * Returns the number of problems found. Defined in mesh.cc. */
  int validateAndRepair(const std::function<void(const char *)> &log = {});

  /** Per-error repair-log lines from validateAndRepair (also echoed to stderr).
   * Not part of the mesh's serialized state; the app reads the count as a
   * "repair happened" signal (e.g. LiteMesh load). */
  std::vector<std::string> repairLog;
  int repairLogCount() const
  {
    return int(repairLog.size());
  }
  void clearRepairLog()
  {
    repairLog.clear();
  }
  /** App-facing no-arg entry to validateAndRepair (logs to stderr + repairLog).
   * Cheap on a healthy mesh — returns 0 without rebuilding. Called on load so a
   * corrupt file is fixed before the spatial tree / any op sees it (#37). */
  int repairMesh()
  {
    return validateAndRepair();
  }

  /* Re-point an existing edge's endpoints from its current {v0,v1} to
   * {nv0,nv1} in place (id preserved), maintaining both verts' disk cycles
   * and firing callbacks with the same discipline as make_edge/kill_edge
   * (onEdgeChange for the edge and its disk neighbours BEFORE each splice;
   * onVertChange for all affected verts). The edge's radial/corner cycle is
   * left untouched — the caller owns the loop/corner rewiring. Used by the
   * in-place flipEdge / splitEdge Euler ops. */
  void relink_edge_verts(int e1, int nv0, int nv1, MeshCallbacks *cb = nullptr);

  /* Tear down a face's list + corners (full radial/edge callback discipline,
   * exactly like kill_face) but KEEP the face id `f1` — caller must immediately
   * reinit_face it. Fires onFaceChange(f1) BEFORE any mutation (so the meshlog
   * snapshots the pre-rewrite face row and the spatial tree re-flags the owning
   * leaf) rather than onFaceKill. Leaves f.l[f1] = ELEM_NONE. The id-preserving
   * counterpart of kill_face for the in-place splitEdge Euler op. */
  void clear_face_contents(int f1, MeshCallbacks *cb = nullptr);

  /* Repopulate a face id `f1` (whose contents were cleared by
   * clear_face_contents) with a fresh loop. Mirrors make_face's list/corner/
   * radial wiring + create callbacks, but reuses f1 and fires onFaceChange(f1)
   * AFTER the rewire (the list + corners are genuinely new → create events; the
   * face merely changed) so the spatial tree's touch_face reads the new verts. */
  void reinit_face(int f1,
                   std::span<int> verts,
                   std::span<int> edges,
                   MeshCallbacks *cb = nullptr);

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

  /** Selection state of one element (domain 0=vert,1=edge,2=face) — the
   * shift-click toggle read. 0/1; 0 for an invalid index. */
  int elemSelected(int domain, int idx)
  {
    switch (domain) {
      case 0:
        return idx >= 0 && idx < int(v.capacity()) && !v.freemap[idx] && v.select[idx] ? 1
                                                                                       : 0;
      case 1:
        return idx >= 0 && idx < int(e.capacity()) && !e.freemap[idx] && e.select[idx] ? 1
                                                                                       : 0;
      case 2:
        return idx >= 0 && idx < int(f.capacity()) && !f.freemap[idx] && f.select[idx] ? 1
                                                                                       : 0;
    }
    return 0;
  }

  /* Count selected elements in a box-modeling domain (0=vert,1=edge,2=face) —
   * drives the "auto" select mode (all-if-empty-else-none). */
  int selectedCount(int domain)
  {
    int count = 0;
    switch (domain) {
      case 0:
        for (int i : v) {
          if (v.select[i]) {
            count++;
          }
        }
        break;
      case 1:
        for (int i : e) {
          if (e.select[i]) {
            count++;
          }
        }
        break;
      case 2:
        for (int i : f) {
          if (f.select[i]) {
            count++;
          }
        }
        break;
    }
    return count;
  }

  /* Gather the co (x,y,z) of each vertex in `idx` into `out` (appended, flat).
   * The box-modeling transform bridge's targeted read of just the movable verts
   * (vs dumping every vertex via dumpVertCo). Invalid/dead indices emit (0,0,0). */
  void gatherVertCos(util::Vector<int> &idx, util::Vector<float> &out)
  {
    for (int vi : idx) {
      if (vi < 0 || size_t(vi) >= v.capacity() || v.freemap[vi]) {
        out.append(0.0f);
        out.append(0.0f);
        out.append(0.0f);
        continue;
      }
      math::float3 co = v.co[vi];
      out.append(co[0]);
      out.append(co[1]);
      out.append(co[2]);
    }
  }

  /* Gather selected element indices for a domain into `out` (appended). Used by
   * the transform bridge (movable-vert set) and tools that act on the selection. */
  void selectedElems(int domain, util::Vector<int> &out)
  {
    switch (domain) {
      case 0:
        for (int i : v) {
          if (v.select[i]) {
            out.append(i);
          }
        }
        break;
      case 1:
        for (int i : e) {
          if (e.select[i]) {
            out.append(i);
          }
        }
        break;
      case 2:
        for (int i : f) {
          if (f.select[i]) {
            out.append(i);
          }
        }
        break;
    }
  }

  /* Box-modeling loop/boundary queries (defined in mesh.cc via
   * utils/modeling_walk.h). selectionBoundaryEdges = boundary of the selected
   * face region; movableVerts = all verts touched by any selected element (the
   * transform bridge's movable set); edgeRing/faceLoop = the quad strip through
   * an edge (loop-cut backends). */
  void selectionBoundaryEdges(util::Vector<int> &out);
  void movableVerts(util::Vector<int> &out);
  void edgeRing(int e, util::Vector<int> &out);
  void faceLoop(int e, util::Vector<int> &out);
  /** Edge loop (end-to-end chain, Blender alt-click) through e. */
  void edgeLoop(int e, util::Vector<int> &out);
  /** Edge of face f nearest point p (cursor-hit -> edge pick). */
  int faceEdgeNearest(int f, const math::float3 &p);
  /** Edges of face f (all loops) with endpoint coords, 6 floats per edge —
   * lets the app pick the nearest edge in SCREEN space (a 3D nearest test
   * mis-picks on foreshortened surfaces). */
  void faceEdgeList(int f, util::Vector<int> &outEdges, util::Vector<float> &outCoords);
  /** Verts of face f (all loops) with coords, 3 floats per vert — the vert
   * sibling of faceEdgeList for screen-space vertex picking. */
  void faceVertList(int f, util::Vector<int> &outVerts, util::Vector<float> &outCoords);
  /** Loop-cut preview: per face-loop quad, the segment between its two ring-edge
   * midpoints (flat xyz pairs) — where the cut verts will land. */
  void loopCutPreviewCoords(int seedEdge, util::Vector<float> &out);

  /* Reorder one element domain in place. Each map is map[old] = new and must
   * be a full bijection over that domain's storage capacity (free slots
   * included — SpatialTree::computeLocalityMaps produces such maps). Every
   * reference field pointing into the reordered domain is remapped, then the
   * domain's own storage is permuted; the domains are independent so the five
   * may be applied in any order. */
  /* The per-domain moved (live) slot lists of a partial/scoped reorder (the
   * closed-permutation set from SpatialTree::computeLocalityMapsPartial, which
   * uses interior-only selection). When `active`, reorder_* fix only references
   * into the moved sets + permute only those attribute slots — O(region) instead
   * of O(mesh). The interior-only selection guarantees every reference into a
   * moved element is itself in a moved set, so this is complete. */
  struct ReorderMoved {
    bool active = false;
    util::span<int> v, e, c, l, f;
  };
  void reorder_verts(util::span<int> vertex_map, const ReorderMoved &moved);
  void reorder_edges(util::span<int> edge_map, const ReorderMoved &moved);
  void reorder_corners(util::span<int> corner_map, const ReorderMoved &moved);
  /* @p corner_map: scoped mode only — corners are already permuted when this runs,
   * so the moved corner's c.l attribute lives at corner_map[c1]. */
  void reorder_lists(util::span<int> list_map, const ReorderMoved &moved,
                     util::span<int> corner_map = {});
  /* @p list_map: scoped mode only — lists are already permuted, so the moved
   * list's l.f attribute lives at list_map[l1]. */
  void reorder_faces(util::span<int> face_map, const ReorderMoved &moved,
                     util::span<int> list_map = {});

  /* Reclaim DRAM by dropping trailing all-free attribute pages from every domain
   * (effective after the live set has been compacted to the front). Returns the
   * total pages freed across all domains. */
  int freeTrailingStorage();

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
    int self = diskPack(e1, side1);

    if (v.e[v1] == ELEM_NONE) {
      v.e[v1] = e1;

      e.disk[e1][side1 * 2] = self;
      e.disk[e1][side1 * 2 + 1] = self;

      return;
    }

    int e2 = v.e[v1];
    int side2 = edge_side(e2, v1);

    /* Tail's encoded link replaces the old edge_side(prev, v1) vs load. */
    int prevLink = e.disk[e2][side2 * 2];
    int prev = diskEdge(prevLink), side3 = diskSide(prevLink);

    e.disk[e2][side2 * 2] = self; /* e2.prev */

    e.disk[e1][side1 * 2] = prevLink;                /* e1.prev */
    e.disk[e1][side1 * 2 + 1] = diskPack(e2, side2); /* e1.next */

    e.disk[prev][side3 * 2 + 1] = self; /* prev.next */
  }

  void disk_remove(int e1, int v1)
  {
    int side1 = edge_side(e1, v1);

    int prevLink = e.disk[e1][side1 * 2];
    int nextLink = e.disk[e1][side1 * 2 + 1];

    /* Embedded sides replace the old edge_side(prev/next, v1) vs loads. */
    int prev = diskEdge(prevLink), sidep = diskSide(prevLink);
    int next = diskEdge(nextLink), siden = diskSide(nextLink);

    e.disk[prev][sidep * 2 + 1] = nextLink; /* prev->next */
    e.disk[next][siden * 2] = prevLink;     /* next->prev */

    if (e1 == v.e[v1]) {
      v.e[v1] = next;
    }

    if (e1 == v.e[v1]) {
      v.e[v1] = ELEM_NONE;
    }
  }
};
}; // namespace sculptcore::mesh
