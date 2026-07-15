# Spatial — High-level overview

The spatial subsystem (`source/spatial/`) is a BVH-style binary tree
layered over a `mesh::Mesh`. It does two largely independent jobs:

* **Spatial queries / brush iteration** — `castRay`, `filterNodes`,
  and per-leaf vertex/face iteration drive the brush executor and the
  view-3D picking path. These run against the *leaves* of the tree;
  `leaf_limit` (verts per leaf) tunes their granularity.
* **GPU draw batching** — a *subset* of nodes (the "GPU nodes") owns
  one aggregated VBO + draw command covering every triangle of every
  leaf in its subtree. The partition is chosen so each GPU node's
  subtree tri count fits a user-controllable target
  (`gpu_tri_target`, default 2048).

The two layers share the same node objects but use disjoint state:
`SpatialNode::data` (leaf-only) holds the spatial-query payload;
`SpatialNode::gpu_data` (GPU-node-only) holds the VBOs and per-leaf
slice table. A leaf that is also small enough to be its own GPU node
holds both.

Face/vertex ownership is recorded on the live mesh via two attributes
(`treeMesh.f.node`, `treeMesh.v.node`) that store the owning *leaf*'s
node id. Every face belongs to exactly one leaf's `unique_faces`, so
concatenating subtree leaves' tris in a GPU node renders each face
exactly once.

## File map

| File | Role |
|---|---|
| `spatial.h` / `spatial.cc` | `SpatialTree` — build, update, query, GPU partition, draw batch assembly. |
| `node.h` / `node.cc` | `SpatialNode`, `NodeData`, `GpuData`, `LeafSlice`, `NodeTri`; per-leaf ray-cast. |
| `spatial_gpu.cc` | GPU-node buffer regen and per-leaf slice update. |
| `spatial_attrs.h` | `SpatialTreeMesh` — the `.spatial.{v,f}.node` and `.spatial.v.mask` builtin attributes living on the mesh. |
| `spatial_enums.h` | `NodeFlags` bitmask (`Spatial_Leaf`, `Spatial_RegenBounds`, `Spatial_RegenTris`, `Spatial_RegenGPU`, `Spatial_UpdateGPU`, `Spatial_UpdateNormals`). |
| `spatial_base.h` | `CastRayIsect` ray-cast result. |
| `bindings.h` / `bindings.cc` | `registerBindings(BindingManager&)` — exposes `SpatialTree`, `SpatialNode`, `NodeFlags`, `SpatialShaders` to the litestl reflection layer. |
| `c-api/spatial_c_api.cc` | C surface for WASM/JS construction of trees. |
| `shaders/` | `basic_mesh.wgsl`, `basic_line.wgsl` and the `SpatialShaders` registry used by GPU draw batches and the leaf-bounds debug viz. |
| `CMakeLists.txt` | Static library `spatial`, depends on `util`, `math`, `mesh`, `gpu`. |

## Core types

### `SpatialTree`
Owner of the tree. Holds the root node, a flat `nodes` vector for
linear iteration, a node-id → node lookup (`node_idmap`), the
`SpatialTreeMesh` attribute set, and the assembled
`sculptcore::gpu::DrawBatch*`.

Public tunables:

* `leaf_limit` (default 512) — vertex count above which a leaf splits.
  Controls spatial-query granularity. Independent of GPU batching now
  that GPU nodes can sit above leaves.
* `depth_limit` (default 10) — hard cap on `node_needs_split` recursion.
* `gpu_tri_target` (default 2048) — desired upper bound on tris per
  GPU node. A leaf whose own tri count exceeds the target still
  becomes its own GPU node (cannot split further from the GPU layer's
  POV).

Notable methods:

* `buildAll()` — calls `setup()`, computes the root AABB, recalcs
  normals, then inserts all mesh faces in a randomised order to
  balance the tree better.
* `add_face(int)` / `add_face_intern(...)` — routes the face by its
  centroid into the single child it belongs in (one scalar compare
  against the split midplane), splitting leaves on the way down when
  `node_needs_split` is true. Leaf-level work records face/vertex
  ownership into `treeMesh.{f,v}.node`. **Incremental
  (dyntopo, M7.6):** if a new face already has an owned vert,
  `add_face` skips the root descent and files it straight into that
  neighbour's leaf via `add_face_at` (O(1) anchor placement), deferring
  the leaf split — see the incremental-currency methods below.
