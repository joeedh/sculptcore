# Boundary conditions & paint brushes — implementation plan

Lays the last groundwork before a dynamic-topology brush: paint brushes,
per-type boundary flags with a lazy derived classification, boundary-aware
smoothing with UV-preserving slide, and seam-based UV generation. Source
spec: [`boundary.md`](../boundary.md).

The throughline is the dyntopo endgame: the face-attribute machinery, the
per-attribute interpolation flags, the lazy boundary-dirty classification, and
localized-raycast reprojection are exactly the primitives a dyntopo brush needs
to preserve UVs / poly groups / boundaries while it retopologizes under the dab.

## Waves

| # | Wave | Depends on |
|---|------|-----------|
| 1 | DSL: typed attribute access + face stage (all backends + A/B) | — |
| 2 | Attribute visualization infrastructure (overlays + `Spatial_UpdateAttrs`) | — |
| 2b | Attribute management UI (ObData panel: list / add / remove / categorize + per-category active attr) | 2, 3 |
| 3 | Paint brushes: Color (per-vertex) + Poly group (per-face) | 1, 2 |
| 4 | Boundary flag model + lazy dirty classification | — |
| 5 | Seam/boundary marking tool (shortest path) | 2, 4 |
| 6 | Boundary-aware smooth brush + slide UV reprojection | 1, 4 |
| 7 | UV generation from seams | 5 |

Each wave ends in a verifiable checkpoint and a stop for user feedback. Waves 1
and 2 are independent and may be parallelized.

## Locked decisions (Q&A with user)

- **Paint goes through the sbrush DSL itself** (typed attribute handles), not a
  C++ side-path — keeps the all-backend/GPU-ready philosophy.
- **All-backend + A/B parity is the bar per feature** (C++/WGSL/SPIR-V/CUDA/HIP/
  OpenCL via `sbrush-validate` + `sbrush-verify`).
- **Per-type boundary flags** (`projected`/`sharp`/`seam` source-of-truth edge
  bools) + **derived, lazily-recomputed** per-vertex/per-edge classification
  driven by `boundary-dirty` bool attrs. Poly-group & UV-chart boundaries are
  derived.
- **Per-vertex color now, corner-ready.** Poly groups per-face. UVs per-corner.
- **Slide UV-preservation auto-engages when UV layers exist**, with a brush
  override.
- **Full attribute overlays** are in scope (built early in Wave 2 so later waves
  plug in).
- **Per-face painting → a real DSL face stage** (not executor-side).

## Status

- **Wave 1: DONE & verified (2026-05-30).** Vertex attribute path complete on
  CPU + WGSL/SPIR-V with cross-backend A/B (`sbrush-verify` ✓ `color`); face
  stage done on the C++ executor (`polygroup`), GPU emitters stubbed. CPU
  regression test: `tests/test_brush_attr.cc`. Remaining: real GPU face dispatch
  (deferred — "Wave 1b") and the pre-existing stale-golden refresh
  (`sbrush-verify --regen`, a separate housekeeping commit).

- **Wave 4: core DONE & verified (2026-05-30).** `source/mesh/boundary.{h,cc}`:
  per-type source edge flags + lazy dirty markers + derived poly-group boundary +
  per-vertex classification bitmask + `recomputeDirty`. Test: `tests/test_boundary.cc`.
  Deferred (fill when consumed): UV-chart derived flag, one-projected-edge count,
  C-API/TS accessors (Wave 5), meshlog undo of flags, dirty-list.

- **Wave 5: compute core DONE & verified (2026-05-30).**
  `source/mesh/mesh_path.{h,cc}` `shortestEdgePath` (Dijkstra over edges,
  weighted by 3D length, via `litestl::BinaryHeap`); tested in
  `tests/test_boundary.cc`. Remaining (app-side): the TS modal marking tool +
  overlay + C-API binding + writing `boundary` seam/projected/sharp flags along
  the path.

- **Wave 7: core DONE & verified (2026-05-30).** `source/mesh/uvgen.{h,cc}`
  `generateUVFromSeams`: flood-fill charts bounded by `EDGE_SEAM`, group-normal
  projection, shelf box-pack into [0,1], per-corner FLOAT2. Test:
  `tests/test_uvgen.cc`. Remaining: a TS ToolOp to invoke it + pick the source
  flag set + name the layer (app-side).

