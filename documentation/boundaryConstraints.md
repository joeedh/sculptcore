# Boundary Constraints — Current State

This is a status report on the boundary-constraint system in sculptcore: the
data model that records which mesh edges are *features* (seams, sharp creases,
projected curves, poly-group / UV-chart borders), how that is summarized
per-vertex, and how the sculpt smoothing brushes and the dynamic-topology
remesher consume it to keep those features intact.

It complements the original design sketch in
[`boundary.md`](boundary.md) (the "what we want" list) — this document is the
"what is actually built" snapshot.

---

## 1. The data model

The source of truth lives in `source/mesh/boundary.{h,cc}`
(`sculptcore::mesh::boundary`). A mesh carries several **structured overlays**
expressed as ordinary mesh attributes, plus a derived per-vertex summary.

### Edge flags

| Layer name | Kind | Domain | Notes |
|---|---|---|---|
| `.boundary.edge.projected` | source (persistent bool) | edge | "curve" edges that stay on the surface |
| `.boundary.edge.sharp` | source (persistent bool) | edge | crease edges; smoothing leaves a discontinuity |
| `.boundary.edge.seam` | source (persistent bool) | edge | UV seams / user seams |
| `.boundary.edge.polygroup` | derived (TEMP bool) | edge | inter-`group` face borders |
| `.boundary.edge.uvchart` | derived (TEMP bool) | edge | UV-discontinuity borders |
| `group` | source (INT) | face | poly-group id painted by the polygroup brush |

- **Source flags** (`projected` / `sharp` / `seam`) are user-set and persist
  with the mesh.
- **Derived flags** (`polygroup` / `uvchart`) are recomputed from the face
  `group` attribute and the UV corner layers; they are `TEMP` (never saved).

### Per-vertex classification

`.boundary.vert.class` (TEMP INT, `VERT_CLASS`) is a **bitmask** summarizing
which boundary types touch a vertex, plus one derived structural bit. The bits
are `boundary::BoundaryClass` (`boundary.h`):

```cpp
enum BoundaryClass : int {
  BC_NONE      = 0,
  BC_PROJECTED = 1 << 0,
  BC_SHARP     = 1 << 1,
  BC_SEAM      = 1 << 2,
  BC_POLYGROUP = 1 << 3,
  BC_UVCHART   = 1 << 4,
  BC_TYPE_MASK = 0x1F,    // BC_PROJECTED..BC_UVCHART

  BC_ENDPOINT  = 1 << 5,  // derived: exactly one dominant-type constraint edge
};
```

- Bits 0–4 (`BC_TYPE_MASK`) are the **type union** — which boundary types are
  present on the vertex's incident edges.
- Bit 5 (`BC_ENDPOINT`) is **derived structural state**: the vertex is the
  *dangling end* of a feature chain (exactly one incident edge of its dominant
  type). Smooth brushes drop the tangential slide for such a vertex (averaging
  toward its lone like-neighbor would collapse the end) and move it only along
  the normal, so the chain end follows the surface.

The **dominant type** for the endpoint count obeys the override rule below:
`SHARP` wins over the smooth types. `BC_ENDPOINT` is kept *out* of
`BC_TYPE_MASK` so the neighbor-share test (`dom & nb.class`) compares only type
bits.

### Dirty tracking

Recompute is **lazy**. Changing any source flag (or a poly id / UV) marks the
affected edges/verts boundary-dirty and sets `MeshBase::boundaryDirty`
(`mesh_types.h`) — an O(1) "is the derived overlay stale?" flag. The next
`recomputeDirty` refreshes only the dirty elements and clears the markers.

- `.boundary.edge.dirty`, `.boundary.vert.dirty` (TEMP bool) — the per-element
  dirty markers.
- `setEdgeFlag`, `markEdgeDirty`, `markVertDirty`, `markFaceDirty`,
  `markAllDirty` are the mutators; each dirties what it touches.

---

## 2. The recompute pipeline

`boundary::recomputeDirty(mesh)` (`boundary.cc`) runs two passes over the dirty
set. It walks disk/radial connectivity, so it **requires live topology** (not
the frozen-topo brush fast path).

**Pass 1 — derived edge flags.** For each dirty edge, recompute
`.boundary.edge.polygroup` (`computePolygroupBoundary` — adjacent faces with
different `group` ids) and, when the mesh has UVs, `.boundary.edge.uvchart`
(`computeUvChartBoundary` — UV discontinuity across the edge). Dirtying an edge
re-dirties its endpoints so their class refreshes.