* `split_node(SpatialNode*, claimTag)` — splits along the longest axis at
  the geometric **mean** of the node's verts (a fraction `t` of the box,
  clamped off the edges to `[0.01, 0.99]` so a degenerate all-in-one
  child can't happen), allocates two children, unassigns the parent's
  faces/verts on the live mesh, and re-inserts them through
  `add_face_intern` (centroid routing only — no re-triangulation; the
  build-path `add_face` keeps `triangulateFace` for its degenerate-face
  admission check). `claimTag` is the "unassigned" marker written into
  the split's verts' `.spatial.v.node` entries and the only value its
  re-file may claim (0 serially; a unique negative tag per parallel
  candidate, see below).
* **Incremental dyntopo currency (M7.6).** `add_face_at` defers the
  inline split of over-full leaves into `nodeSplitCandidates_`;
  `applyDeferredNodeSplit()` (top of `update()`) splits each once —
  **candidates in parallel** (they are disjoint subtrees; `alloc_node`
  is mutex-guarded as the only shared bookkeeping). Determinism is kept
  by two mechanisms: per-candidate negative claim tags stop a candidate
  from racing on boundary verts another candidate unassigned (which
  reproduces serial claim semantics exactly), and a renumbering epilogue
  reassigns the new nodes' ids + `nodes` order in candidate DFS order
  and re-stamps their ownership columns, so trees are bit-identical to
  the serial candidate order. `SC_SPLIT_SERIAL=1` forces the serial path
  for A/Bs.
  `remove_vert` records shrinking leaves' parents in `mergeCandidates_`;
  `applyDeferredMerge()` (every `mergeCadence_`-th `update()`) folds
  under-full sibling leaves back into their parent via `merge_node`,
  cascading up, with `free_node` doing an O(1) swap-remove that keeps
  the `node->index == nodes[index]` invariant `castRay` relies on. This
  keeps a dab's tree maintenance O(brush region) (2–11 ms at 5 M) rather
  than a full rebuild.
* `castRay(orig, dir, out)` — defers to `SpatialNode::castRay`, which
  descends to leaves for triangle-level precision, then resolves
  hit position/normal from the matching mesh corners.
* `filterNodes(co, radius, out)` — returns leaves for brush iteration.
* `update(GPUManager*)` — the per-frame phase pipeline (see below).
* `leaves()` / `gpu_nodes()` — flat-vector scans returning the leaf
  set and the GPU-node set respectively.

