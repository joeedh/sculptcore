# `source/mesh` — Overview

The `mesh` module implements sculptcore's mesh data structure: a topology
representation built around five element domains, paged attribute storage,
and a small reflection / proxy / iterator layer on top. It also exposes a
C ABI for the WASM/JS side and a GPU draw-batch builder.

The design is index-based (no pointer chasing between elements) and is the
core data structure that brushes, the spatial tree, and the GPU layer all
operate against.

## Element model

Five element domains, declared in `mesh_enums.h`:

| Domain | Meaning |
|--------|---------|
| `VERTEX` | A position in space. |
| `EDGE`   | A pair of vertex indices, plus a disk cycle. |
| `CORNER` | A per-face occurrence of a vertex (Blender BMesh "loop"). |
| `LIST`   | An ordered ring of corners — a face boundary or hole. |
| `FACE`   | A face referencing one or more lists (outer + holes). |

Each domain is a struct (`VertexData`, `EdgeData`, `CornerData`, `ListData`,
`FaceData` in `mesh_types.h`) that derives from `ElemData`. `Mesh` itself
inherits from `MeshBase`, which simply aggregates the five
`ElemData` subclasses as members `v`, `e`, `c`, `l`, `f`.

Topology connectivity is stored as plain integer attributes (sentinel value
`ELEM_NONE = -1`):

- `v.e` — one edge incident to each vertex (entry into the disk).
- `e.vs` — the two endpoint vertex indices.
- `e.disk` — `int4` storing prev/next edge indices around each endpoint
  (the disk cycle, similar to BMesh).
- `e.c` — one corner on the radial cycle for that edge.
- `c.v`, `c.e`, `c.l` — vertex / edge / owning-list of each corner.
- `c.next`, `c.prev` — list cycle around a face boundary.
- `c.radial_next`, `c.radial_prev` — radial cycle around an edge.
- `l.c`, `l.f`, `l.next`, `l.size` — list head corner, owning face, next
  list (for faces with holes), and corner count.
- `f.l`, `f.list_count`, `f.no` — first list, list count, face normal.

`mesh.cc` implements the standard Euler-style operators built on these
cycles: `make_vertex`, `make_edge`, `make_face`, `kill_vertex`, `kill_edge`,
`kill_face`, plus `find_edge`, `recalc_normals`, and `reorder_verts`. The
private `disk_insert` / `disk_remove` and `radial_insert` / `radial_remove`
helpers maintain the disk and radial cycles.

## `ElemData` — domain storage and lifecycle

`elem_data.h` provides the per-domain bookkeeping that all element types
share:

- `attrs` — an `AttrGroup` holding all attributes for that domain.
- `count`, `capacity_` — live element count and total slot capacity.
- `freemap` (`BoolVector`) and `freelist` (`Vector<int>`) — slot recycling.
  When a slot is freed, the index is pushed onto the freelist and marked in
  the freemap; allocating a new element pops a free slot, growing the
  attribute storage by one page (`ATTR_PAGESIZE = 4096`) on demand.
- `on_swap` — a `CallbackList` invoked when two element slots are swapped,
  so external systems (e.g. `IDMap`) can fix up references.
- An `iterator` that walks live (non-freed) slots from `0..capacity_`.

This means *element indices are stable until that slot is explicitly freed*
— attribute data lives at a fixed index, with deleted slots becoming holes
that get reused.

## Attribute system

The attribute system (`attribute*.h`, `attribute.cc`) is the storage layer
under every per-element field. It supports both built-in topology fields and
arbitrary user attributes.

- **`AttrType`** (`attribute_enums.h`) — `FLOAT`, `FLOAT2/3/4`, `INT`,
  `INT2/3/4`, `BOOL`, `BYTE`, `SHORT`, and `WEIGHTS` (sparse deform weights;
  see below). It is a **bitmask**, so anything treating a set of types as one
  needs auditing when a value is added. **`AttrFlag`** marks attributes as
  `TOPO`, `TEMP`, `NOCOPY`, `NOINTERP`, `TOPO_KEEP_FROZEN`, or `DERIVED`.
- **`AttrMerge`** (`attribute_enums.h`, resolved in `attr_merge.cc`) — how a
  layer produces the value of an element a topological operator creates or
  merges from two sources (a split's midpoint, a collapse's survivor):
  `DEFAULT` (lerp float-backed, copy src0 otherwise), `COPY_SRC0`, `NONE`, or
  `CUSTOM` with a handler. Stamped by name (or, for `WEIGHTS`, by type) when
  the layer is created, so it costs nothing in the file format and is
  re-derived on load.