**Pass 2 — per-vertex class.** For each dirty vertex, union the boundary bits of
its incident edges into `cls`, and **count dominant-type constraint edges** to
flag endpoints:

```cpp
int sharpCount = 0, smoothCount = 0;
for (int e : EdgeOfVertIter(...)) {
  int eb = /* bits carried by edge e */;
  cls |= eb;
  if (eb & BC_SHARP) sharpCount++;
  if (eb & (BC_PROJECTED | BC_SEAM | BC_POLYGROUP | BC_UVCHART)) smoothCount++;
}
int domCount = sharpCount > 0 ? sharpCount : smoothCount;  // sharp overrides
if (cls != BC_NONE && domCount == 1) cls |= BC_ENDPOINT;
```

A smooth edge carrying several bits counts once; `SHARP` overrides the smooth
types when picking the dominant type for both the count and the smoothing rule.

---

## 3. Marking the source flags

- **Interactive seam / sharp marking.** `Mesh::markEdgePath(vStart, vEnd, kind,
  state)` (`mesh.cc` / `mesh.h`) is the kind-parameterized engine path
  (`kind` 0 = `EDGE_SEAM`, 1 = `EDGE_SHARP`) behind the knife-style marking tools
  (`litemesh.mark_seam_interactive` / `mark_sharp_interactive`, hotkeys `K` /
  `Shift+K`). It sets the source flag along a shortest-path chain and dirties the
  affected elements. See [feature-marking.md](../../documentation/feature-marking.md).
- **By dihedral angle.** A bound method sets `EDGE_SHARP` on every manifold edge
  whose crease angle exceeds a threshold (`mesh.h`).
- **Poly groups.** The polygroup brush paints the face `group` attribute;
  `markFaceDirty` re-dirties a painted face's edges so the derived
  `polygroup` border refreshes.
- **Projected** edges have the data-model support (`EDGE_PROJECTED`, the
  collapse rules, the smoothing path) but **no dedicated interactive marking
  tool yet** — they are settable through `setEdgeFlag` / the data API.

---

## 4. How smoothing consumes it

Two compiled `sbrush` kernels are boundary-aware:
`source/brush/kernels/bsmooth.sbrush` (boundary-aware Laplacian smooth) and
`featurealign.sbrush` (topology-rake smooth). Both read `.boundary.vert.class`
and share the same boundary logic; they differ only in how the tangential
neighbor weights are computed (featurealign adds a cross-field rake bias).

### Type gate, sharp override, and endpoint

Each kernel splits a neighbor's displacement into a **normal part** (along the
vertex normal) and a **tangential part**, then weights them by `wNor` / `wTan`
set from the class bitmask:

```
dom  = vc & 0x1F                 // BC_TYPE_MASK: the boundary-type bits (never
                                 //   narrowed to one) that gate the tangential avg
wNor = (vc & BC_SHARP) ? 0 : 1   // sharp drops the normal component...
wTan = 1
if (vc & BC_ENDPOINT) { wTan = 0; wNor = 1; }   // ...endpoint drops the tangent
```

- **Interior** (`vc == 0`, gate off): plain Laplacian; the normal component is
  damped by `(1 - projection)` (volume-preserving tangential slide). `projection
  == 0` is bit-identical plain Laplacian.
- **Sharp crease** (`vc & BC_SHARP` → `wNor = 0`): average only like-(sharp)
  neighbors tangentially and **drop the normal component entirely** — the vertex
  slides purely in the tangent plane along the crease. The crease survives as a
  clean discontinuity because the surface never averages across it (like-neighbor
  gate) and never bulges (no normal motion). Sharp wins: a vertex carrying both a
  sharp and a smooth type is treated as sharp.
- **Smooth boundary** (`projected` / `seam` / `polygroup` / `uvchart`,
  non-sharp; `wNor = 1`): average only like-type neighbors tangentially but
  **keep the interior `(1 - projection)` normal motion** (from all neighbors), so
  the curve relaxes while still following surface curvature.
- **Endpoint** (`BC_ENDPOINT` → `wTan = 0`): **drop the tangential slide** — a
  chain end has a single like-neighbor and averaging toward it would collapse the
  end — and move only along the normal toward all neighbors, so the end follows
  the surface height without sliding off.