### `SpatialNode`
The tree node. Always has `parent` and `children[2]` (`nullptr` for
leaves); carries an `AABB`, a `NodeFlags` bitmask, an integer `id`
(stable across the tree's lifetime, > 0), an `index` into
`SpatialTree::nodes`, and pointers to the two payload structs.

* `data` (type `NodeData*`) — populated only on leaves. Holds the
  ordered sets of `unique_verts` and `unique_faces`, plus the
  triangulated `tris` (`NodeTri`, three corner indices into the mesh +
  face index + eflag).
* `gpu_data` (type `GpuData*`) — populated only on GPU nodes.
* `subtree_tri_count` — cached sum of `data->tris.size()` over leaves
  in this subtree. Recomputed bottom-up once per `update()` tick
  (`recompute_subtree_tri_counts`).
* `is_gpu_node` — set by `assign_gpu_nodes()` to mark the partition.

Single-owner partition: each face is routed to exactly one leaf by its
centroid (`add_face_intern` does a single scalar compare against the
split midplane per level — no triangle/AABB overlap test, so a face is
never replicated into multiple leaves), and the owner is recorded in
`treeMesh.f.node`. A vert is owned by the first leaf to claim it. This
is what makes the partition sound: aggregating `data->tris` (built from
`unique_faces`) across leaves of a GPU subtree visits each face exactly
once. Leaf AABBs still cover neighbour-owned boundary verts via the
`tris` loop in `regen_node_bounds`.

### `NodeData`
Per-leaf payload. Two `OrderedSet<int>`s (`unique_verts`,
`unique_faces`), a `Vector<NodeTri>` triangulation of `unique_faces`,
and the parent `Mesh*`. Recreated/destroyed as nodes turn into leaves
or split.

### `GpuData`
Per-GPU-node payload. Owns its `gpu::Buffer *pos`/`*nor`, a
`Vector<gpu::Buffer *> attrBufs` (one per *requested attribute*, see
below), and the `gpu::DrawCommand *cmd`. `slices: Vector<LeafSlice>` is
the DFS-order leaf list defining the buffer layout — each entry records
`(leaf*, vert_start, vert_count = tris*3)`. `total_verts` is the sum
of all `vert_count`s. `builtAttrsVersion` records the
`SpatialTree::requestedAttrsVersion` these `attrBufs` were filled
against, so the slice fast-path can detect a stale set and fall back to
a full regen.

`attrBufs` is index-aligned with `SpatialTree::requestedAttrs` (slot
order, after the implicit `pos@0`/`nor@1`). When no requested set is
installed it instead holds a single legacy `color` stream (float4,
default white) so the basic mesh shader's `@location(2)` is never left
unbound — the dynamic path only kicks in once `setDrawShader` has marked
the tree's material shader ready.

`dispose()` `Delete`s `pos`/`nor`, every `attrBufs` entry, and `cmd`,
then clears `slices` and resets `builtAttrsVersion`; the destructor
calls `dispose()` so the leak-tracking allocator stays happy when a
GPU node loses GPU-node status mid-update.

### `LeafSlice`
Plain triple `(SpatialNode *leaf, int vert_start, int vert_count)`.
Lookups inside `update_gpu_node_slice` walk this vector linearly —
the slice count per GPU node is bounded by `gpu_tri_target /
avg_leaf_tris`, so it stays small in practice.

### `SpatialTreeMesh`
Wrapper around three builtin attributes living on the *mesh*, not on
the tree:

* `.spatial.v.node` (`int`, per-vert) — owning leaf's node id, or 0.
* `.spatial.f.node` (`int`, per-face) — owning leaf's node id, or 0.
* `.spatial.v.mask` (`float`, per-vert) — sculpt mask channel.

These outlive the tree object; destroying a `SpatialTree` does *not*
clear them. Tests that build multiple trees against the same mesh
must use a fresh `Mesh` per iteration, or the second tree will see
every face as already-owned and produce empty leaves.

### `NodeFlags`
Per-node dirty bitmask. Leaves accumulate flags from upstream events
(`split_node`, `add_face_intern`, sculpt/meshlog undo); `update()`
clears them after acting:

* `Spatial_Leaf` — set on leaves; cleared by `split_node`.
* `Spatial_RegenBounds` — recompute this node's AABB (and the
  ancestor chain).
* `Spatial_RegenTris` — leaf's `unique_faces` changed; rebuild
  `data->tris`. Implies the owning GPU node needs a full rebuild.
* `Spatial_RegenGPU` — leaf's triangle set changed in a way that
  invalidates its slice's vert count; forces full rebuild of the
  owning GPU node.
* `Spatial_UpdateGPU` — leaf's vertex positions/normals changed but
  topology did not; can be served by a per-slice rewrite.
* `Spatial_UpdateNormals` — recompute per-vert/per-face normals
  for this leaf.

## Update lifecycle

The pipeline is split into two halves behind one private
`updateImpl(gpu, phases)`:

* **`updateQueries()`** — the *queries half* only: everything the next brush
  dab's spatial queries need (deferred split/merge, tris, bounds, normals).
  Runs **per dab** (the TS sculpt path calls it from `applyDab` and from the
  GPU-stroke readback sync). It consumes only `Spatial_RegenTris` /
  `Spatial_RegenBounds` / `Spatial_UpdateNormals`; the GPU dirty bits
  (`Spatial_RegenGPU` / `Spatial_UpdateGPU`) survive untouched. Returns
  "bounds changed".
* **`update(gpu)`** — both halves: the queries half followed by the *GPU
  half* (partition, dirty-bit propagation, plan, fill, upload flags,
  draw-batch rebuild). Runs **once per rendered frame** (`LiteMesh.drawQ`)
  and at every non-stroke site (undo, load, tests). Returns "bounds changed
  **or** GPU work done", so a drawQ call mid-stroke still reports work after
  the per-dab `updateQueries()` calls consumed the bounds dirt.