## Wave 1 design (approved)

Extends the sbrush DSL (`source/brush/compiler/`, `kernels/ir/`) so kernels can
read/write typed mesh attributes and iterate faces.

### Declarations
New `FieldKind::Attr` alongside `uniform`/`ctx` (`ir.h`):

```sbrush
attr vertex float4 color;     // per-vertex, read+write on iterated vertex
attr face   int    group;     // per-face,  read+write in a face stage
attr edge   bool   sharp;     // per-edge (read-only via nb.edge — Wave 6)
attr corner float2 uv;        // reserved (per-vertex color now, corner later)
```

Grammar: `attr <domain> <type> <name> [= "layerName"] ;`,
domain ∈ `{vertex, face, edge, corner}`. Identifier = the kernel's **handle**;
optional string fixes a mesh-layer name.

### Access model — element members (approved)
Custom attrs extend the same member namespace as builtins (`v.co/v.no/v.mask`):
`v.color` (iterated vertex, `inout`), `nb.color` (neighbor, read-only),
`f.group` (face stage, `inout`). Type system adds `Int2/3/4`, `Byte`, `Short`
to `TypeKind`; kernels work in canonical types, narrow storage adapted by the
binding layer.

### Face stage
New `StageKind::Face` + `Face` special type exposing `f.center` (centroid),
`f.no`, and declared face attrs. Covered-face set from the existing cone query
(`collectConeFaces`) + face→node ownership (`.spatial.f.node`).

### Layer binding — runtime (approved)
Kernel declares a logical handle; `createCommand` reads a codegen-emitted **attr
manifest** (`{handle, domain, type, rw}`), resolves the bound layer name from an
`attrBindings` map on the `Brush` (default = handle name), `ensure`/`find`s the
`AttrRef`, and stashes a typed handle. CPU → `AttrData<T>*`/`BoolAttrView*`
(PtrHelper gains a `T&` per element, like `mask`); GPU → uploaded storage buffer.
Missing layer: create-if-write-target, else feature-off.

### GPU plan — per-face threads (approved)
Fixed slots 0–13 stay; custom attrs take slots ≥14 (Vulkan ≥32 guaranteed).
Dispatcher (`vulkan/vk_compute.cc`) creates one storage buffer per declared
attr, uploads from the `AttrRef` (expanding `BYTE`/`SHORT`/packed-`BOOL` →
`u32`/`f32`), binds, and reads back via a new generic
`readbackAttr(slot, count, type, out)`. Face stage adds a parallel path:
`unique_faces` + face-node-chunks + precomputed face-centroid buffer + per-face
attr buffers, one workgroup per node over covered faces. WGSL emits
`@group(0) @binding(N)` and routes `v.color`/`f.group` to `attr_<handle>[idx]`
with write-back at kernel exit.

### Deferred to the wave that needs it
- `for_neighbor` neighbor/edge attr access (`nb.color`, `nb.edge.sharp`) — Wave 6
  (CPU knows the edge; GPU needs the `nbr_verts` CSR widened with edge index).
- `corner`/`edge` as first-class iterated stages — only declaration + reserved.

### Wave 1 done = green
`attrtest.sbrush` (vertex writes a `float4` attr; face stage writes an `int`
attr) compiles on all backends (`sbrush-validate`) and A/B bit-matches C++ vs
GPU via the new generic readback (`sbrush-verify`).

## Wave 2b design — attribute management UI (ObData panel)

Builds on the ObData properties tab (Wave 2, `LiteMesh.buildPropertiesTab` +
`api_define_litemesh`). Gives the user a pathux **ListBox** (the DataList-bound
widget) to inspect, categorize, activate, add, and remove mesh attributes — and
generalizes the brushes' currently-hardcoded layer names (`color`, `group`) to a
per-category **active attribute**.