### The neighbor-share gate

A boundary vertex only averages neighbors that share one of its boundary types:

```
if (dom != 0 && (dom & nb.vclass) == 0) wTan2 = 0.0;   // mask-before-AND
```

`dom` is the full `BC_TYPE_MASK` union (never narrowed to a single bit), so a
vertex on several feature types averages neighbors sharing *any* of them.
`BC_ENDPOINT` (bit 5) is kept out of `BC_TYPE_MASK`, so it never enters this AND
— the endpoint case is handled by `wTan`, not the gate. `continue` cannot skip a
single neighbor on the WGSL backend, so neighbors are gated by weight, never by
early exit.

### bsmooth vs. featurealign

The two kernels share the class logic above verbatim (`wNor` / `wTan` / `dom`,
sharp / smooth / endpoint, the same `(1 - projection)` normal damping). They
differ only in the **tangential neighbor weight**:

- **bsmooth** weights every kept tangential neighbor equally (plain Laplacian).
- **featurealign** multiplies each kept tangential neighbor by a cross-field
  alignment factor (`1 + rake·8·align⁴`, `rake` = `brush.rake`), so grid-aligned
  edges pull harder and the topology drifts to follow the field. `rake == 0`
  reduces it to bsmooth. The rake applies wherever `wTan2 != 0`, including a
  boundary vertex's like-neighbors; only the normal component is untouched by it.

### Refresh entry points

`recomputeDirty` is called (only when `boundaryDirty`) before/within strokes by
the brush executor (`brush_executor.h`):
- **Stroke start** for bsmooth/featurealign (`refreshBoundaryClassForBSmooth`),
  while links are live and before any topo freeze.
- **Per dab** after dyntopo, when `preserve_features` is set and the overlay
  went dirty.
- **Poly-group paint** marks painted faces dirty (`markPolygroupDirty`) so the
  derived border refreshes on the next recompute.
- **After deserialization** — `serial::readMesh` (`mesh_serialize.cc`) calls
  `markAllDirty` at the end of a load. The derived flags + `VERT_CLASS` are
  `TEMP` (not serialized) and `boundaryDirty` defaults false, so without this a
  loaded mesh would carry the source flags (group/seam/sharp) but no recomputed
  classification — the overlay (`buildSeamBatch`, gated on `boundaryDirty`) and
  the smooth brush would see empty boundaries until the next paint.

---

## 5. GPU / backend parity

The kernels compile to every backend (C++, WGSL, SPIR-V, …) from one `sbrush`
source. The class attribute is uploaded as a plain int buffer in the WGSL path
(`source/debug/gpu_stroke.cc`, binding 14) — zeros when no boundaries exist, so
the binding is always full-size. The generated WGSL/C++ apply identical logic.
Cross-backend correctness is gated by `sbrush-validate` (per-backend compile)
and `sbrush-verify` (C++ vs GPU A/B); those gates need `tint` + `spirv-val` on
`PATH` (the Vulkan SDK ships `spirv-val`/`spirv-opt`/`spirv-dis`/`glslang`;
`tint` is a Dawn standalone build). The boundary-aware kernels validate clean on
the **WGSL** and **SPIR-V** backends. The **OpenCL** backend cannot emit them —
its emitter reads only `co`/`no`/`v` off the neighbor bundle, not a neighbor
attribute like `nb.vclass` (`emit_opencl.cc`), a long-standing gap unrelated to
boundaries; OpenCL is not on the sculpt runtime path (browser = WGSL, native).

---

## 6. Dynamic-topology feature preservation

The dyntopo remesher (`source/dyntopo/dyntopo.h`) keeps features intact during
split/collapse via `FeatureViews` and `featureCollapseOk`:

- `FeatureViews` caches the live edge-flag bool views and exposes `edgeMask(e)`
  (the `BoundaryClass` bits on an edge), `isFeatureEdge`, and `isFeatureVert`.
  Feature-vertex membership is read from the **live edge overlay**, not the
  per-vertex class attribute — split/collapse propagate edge flags immediately,
  but the class attr is only rebuilt by the caller's `recomputeDirty` after the
  dab, so reading the stale class could let a non-feature collapse pinch a
  freshly-split crease.
