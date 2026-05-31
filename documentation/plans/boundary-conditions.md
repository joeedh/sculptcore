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