- **`AttrData<T>`** — paged storage. Data is held in `AttrPage`s of
  `ATTR_PAGESIZE = 4096` elements; pages are lazily *materialized* (a page
  with `exists = false` reports a single default `value` for every slot
  without allocating). `safe_get`, `materialize`, and `materialize_all`
  manage that lazy fill.
- **`PackedBoolAttrs` / `BoolAttrView`** (`attribute_bool.h`) — bools are
  stored bit-packed across all bool attributes for a domain to keep cache
  density high; each bool attribute is a `BoolAttrView` over the shared
  packed buffer at a `(block, bit)` offset.
- **`AttrRef`** — a `(name, type, flag, AttrDataBase*)` handle returned by
  the C API and used for type-erased iteration.
- **`AttrGroup`** — the per-domain owner. `ensure(type, name)` creates an
  attribute on demand; `find_attribute`, `swap`, `reorder`, `set_default`,
  and `ensure_capacity` round out the surface. A `type_dispatch` lambda
  helper switches an `AttrType` into a compile-time `T` so each operation
  can reach the typed `AttrData<T>`.
- **`BuiltinAttr<T, Name, Flag>`** (`attribute_builtin.h`) — a typed,
  named attribute member that auto-registers into its parent domain's
  `AttrGroup` (used for all the topology fields above and for things like
  `co`, `no`, `select`).

## Vertex-group weights (`AttrType::WEIGHTS`)

Blender's `MDeformVert` — a short, group-sorted run of `(group, weight)` pairs
per vertex — reimplemented as an attribute type, so sculpting, dyntopo and undo
carry deform weights the same way they carry any other layer. The engine owns
the weights for the length of a sculpt session; the addon bridges them in and
out (`sculptcore_addon/convert.py`).

**The column is not the storage.** A `WeightSlot` element is a plain 32-bit
index into a mesh-owned `DeformPool` side table (`deform_pool.h`). That keeps
the column bit-identical to an `INT` column, which is exactly what lets
`reorder`, `swap`, `resize`, the meshlog's raw byte copies and the serializer's
raw byte writes all keep working untouched. `WeightSlot` is nonetheless a
*distinct type* so generic attribute paths dispatch on it rather than silently
lerping or uploading a pool index as a number.

- **Runs are immutable and interned.** `DeformPool::intern` canonicalizes a run
  (group-ascending, zeros and duplicates dropped) before hashing, so equal
  weight sets across a region share one slot. Slot 0 is the empty run and is
  immortal, so a zero-filled column is already valid.
- **Slots are refcounted**, and the reference discipline lives in `WeightsRef`
  (`attr_weights.h`), *not* in the column: every store goes through
  `DeformPool::reassign` (retain new, release old), and every read copies the
  run out, because interning may reallocate a shard's arena. Write weights
  through `WeightsRef`, never through `AttrRef::get_data<WeightSlot>()`.
  `ensureVertWeights` / `findVertWeights` are the entry points —
  `AttrGroup::ensure` alone cannot create one, having no way to reach the pool.
- **The pool is sharded** (64 shards, one mutex each; a slot's shard follows
  from its index and never changes) because dyntopo is written for a parallel
  caller and the meshlog's `parallel_capture` already fills rows from several
  threads.
- **The pool outlives the mesh.** Lifetime is a *user* count via
  `DeformPoolUser`, not plain mesh ownership: meshlog rows hold slot indices,
  and a `Scene` frees its mesh before its log. The last user deletes.
- **Merging** (`mergeWeights`, `attr_merge.cc`) walks the union of two
  canonicalized runs, lerping by `ctx.t` and treating an absent group as 0 —
  not as "unchanged". Results are capped at `DEFORM_MAX_INFLUENCES` (32),
  keeping the largest-magnitude influences, and are deliberately **not
  normalized**: Blender does not, and silently renormalizing a rigged mesh
  would be a worse bug. The policy is keyed by *type*, not name, since a
  `WEIGHTS` column is refcounted whatever it is called.
- **Serialization** (format v6) writes the pool after the domains, slots in
  compacted order with the columns rewritten to dense ids, plus the
  `group_names` table. `migrate()` upgrades a v5 file by clearing any `WEIGHTS`
  column, whose indices would name a pool the file never stored.
- **Debugging**: `DeformPool::auditRefcounts` recomputes every refcount from a
  root set (`WeightsRef::collectRoots` over every column, mesh and meshlog) and
  reports disagreements. Nonzero means a write bypassed the funnel — the
  failure mode this design is most exposed to, and one that will not reproduce
  deterministically. `sweep()` reclaims zero-reference slots and compacts the
  arenas; it is a safepoint operation, so call it between dabs, not during one.