Cross-call state: the queries half sets the sticky `pendingGpuTopology_`
whenever it regenerated leaf tris (fresh split/merge leaves carry
`Spatial_RegenTris`, so restructures arm it too); the GPU half consumes and
clears it to gate `recompute_subtree_tri_counts()` + `assign_gpu_nodes()`.
The merge cadence (`updatesSinceMerge_`) advances in the queries half only.

One deliberate consequence: per-leaf buffer fills happen at the *frame*, not
the dab, so a border leaf's replica of a vert moved by a dab that never
flagged that leaf is refilled with final values at the frame flush — the
split world's staging buffers are never staler than the old per-dab world's
(gated byte-exact in `tests/test_spatial_update_split.cc`).

```
SpatialTree::update(gpu)  =  updateImpl(gpu, Update_All)
  ── queries half (updateImpl with Update_Queries; alone = updateQueries()) ──
  ├── applyDeferredNodeSplit()                  (parallel over candidates; see below)
  ├── applyDeferredMerge()                      (every mergeCadence_-th queries half, serial)
  ├── parallel_for leaves with Spatial_RegenTris → ensure_node_tris(leaf)
  ├── propagate Spatial_RegenBounds up to root → regen_node_bounds(root, true)
  ├── parallel_for leaves with Spatial_UpdateNormals → update_node_normals(leaf)
  ├── pendingGpuTopology_ |= any leaf re-tri'd
  ── GPU half (Update_Gpu; `gpu` is only dereferenced here) ──
  ├── if pendingGpuTopology_ (consumed+cleared) OR root not yet a GPU node:
  │     ├── recompute_subtree_tri_counts()      (bottom-up)
  │     └── assign_gpu_nodes()                  (top-down, threshold = gpu_tri_target)
  ├── collect, per dirty leaf (serial scan):
  │     ├── owner = find_gpu_owner(leaf)        (walk parents until is_gpu_node)
  │     ├── full rebuild needed (no buffers / layout missing / Spatial_RegenGPU):
  │     │     └── owner → regenOwners (deduped by id; also any GPU node missing buffers)
  │     └── else: {owner, leaf} → sliceWork  (dropped later if its owner got planned)
  ├── serial per regen owner: plan_regen_gpu_node(owner, gpu, srcRefs)
  │     └── emits one fill job per non-empty slice
  ├── unified parallel_for over regen fill jobs + slice work:
  │     ├── fill_regen_slice(owner, sliceIdx, srcRefs)          (pure, disjoint ranges)
  │     └── update_gpu_node_slice(owner, leaf, gpu)             (pure, in-place)
  ├── serial epilogue: per slice owner, flag buffers update_buffer=true;
  │           regen_gpu_node(owner) (deduped) for any slice that returned "needs rebuild"
  └── if any regen happened → rebuild drawBatch from the set of GPU nodes
```