### Concepts
- **Category** — an attribute's *role*: `COLOR`, `UV`, `POLYGROUP`, `MASK`, or
  `NONE`. Stored as attr metadata in sculptcore (a small `category` enum on the
  attr layer descriptor, serialized with the mesh) so the brushes and the UI
  agree and it survives save/load. Allowed categories are constrained by
  (type, domain) — see the table.
- **Active attribute per category** — per-mesh, the layer name currently used
  for each role (the COLOR brush paints the active COLOR attr, slide reprojects
  the active UV attr, …). Stored on the mesh; clicking a categorized attr in the
  list sets that category's active attr to it. Feeds the brushes through Wave 1's
  `Brush.attrBindings`: the bridge sets `attrBindings[handle] =
  mesh.activeAttr(category)` per stroke, replacing the hardcoded `color`/`group`
  in `sculptcore_bindings`/`sculptcore_ops`.

### Valid categories per type / domain
| Category | Domain | Type | Consumed by |
|----------|--------|------|-------------|
| COLOR | vertex (corner later) | FLOAT4 / BYTE4 | color brush |
| UV | corner (vertex for now) | FLOAT2 | slide reprojection (W6), UV gen (W7) |
| POLYGROUP | face | INT | polygroup brush |
| MASK | vertex | FLOAT | (builtin-adjacent) |
| NONE | any | any | — (unset) |

`validCategories(type, domain)` returns the allowed set + `NONE`; the dropdown
shows exactly that.

### Data-API + UI
- **Attribute list** — a data-API `list` on the litemesh struct
  (`api_define_litemesh`), mirroring `api_define_mesh`'s element lists: each
  element is an "attribute" struct exposing `name` (string, read-only), `domain`
  (enum), `type` (enum), `category` (enum, writable). Backed by a C-API
  enumeration (extends `getAttrs`/`getAttrName` in `mesh_c_api.cc`) returning a
  stable per-mesh snapshot of (name, domain, type, category) over all five
  domains.
- **ListBox** — pathux `ListBox` (DataList binding) bound to that list via
  `datapath`; the row shows name + domain + category. Selecting a row that has a
  category sets the per-category active attr (the list's `setActive`, routed by
  the row's category).
- **Builtin filter** — a `Show builtin attributes` bool, default **off**.
  Builtins = names starting with `.` (e.g. `.spatial.*`) or the geometry
  builtins (`co`/`no`/`mask`). The list iterator skips them unless the toggle is
  on; the brushable user attrs (`color`, a UV layer, `group`) show by default.
- **Category dropdown** — an enum prop for the selected attr; options =
  `validCategories(attr.type, attr.domain)` + `None`. Setting it writes the
  attr's category (and activates it for that category); `None` clears it.
- **Add / Remove** — buttons (Add via a small type + domain picker). Add
  `ensure`s a new layer with a **unique** name (base from the type/category, a
  numeric `.NNN` suffix when taken — `uniqueAttrName` helper); Remove deletes the
  selected layer (disabled for builtins). Both run through ToolOps so they undo
  and mark the mesh dirty (+ a display re-fill when the added/removed attr is the
  one being shown).

### Cross-backend / sculptcore touchpoints
- Attr `category` enum on the layer descriptor + (de)serialization; the
  active-per-category map on the mesh. Exposed to TS by **index** (string params
  don't marshal as bound-method args — enumerate by index, fetch names through
  the string-returning C-API like `getAttrName`); `category` get/set +
  `activeAttr(category)` get/set as int-keyed bound methods or C-API.
- Add/remove/ensure a layer from TS: a C-API creating/removing by (domain, type,
  name) and returning the new index; mirrors `getAttr`.
- The paint bridge reads `activeAttr(category)` → `Brush.attrBindings` before
  each stroke (color → active COLOR, polygroup → active POLYGROUP, slide → active
  UV), so a stroke always targets the user-selected layer.

### Wave 2b done = green
In the ObData panel: the ListBox lists user attributes with domain + category
(builtins hidden by default, revealable); Add yields a uniquely-named layer; the
category dropdown offers only type-valid roles + None; clicking a COLOR attr
makes the color brush paint *that* layer (likewise UV / poly-group); Remove
deletes it. Undoable; verified live in the Electron app.