`group_names` mirrors Blender's `Mesh::vertex_group_names` — a
`DeformWeight::group` is an index into it, so that ordering is the whole
contract between host and engine. The c-api moves both in bulk as CSR
(`sc_mesh_weights_get`/`_set`, `sc_mesh_weight_groups_get`/`_set`) in
*live-vertex* order, matching `Mesh_toArrays`; a dyntopo mesh has freelist gaps,
so keying by engine index would write the wrong vertices.

## Iterators and proxies

Direct index-and-array access is the hot path, but the module also offers
two friendlier APIs:

- **`mesh_iter.h`** — minimal C++ range iterators:
  - `EdgeOfVertIter` walks the disk cycle of a vertex.
  - `CornerOfEdgeIter` walks the radial cycle of an edge.

- **`mesh_proxy.h`** — proxy objects (`VertProxy`, `EdgeProxy`,
  `CornerProxy`, `ListProxy`, `FaceProxy`) that wrap `(MeshBase*, int)` and
  expose connectivity as method calls (`v.edges()`, `e.v1()`, `c.next()`,
  `face.lists()`, `face.calc_center()`, etc.). Each proxy is templated on
  an `AssignMode` (`ASSIGN_PROXY` / `ASSIGN_SRC`) — assigning to an
  `ASSIGN_SRC` proxy writes back through a stored pointer to the originating
  topology slot, letting expressions like `v.e() = newEdge` mutate the mesh.

## Reflection bindings

Every public type in this module exposes a `defineBindings()` static that
constructs a `binding::types::Struct<T>` describing its layout. These
descriptors feed `litestl::binding`, which the TypeScript generator
consumes to emit the JS-side mesh API automatically. Per project
convention (see top-level `CLAUDE.md`), the runtime allocations inside
`defineBindings()` are intentional scaffolding for an eventual `consteval`
form — not a bug to fix.

## Mesh shapes

`mesh_shapes.{h,cc}` contains constructive helpers — currently just
`createCube(dimen, size, sphereFac)`, which builds a subdivided cube (with
optional spherical projection) for tests and demos.

## Utilities

`utils/triangulate.h` — header-only fan triangulation. `triangulateFace`
walks a face's first list and emits `Tri` records (three vertex indices,
three corner indices, owning face). `triangulate(range, tris)` is the
range-based front-end. Used by the GPU batch builder.

`utils/delaunay.h` — Delaunay-related topology helpers operating on the
mesh's disk/radial cycles.

`utils/edge_collapse.h` — edge-collapse topology operator. Covered by
`tests/test_edge_collapse.cc`, which exercises it under randomized
input with integrity checks.

## GPU draw batch

`gpu/mesh_drawbatch.{h,cc}` bridges to the `sculptcore::gpu` layer.
`MeshBatchManager::createMeshBatch(GPUManager*)` triangulates the whole
mesh, creates a `DrawBatch` with `position` and `normal` `Buffer`s sized
to `tris * 3`, fills them per-triangle (currently using flat per-tri
normals via `triNormal`), and emits a `DRAW_TRIS` `DrawCommand`. A
`TRI_DRAW_LINES` build flag swaps in a wireframe path.

## C API

`c-api/mesh_c_api.{h,cc}` is the external surface used by the WASM loader and by
the Blender addon's ctypes bridge. It exposes `extern "C"` shims:

- Lifecycle / reflection: `createMesh`, `freeMesh`, `makeVertex`, `getStrData`,
  `getAttr`, `getAttrs`, `getAttrName`, `copyAttrRef`, `freeAttrRef`
- Primitives: `Mesh_createCube`, `Mesh_makeGrid`, `Mesh_makeCylinder`,
  `Mesh_makeTorus`, `Mesh_makeUVSphere`, `Mesh_triangulate`
- Bulk array transfer: `Mesh_fromArrays`, `Mesh_arraySizes`, `Mesh_toArrays`,
  `Mesh_topoStamp`
- Attribute columns: `Mesh_readAttr` / `Mesh_writeAttr` plus the typed
  `Mesh_read*Attr` / `Mesh_write*Attr` shorthands
- Vertex-group weights: `sc_mesh_weights_element_count`, `sc_mesh_weights_get`,
  `sc_mesh_weights_set`, `sc_mesh_weight_group_count`,
  `sc_mesh_weight_groups_get`, `sc_mesh_weight_groups_set`
- Edges / boundary / UV: `Mesh_edgeCount`, `Mesh_writeEdgeFlagsByVerts`,
  `Mesh_readEdgeFlags`, `Mesh_recomputeBoundary`, `Mesh_generateUVFromSeams`
- Serialization: `serializeMesh`, `serializeMeshRaw`, `deserializeMesh`,
  `freeMeshBuffer`

These are deliberately additive — per project convention, changes here ripple
through Embind and the generated TS bindings, so prefer extending rather than
reshaping existing entry points.

