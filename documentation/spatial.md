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
* `add_face(int)` / `add_face_intern(...)` — descends into children
  whose AABB overlaps the face's triangles, splitting leaves on the
  way down when `node_needs_split` is true. Leaf-level work records
  face/vertex ownership into `treeMesh.{f,v}.node`.
* `split_node(SpatialNode*)` — picks the longest-axis midpoint
  (currently forced to `t = 0.5`, see *Design notes*), allocates two
  children with halves of the parent's AABB, unassigns the parent's
  faces/verts on the live mesh, and re-inserts them through
  `add_face_intern`.
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
  ordered sets of `unique_verts`, `other_verts`, `unique_faces`,
  `other_faces`, plus the triangulated `tris` (`NodeTri`, three corner
  indices into the mesh + face index + eflag).
* `gpu_data` (type `GpuData*`) — populated only on GPU nodes.
* `subtree_tri_count` — cached sum of `data->tris.size()` over leaves
  in this subtree. Recomputed bottom-up once per `update()` tick
  (`recompute_subtree_tri_counts`).
* `is_gpu_node` — set by `assign_gpu_nodes()` to mark the partition.

`unique_*` vs `other_*`: a face is *unique* to whichever leaf first
claims it (recorded in `treeMesh.f.node`); other leaves whose AABB
also overlaps it list it as `other` for query coverage but never own
its tris. Same rule for verts. This is what makes the partition
sound: aggregating `data->tris` (built only from `unique_faces`)
across leaves of a GPU subtree visits each face exactly once.

### `NodeData`
Per-leaf payload. Four `OrderedSet<int>`s for verts/faces, a
`Vector<NodeTri>` triangulation of `unique_faces`, and the parent
`Mesh*`. Recreated/destroyed as nodes turn into leaves or split.

### `GpuData`
Per-GPU-node payload. Owns its `gpu::Buffer *pos`/`*nor` and the
`gpu::DrawCommand *cmd`. `slices: Vector<LeafSlice>` is the
DFS-order leaf list defining the buffer layout — each entry records
`(leaf*, vert_start, vert_count = tris*3)`. `total_verts` is the sum
of all `vert_count`s.

`dispose()` `Delete`s the buffers and clears `slices`; the destructor
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

```
SpatialTree::update(gpu)
  ├── propagate Spatial_RegenBounds up to root → regen_node_bounds(root, true)
  ├── for each leaf with Spatial_RegenTris → regen_node_tris(leaf)
  ├── for each leaf with Spatial_UpdateNormals → update_node_normals(leaf)
  ├── if any leaf re-tri'd OR root not yet a GPU node:
  │     ├── recompute_subtree_tri_counts()      (bottom-up)
  │     └── assign_gpu_nodes()                  (top-down, threshold = gpu_tri_target)
  ├── for each leaf with Spatial_{RegenGPU,UpdateGPU}:
  │     ├── owner = find_gpu_owner(leaf)        (walk parents until is_gpu_node)
  │     ├── full rebuild if owner has no buffers OR layout missing OR Spatial_RegenGPU set:
  │     │     └── regen_gpu_node(owner, gpu)
  │     └── else: update_gpu_node_slice(owner, leaf, gpu)
  ├── any GPU node still missing buffers (newly promoted, no dirty leaves):
  │     └── regen_gpu_node(node, gpu)
  └── if anything changed → rebuild drawBatch from the set of GPU nodes
```

`regen_gpu_node` collects subtree leaves, sums `tris.size() * 3` for
`total_verts`, allocates `pos`/`nor` buffers, and fills them slice by
slice via `fill_leaf_slice`. The `DrawCommand` is recreated by the
draw-batch loop when needed.

`update_gpu_node_slice` finds the leaf's `LeafSlice` in the owner's
`slices`. If the leaf's current tri count no longer matches the
recorded `vert_count`, it falls back to a full `regen_gpu_node` —
that means the leaf's topology shifted since the partition was built
and per-slice offsets are stale.

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

`meshlog`'s simple chunks call `node->update(Spatial_UpdateGPU |
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
  mesh sees every face as already owned (goes to `other_faces`,
  empty `unique_faces`, empty tris). Use a fresh mesh per tree in
  tests; in production this is a non-issue because there's one tree
  per sculpt session.
* **`other_verts` / `other_faces` are coverage, not ownership.**
  Treating them as authoritative produces double-rendered tris on
  the GPU side and double-summed normals during
  `update_node_normals`.
* **Slice fallback is a real path, not a panic.** A leaf can change
  its tri count between two `update()` ticks (sculpt-time topology
  edit). When that happens `update_gpu_node_slice` correctly bails to
  a full `regen_gpu_node`; that branch is exercised by the topology
  sculpt brushes and should not be silently dropped.
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
