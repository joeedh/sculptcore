# Proposed Spatial / Brush Speedups

*Status: deferred. Capture from the May 2026 small-radius-stroke audit.
Pick up after the [brush compute DSL](brush_compute_dsl.md) plan lands —
the DSL replaces the per-leaf brush executor, so several of these fixes
want to be revisited against the new IR rather than retrofitted into the
current C++ executor.*

## Background

Small-radius brush strokes were ~10–100× slower than large-radius
strokes on the debug app, even with stroke spacing maxed out. A first
pass landed three fixes:

- Re-enabled the AABB-sphere prune in `SpatialTree::filterNodes`
  (`spatial.cc:45`) — it had been short-circuited and was returning
  every leaf in the tree per dab.
- Removed a stray `printf` in the GPU-partition phase
  (`spatial.cc` `update()`).
- Made `update_node_normals` incremental: per-node `affected_verts` is
  populated by the brush, and the normals pass only zeros / accumulates
  / normalizes the 1-ring around moved verts. Falls back to a full
  rebuild when `affected_verts` is empty (initial build or topology
  change cleared it in `regen_node_tris`).

What's left, in rough order of impact:

## 1. Dirty-bounds queue + grow-only bounds

**Where:** `SpatialTree::update()` at `spatial.cc:595-603`;
`regen_node_bounds` at `spatial.cc:256+`.

**Cost today:** every frame iterates the flat `nodes` list (leaves +
internals) to find dirty leaves, then `regen_node_bounds` recomputes
each dirty leaf's AABB by walking every `unique_vert` and every corner
of every `unique_face` in that leaf — even when the brush only moved a
handful of verts.

**Fix:**
1. Add `Vector<SpatialNode *> dirty_bounds_leaves` on `SpatialTree`,
   appended by the brush whenever it populates `affected_verts` (use a
   `Spatial_InDirtyQueue` flag to dedupe). Replace the scan with a
   walk over the queue; move ancestor propagation into the same loop.
2. In `regen_node_bounds`, when `affected_verts` is non-empty, expand
   the existing AABB only by the moved verts:
   ```cpp
   for (int v : node->affected_verts) {
     node->aabb.min.min(m->v.co[v]);
     node->aabb.max.max(m->v.co[v]);
   }
   ```
   This grows but never shrinks. That's correct for sculpt strokes,
   which push outward; the only downside is a slightly loose AABB, which
   makes `aabbSphereIsect` marginally more permissive but never wrong.
   Schedule a full rebuild on topology change (or every N strokes) to
   reclaim looseness.
3. Skip propagation to the root when the leaf's grown AABB still fits
   inside its parent's AABB — most dabs don't escape.

Takes per-step bounds cost from O(total_nodes + Σ dirty_leaf_size) to
O(dirty_leaves × moved_verts_per_leaf).

## 2. Per-vert spatial reject in the brush iterator

**Where:** `BasicVertexIter` in `brush_iterators.h:36+`, consumed by
`brushes/draw.h:34+` (and every other per-vertex brush).

**Cost today:** the iterator walks every `unique_vert` in each filtered
leaf and evaluates `strength()` (sqrt + falloff curve) on it, even for
verts the brush sphere can't reach. With a small brush in a large leaf,
that's potentially 100× more `sqrt`s than affected verts.

**Tier 1 — squared-distance early-out:** in the brush body, compute
squared distance to the brush center first and `continue` on `d² >= r²`
before doing the `sqrt`/falloff. Rejected path drops to a vector
subtract + dot + branch. Trivial, do regardless of the bigger fix.

**Tier 2 — per-leaf vert spatial index:** lazily build a uniform hash
grid over each leaf's `unique_verts` (cell size ≈ typical-brush-radius;
build on first query, invalidate on topology change). Brush dab queries
only the cells overlapping the brush sphere. Iteration count drops to
O(cells_touched × verts_per_cell), independent of leaf size. Morton-
sort-and-range-search is a cheaper but looser alternative.

**Note for the DSL transition:** the iteration shape moves from a
hand-coded C++ loop to a kernel scheduled by the executor against a
node-vert iterator. Tier 1 is something the codegen can emit
unconditionally; Tier 2 wants to live on the node / executor side
(below the DSL) so every backend gets it for free.

## 3. Incremental tri-count propagation

**Where:** `SpatialTree::update()` at `spatial.cc:660+`.

**Cost today:** any `Spatial_RegenTris` on any leaf flips
`topology_changed`, which triggers a full `recompute_subtree_tri_counts()`
+ `assign_gpu_nodes()` over the whole tree.

**Fix:** when `regen_node_tris` runs, compute `delta = new_tris -
old_tris` and walk up the parent chain adding the delta to
`subtree_tri_count`. Only re-run `assign_gpu_nodes()` when the GPU
partition's invariant is actually violated (a GPU node's subtree count
crossed `gpu_tri_target`), not on every topology change.

## 4. GPU node regen — cache per-leaf tri counts

**Where:** `spatial_gpu.cc:98-103`.

**Cost today:** `regen_gpu_node` walks every leaf in the GPU node's
subtree (can be 100+ leaves at default `gpu_tri_target=2048`) to count
verts, even if only one leaf changed.

**Fix:** cache `tris.size() * 3` on each leaf, updated when
`regen_node_tris` runs. Walk only dirty leaves; trust the cache for the
rest. Keep the existing `expected_vcount != slice->vert_count` check in
`update_gpu_node_slice` as the safety valve.

## 5. Draw batch — cache the GPU-node list

**Where:** `SpatialTree::update()` at `spatial.cc:732-757`.

**Cost today:** rebuilds the draw batch by iterating *all* nodes to
find GPU nodes whenever anything was dirty.

**Fix:** maintain the GPU-node list as a `Vector<SpatialNode *>` on
`SpatialTree`, updated incrementally inside `assign_gpu_nodes()`. Draw
batch rebuild walks the list directly.

## Non-fixes (ruled out by the audit)

- `filterNodes` itself is not a hot path. A few hundred AABB-sphere
  tests per dab is microseconds. Don't add a query cache on top.
- Stroke-spacing math in `stroke_spacing.h` is sound; the apparent
  small-radius slowdown was downstream of `filterNodes`, not in the
  spacing calculation.