- **Collapse rule** (`featureCollapseOk`): a feature edge may collapse only
  *along its own collinear curve* — both endpoints must be simple interior
  points of one uniform feature curve (exactly two incident feature edges, all
  matching the edge's exact type signature, no junction/mixed type). An optional
  `corner_angle` adds a geometric gate: an endpoint whose curve bends past the
  threshold is treated as a corner and pinned. This lets a feature line coarsen
  without tearing or eroding corners.
- **Split** propagates source flags to the new geometry and marks it dirty so
  the post-dab `recomputeDirty` refreshes the derived flags + vertex class.
- Gated by `DynTopoParams::preserve_features` (default on); the pre-remesh stage
  has its own `pre_remesh_preserve_features`.

This realizes the `boundary.md` rules: collapse a projected/feature edge into a
like edge ✓, not into an unlike edge ✓, subdivide a feature edge ✓.

---

## 7. Other consumers

- **Pre-remesh isotropic smooth** (`source/remesh/preremesh.cc`) pins any vertex
  with a non-zero class (`vclass[v] != 0`) so feature verts don't slide off
  their curve during the isotropic pass.
- **Cross-field seeding** (`source/brush/feature_field.cc`) seeds the
  featurealign cross field from `EDGE_SHARP` / `EDGE_SEAM` / face `group`
  borders.
- **UV unwrap** (`source/mesh/uvgen.cc`) reads `EDGE_SEAM` as the cut set.
- **Remesh-invariance gate** (`boundary::graphStats`) reports
  `[flaggedEdges, graphVerts, non2ValenceVerts, components]` over the union of
  all boundary edge flags; the non-2-valence-vertex and component counts are
  invariant under feature-preserving remeshing, so they detect constraint-network
  damage (used by the parity tests).

---

## 8. Status summary

**Implemented and tested**
- Source flags (projected / sharp / seam), derived polygroup + UV-chart borders,
  lazy dirty recompute with `boundaryDirty` short-circuit.
- Per-vertex class bitmask with the `BC_ENDPOINT` derived bit and the
  sharp-overrides-smooth dominant-type rule.
- Boundary-aware smoothing in both kernels: sharp crease (like-neighbor
  tangential slide, normal component dropped), projected/seam (like-neighbor
  tangent + `(1 - projection)` normal), endpoint (normal-only), interior
  Laplacian.
- Dyntopo collinear-collapse + corner-angle pinning; split flag propagation.
- Regression: `tests/test_boundary.cc` (classification + endpoint bits + shortest
  edge path); the TS `sculptcore_boundary` parity test (polyline-graph invariance
  under dyntopo + both undo stacks).

**Deferred / follow-ups**
- **Corner (≥3-valence junction) smoothing.** Junctions currently smooth as
  chain vertices (pulled to equilibrium by their constraint edges); a dedicated
  corner case (pin, or angle-based detection) is deferred.
- **Mixed-feature override semantics.** Where a sharp crease terminates at a
  smooth border, `SHARP` wins and the vertex counts as a sharp endpoint → it
  relaxes. Revisit if such junctions should stay constrained by the surviving
  smooth border.
- **UV-area-preserving smoothing.** The UV-chart border is *derived* and gates
  neighbors, but the slide-reprojection / area solver from `boundary.md` (so UV
  charts deform correctly under smoothing) is not built.
- **Projected-edge interactive marking tool** (data-model support exists).

---

## 9. Source map

| File | Role |
|---|---|
| `source/mesh/boundary.{h,cc}` | data model, `BoundaryClass`, `recomputeDirty`, `graphStats` |
| `source/mesh/mesh.{h,cc}` | `markEdgePath`, by-angle sharp marking |
| `source/mesh/mesh_types.h` | `MeshBase::boundaryDirty` |
| `source/brush/kernels/bsmooth.sbrush` | boundary-aware Laplacian smooth |
| `source/brush/kernels/featurealign.sbrush` | topology-rake smooth |
| `source/brush/brush_executor.h` | refresh entry points |
| `source/brush/feature_field.cc` | cross-field seeding from features |
| `source/dyntopo/dyntopo.h` | `FeatureViews`, `featureCollapseOk`, split propagation |
| `source/remesh/preremesh.cc` | feature-vert pinning in isotropic smooth |
| `source/mesh/uvgen.cc` | seam-driven UV cut |
| `source/debug/gpu_stroke.cc` | class-attr upload to WGSL |
| `tests/test_boundary.cc` | classification + endpoint regression |