A full owner rebuild is split in two. **`plan_regen_gpu_node`** (serial —
`gpu::GPUManager` is not thread-safe) disposes the old buffers, collects
subtree leaves (re-tri'ing any still flagged), sums `tris.size() * 3` for
`total_verts`, allocates `pos`/`nor` plus one buffer per requested
attribute, builds the `LeafSlice` table, clears the subtree leaves' GPU
dirty flags, resolves each requested attribute's source layer once, and
stamps `builtAttrsVersion`. **`fill_regen_slice`** then fills one slice
(`fill_leaf_slice` for pos/nor, `fill_leaf_attr` per attribute); the fills
are pure disjoint-range writes, so they run in the same parallel pass as
the in-place slice updates. `regen_gpu_node` remains as plan + serial fill
for the fallback sites. Owner dedup happens at collection time (a set of
owner ids) — it must not rely on the first regen clearing leaf flags
mid-loop once fills overlap. The `DrawCommand` is recreated by the
draw-batch loop when needed. `SC_FILL_SERIAL=1` forces the unified pass
serial for A/Bs; fills are pure gathers, so parallel output is
bit-identical to serial.

`update_gpu_node_slice` finds the leaf's `LeafSlice` in the owner's
`slices` and rewrites only that disjoint sub-range in place. It is
**pure and data-race-free** because it runs under `parallel_for`: it
writes only its own slice and its own leaf flag, and **never** calls
`regen_gpu_node` or touches the owner-level `update_buffer` flags. If an
in-place update isn't possible — the leaf's tri count no longer matches
the recorded `vert_count`, the slice isn't found, **or**
`gd.builtAttrsVersion` has drifted from the tree's `requestedAttrsVersion`
— it returns `false` and the **serial** pass after the loop does the full
`regen_gpu_node` (deduped per owner). That serial split is deliberate:
`regen_gpu_node` disposes and reallocates the node's shared `GpuData`
(`pos`/`nor`/`attrBufs`/`cmd`/`slices`), so two worker threads regenning
the same owner concurrently could leave it with a null `pos`, which the
draw-batch loop then silently skips — the historical "faces randomly
don't draw" bug. Keeping every shared-state mutation in the serial pass
removes that race while the in-place fill stays parallel.

## Requested attributes & the material draw shader

By default a GPU node draws with the tree's built-in `basicMeshShader`
(pos/nor + a legacy color stream). To render a leaf with a *material's*
shader instead, the renderengine pushes two things down the C API
(`setTreeRequestedAttrs` / `setTreeDrawShader`, see
`source/spatial/c-api/spatial_c_api.cc`):

* **`setRequestedAttrs(span<RequestedAttr>)`** — the set of named mesh
  attributes the material reads (`gpu::RequestedAttr` in
  `source/gpu/gpu_attr_request.h`: `name`, `srcType`, `gpuType`,
  `elemSize`, `slot`, `domain`, `defaultKind`). It **stores the set
  canonically in `slot` order** (sorting the caller's array), bumps
  `requestedAttrsVersion`, and re-flags every leaf `Spatial_RegenGPU` so
  the next `update()` rebuilds buffers to the new layout. Slot-ordering
  here is load-bearing: the per-attribute buffers are built/filled in
  `requestedAttrs` *index* order and bound to `cmd->attrs` in that order,
  so storing slot-ordered makes index order == slot order regardless of
  how the caller passed the set.
* **`setDrawShader(wgsl)`** — installs the material's WGSL and builds a
  tree-owned dynamic `ShaderDef` whose vertex attributes are
  `pos@0, nor@1`, then `requestedAttrs` in stored (slot) order — a
  straight append, since the set is already canonical. This must match
  the TS-side contract (`slot = 2 + index`, see
  `documentation/shader-attributes.md` in the app repo): the i-th
  `cmd->attrs` buffer binds to the i-th `ShaderDef` attribute, i.e. to
  `@location(slot)`.
* **`refreshRequestedAttrs()`** — forces a per-attribute buffer rebuild
  against the *current* mesh layers without touching the `ShaderDef`.
  Needed because `setRequestedAttrs` **short-circuits when the incoming
  set is byte-identical** to the stored one: adding or removing a mesh
  layer whose `domain` happens to match the requested attribute's
  category default leaves every `RequestedAttr` field unchanged, so the
  version never bumps and the buffers stay default-filled even though the
  real data is now present (or gone). The renderengine watches a cheap
  attribute-layer signature (`LiteMesh.attrLayersSignature`) and calls
  this — instead of re-issuing `setDrawShader` — when only the layers
  moved. It recomputes `missingAttrSlots`, bumps
  `requestedAttrsVersion`, drops the draw batch, and re-flags every leaf
  `Spatial_RegenGPU`.

`fill_leaf_attr` gathers each attribute per corner: VERTEX-domain layers
are read through the corner's vertex (`c.v`), CORNER/FACE layers read
directly. **It never throws** — a missing or non-float source layer is
default-filled (zero, or white for color) so the frame is never blank.
`computeMissingAttrSlots()` reports which requested slots had no backing
layer, but it is **advisory only**: the draw always proceeds with the
default-filled buffers. The dynamic path engages only once
`requestedAttrs` is non-empty *and* `setDrawShader` has marked the
material shader ready; until then the legacy color stream is used.

## GPU partition algorithm

```
assign(node):
  if node is a leaf OR node->subtree_tri_count <= gpu_tri_target:
    node->is_gpu_node = true
    unmark_descendant_gpu_nodes(node)   // dispose stale gpu_data
    return
  node->is_gpu_node = false
  if node had gpu_data: dispose + null
  assign(children[0])
  assign(children[1])
```

Invariants the partition guarantees:

* Every leaf has exactly one GPU-node ancestor (`find_gpu_owner`
  walks up; the loop terminates by the time it reaches the root,
  which is always a GPU node once trees have any geometry).
* No GPU node is an ancestor of another GPU node.
* `sum(gpu_node.subtree_tri_count) == total mesh tri count`.

These are checked by `tests/test_spatial_gpu_partition.cc` across
`gpu_tri_target ∈ {64, 256, 2048, 100000}`.

## Ray-cast / filter paths

`SpatialNode::castRay` is implemented in `node.h` and is recursive.
Internal nodes descend into whichever children's AABB the ray hits;
leaves iterate `data->tris` and call `math::rayTriIsect`. The leaf
that wins records `nodeIndex` (into `SpatialTree::nodes`) and
`triIndex` (into that node's `data->tris`); `SpatialTree::castRay`
then resolves the world-space hit position and interpolated normal
from the mesh.

`filterNodes` currently returns all leaves unconditionally (the
sphere/AABB cull is disabled with `if (1 || ...)`); it bumps each
returned leaf's `debugIdOffset` so the leaf-bounds debug colours
animate when iterated. Brush iterators (`brush_iterators.h`) walk
returned leaves' `unique_verts`.

## Integration with meshlog / undo

`meshlog`'s element-store chunks call `node->update(Spatial_UpdateGPU |
Spatial_RegenBounds)` on the leaf they swapped attributes for. The
flags accumulate per-leaf; the next `SpatialTree::update` tick walks
the parent chain to find the owning GPU node and reroutes the dirty
notification there. Meshlog itself stays leaf-scoped — it never
touches `gpu_data` or `is_gpu_node`.

## Design notes / pitfalls

* **Splits use the AABB midpoint, not the centroid.** `split_node`
  computes the mean of `unique_verts` to *pick the axis*, then forces
  `t = 0.5` along the longest axis. This produces an unbalanced
  tree on non-uniform meshes; the GPU partition tolerates that
  (nodes promote at varying depths) but query performance suffers.
  Improving the split is a separate task.
* **Ownership attributes live on the mesh.** Destroying a tree leaves
  `.spatial.{v,f}.node` populated. A second tree built on the same
  mesh sees every face as already owned (non-zero `.node`), so it
  never re-claims them — empty `unique_faces`, empty tris. Use a fresh
  mesh per tree in tests; in production this is a non-issue because
  there's one tree per sculpt session.
* **Slice fallback is a real path, not a panic.** A leaf can change
  its tri count between two `update()` ticks (sculpt-time topology
  edit). When that happens `update_gpu_node_slice` returns `false` and
  the serial pass after the `parallel_for` does the full
  `regen_gpu_node`; that branch is exercised by the topology sculpt
  brushes and should not be silently dropped. **Never** call
  `regen_gpu_node` from inside the slice `parallel_for` — it mutates
  shared `GpuData`; the false-return + serial-regen split exists to keep
  that off the worker threads.
* **The draw batch is regenerated, not patched.** `update()` rebuilds
  `drawBatch->commands` and `->buffers` from the current GPU-node set
  any time anything changes. Cheap because GPU-node count is small;
  don't optimize prematurely.
* **Root is always a GPU node once geometry exists.** Used as a
  sentinel: `if (!root->is_gpu_node)` triggers a one-time partition
  assignment after `buildAll`.

## Bindings

`registerBindings(BindingManager&)` adds `SpatialTree`, `SpatialNode`,
`NodeFlags`, and `SpatialShaders` to the binding manager, plus
`Vector<SpatialNode *>` for collection returns.

`SpatialTree::defineBindings()` exposes the constructor (taking a
`Mesh*`), `leaf_limit` / `depth_limit` / `gpu_tri_target`, and the
methods JS hosts drive sculpt with — `setup`, `add_face`,
`split_node`, `node_from_id`, `leaves`, `ensure_node_tris`,
`buildAll`, `buildLeafBoundsBatch`, `update`, `getDrawBatch`,
`castRay`, `filterNodes`. `SpatialNode::defineBindings()` exposes
`aabb`, `flag`, `id`, and `debugIdOffset`; the per-leaf payloads
(`data`, `gpu_data`, `subtree_tri_count`, `is_gpu_node`) are
intentionally not surfaced — they are internal scaffolding driven by
`update()`.

The C surface in `c-api/spatial_c_api.cc` is the construction entry
point used by the WASM glue; touch additively, like
`source/mesh/c-api/`.