**Symbol export is an explicit list.** `source/mesh/CMakeLists.txt` passes the
names above to `wasm_add_symbols`, which feeds both the WASM
`-sEXPORTED_FUNCTIONS` and the native shared library's export list. A new
`extern "C"` function that is not in that list compiles and links cleanly and is
simply *invisible* to `ctypes` / JS at runtime — an easy failure to mistake for
a load or ABI problem.

## File map

```
source/mesh/
  mesh.{h,cc}              Mesh struct: Euler ops, normals, reorder, find_edge
  mesh_base.h              Trivial constants (MESH_FACE_VS_LIMIT)
  mesh_types.{h,cc}        VertexData/EdgeData/CornerData/ListData/FaceData + MeshBase
  mesh_enums.h             ElemType, ELEM_NONE, ElemRef<>
  mesh_iter.h              EdgeOfVertIter, CornerOfEdgeIter
  mesh_proxy.h             VertProxy/EdgeProxy/CornerProxy/ListProxy/FaceProxy
  elem_data.h              ElemData base: freelist, paging, swap callbacks
  mesh_callbacks.h         MeshCallbacks: optional create/kill/change hooks
                           every topology op fires (dyntopo threads these to
                           keep the spatial tree and meshlog current)
  mesh_topo_cache.{h,cc}   VertNbrCSR: cached 1-ring vertex adjacency, in the
                           same order an EdgeOfVertIter disk walk produces
  mesh_serialize.{h,cc}    Versioned, lz4hc-compressed mesh blobs + migration
  attribute.{h,cc}         AttrData<T>, AttrRef, AttrGroup, type_dispatch
  attribute_base.h         AttrDataBase, type_to_attrtype<T>()
  attribute_builtin.h      BuiltinAttr<T, Name, Flag>
  attribute_bool.h         PackedBoolAttrs, BoolAttrView (bit-packed bool storage)
  attribute_enums.h        AttrType, AttrFlag, AttrUse, AttrMerge, WeightSlot
  attr_merge.{h,cc}        Per-layer merge policy + the CUSTOM handlers run at
                           an edge split / collapse
  attr_weights.{h,cc}      WeightsRef: the reference discipline over an
                           AttrType::WEIGHTS column
  deform_pool.{h,cc}       DeformPool: interned, refcounted, sharded weight runs
  sculpt_layers.h          Sculpt-layer settings sidecar (see source/displace/)
  boundary.{h,cc}          Boundary-condition data model: source-of-truth edge
                           flags (projected/sharp/seam), derived ones
                           (poly-group / UV-chart boundary), vertex bitmask
  mesh_path.{h,cc}         Dijkstra shortest path over edges, weighted by 3D
                           length (the seam/boundary marking tool's core)
  uvgen.{h,cc}             Unwrap: flood-fill charts bounded by seam edges,
                           project each onto its normal plane, shelf-pack
  uv_reproject.{h,cc}      Re-anchor corner UVs after a tangential smoothing
                           pass slid vertices, per UV wedge
  bindings.{h,cc}          registerBindings(BindingManager&) for the mesh module
  idmap.{h,cc}             Stable external IDs over slot indices
  mesh_shapes.{h,cc}       Procedural fixtures: cube, grid, cylinder, torus,
                           UV sphere
  ops/                     Box-modeling macro-ops: bevel, extrude, inset,
                           loopcut, split, subdivide
  utils/triangulate.h      Fan triangulator (header-only)
  utils/delaunay.h         Delaunay topology helpers (header-only)
  utils/edge_collapse.h    Edge-collapse operator (header-only)
  utils/edge_split.h       Edge-split operator (header-only)
  utils/edge_flip.h        Edge-flip operator (header-only)
  utils/attr_interp.h      Generic per-element attribute interpolation for the
                           operators above (lerps floats, copies src0 for
                           int/bool, never touches TOPO columns)
  utils/mesh_validate.h    Structured health report (census, manifoldness,
                           Euler) for the remesh tests + `remesh_validate`
  utils/select_derive.{h,cc} Derive one domain's selection from another
  utils/surface_walk.h     Greedy closest-point walk over face adjacency — the
                           cheap counterpart to a BVH query given a good seed
  utils/modeling_walk.h    Loop/boundary walk primitives the ops/ macros build on
  utils/symmetrize.h       Destructive symmetrize by topology surgery: bisect
                           at the plane, delete a half, mirror and share the seam
  utils/closest_point.h    BVH-accelerated closest-point-on-surface queries
  utils/obj_io.h           Minimal .obj read/write — fixtures/interchange for
                           the debug app and tests, not a serialization path
  c-api/mesh_c_api.{h,cc}  extern "C" surface for WASM/JS + the addon
  gpu/mesh_drawbatch.{h,cc} Mesh -> sculptcore::gpu::DrawBatch builder
```
