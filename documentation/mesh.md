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
  `INT2/3/4`, `BOOL`, `BYTE`, `SHORT`. **`AttrFlag`** marks attributes as
  `TOPO`, `TEMP`, `NOCOPY`, or `NOINTERP`.
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

## ID map

`idmap.{h,cc}` adds stable *external* IDs on top of the (otherwise reusable)
slot indices. `IDMap` registers `BuiltinAttr<int>` ID columns on the
selected domains (default: vertex / edge / face), maintains an
`id → element-index` map plus a freelist of recyclable IDs, and hooks the
`on_swap` callback so the mapping survives slot swaps. Users that need
persistent identity across edits / serialization use IDs; users that just
need fast iteration use raw indices.

## Mesh shapes

`mesh_shapes.{h,cc}` contains constructive helpers — currently just
`createCube(dimen, size, sphereFac)`, which builds a subdivided cube (with
optional spherical projection) for tests and demos.

## Utilities

`utils/triangulate.h` — header-only fan triangulation. `triangulateFace`
walks a face's first list and emits `Tri` records (three vertex indices,
three corner indices, owning face). `triangulate(range, tris)` is the
range-based front-end. Used by the GPU batch builder.

## GPU draw batch

`gpu/mesh_drawbatch.{h,cc}` bridges to the `sculptcore::gpu` layer.
`MeshBatchManager::createMeshBatch(GPUManager*)` triangulates the whole
mesh, creates a `DrawBatch` with `position` and `normal` `Buffer`s sized
to `tris * 3`, fills them per-triangle (currently using flat per-tri
normals via `triNormal`), and emits a `DRAW_TRIS` `DrawCommand`. A
`TRI_DRAW_LINES` build flag swaps in a wireframe path.

## C API

`c-api/mesh_c_api.{h,cc}` is the external surface used by the WASM
loader. It exposes `extern "C"` shims:

- `createMesh` / `freeMesh`
- `makeVertex`
- `getStrData`
- `getAttr`, `getAttrs`, `getAttrName`, `copyAttrRef`, `freeAttrRef`

These are deliberately additive — per project convention, changes here
ripple through Embind and the generated TS bindings, so prefer extending
rather than reshaping existing entry points.

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
  attribute.{h,cc}         AttrData<T>, AttrRef, AttrGroup, type_dispatch
  attribute_base.h         AttrDataBase, type_to_attrtype<T>()
  attribute_builtin.h      BuiltinAttr<T, Name, Flag>
  attribute_bool.h         PackedBoolAttrs, BoolAttrView (bit-packed bool storage)
  attribute_enums.h        AttrType, AttrFlag, ATTR_PAGE* constants
  bindings.{h,cc}          Glue for the binding/reflection registration
  idmap.{h,cc}             Stable external IDs over slot indices
  mesh_shapes.{h,cc}       createCube primitive constructor
  utils/triangulate.h      Fan triangulator (header-only)
  c-api/mesh_c_api.{h,cc}  extern "C" surface for WASM/JS
  gpu/mesh_drawbatch.{h,cc} Mesh -> sculptcore::gpu::DrawBatch builder
```
