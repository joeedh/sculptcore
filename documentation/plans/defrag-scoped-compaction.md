# Scoped / partial compaction for mechanism-B DRAM defrag

## Status

Not started. Mechanism B (incremental DRAM compaction) is landed and `auto_defrag`
is now **default-on**. The remaining work is making a stroke-boundary compaction
**O(stroke region)** instead of **O(whole mesh)**, so it stays cheap up to the
5 M-element target. This plan is that work.

## Goal & success metric

A stroke-end compaction must cost on the order of the geometry the stroke just
touched, not the whole mesh.

- Now: `applyReorderIncremental` is **~429 ms at 298 k verts** → linearly
  **~7 s at 5 M** (a full-mesh permutation regardless of how little changed).
- Target: a stroke-boundary compaction is **sub-100 ms at 5 M** (bounded by the
  brush region / fragmented leaves, not total element count).
- Hard constraint: identical end-state correctness to the current full path
  (geometry preserved, mesh valid, undo exact), verified by cross-validation +
  the existing `test_spatial_reorder_inc` style harness.

## Background — where the cost is

`MeshLog::compactIfFragmented` → `computeLocalityMaps` → `applyReorderIncremental`:

1. `computeLocalityMaps` (spatial.cc) walks **every** leaf and emits a **full
   bijection over each domain's capacity** (`finish_map` packs all live elements
   to the front). So even one fragmented leaf produces a whole-mesh permutation.
2. `applyReorderIncremental` (spatial.cc):
   - `Mesh::reorder_{verts,edges,corners,lists,faces}` (mesh.cc). Each one
     **(a)** scans the referencing domain(s) to remap cross-domain refs
     (`e.vs`, `c.v`, `c.e`, `l.f`, …) — O(refs) — then **(b)** calls
     `ElemData::reorder` → `AttrGroup::reorder` (attribute.h), which **rewrites
     the entire attribute arrays** for that domain — O(capacity × attrs).
   - Remaps **every** node's cached indices (`unique_verts`/`unique_faces`/
     `tris`) — O(all cached elements).
3. `LogChunkReorder` stores the **full** permutation maps (5 × capacity ints) for
   undo — O(capacity) memory per compaction step.

Every one of these is O(mesh) and pays full price even when the map is mostly
identity.

## Key insight

A stroke only fragments the **brush region**. After a dab, the rest of the mesh's
layout is unchanged, so re-permuting the whole mesh is wasted work. Compact **only
the leaves the stroke touched / scored as fragmented**. That permutation is
**mostly identity** (only the region's elements move), which is exactly the
structure the scoped permutation exploits. Partial scope and scoped permutation
are two halves of the same optimization: partial scope *produces* the mostly-
identity map; scoped permutation *applies* it cheaply.

The executor already knows the dab region (it iterates the in-region `SpatialNode`s
and threads `seedVerts`); `fragmentationStats` already computes a per-leaf page
spread. So the region is available without new bookkeeping.

## Design

Five coupled changes, each reducing an O(mesh) term to O(region). **Profile the
breakdown first** (Phase 0) — the attribute rewrite is expected to dominate, so
the early phases target it; the reference/ node scans may already be cheap enough
to leave as full passes initially.

### 1. Region selection — which leaves to compact
- Add a per-leaf fragmentation score (reuse `fragmentationStats`' page-spread per
  leaf). Select leaves above a threshold, OR take the leaves the just-finished
  stroke touched (the executor's region set) — decide by Phase-0 measurement.
- Output: a `Vector<SpatialNode*> dirtyLeaves`.

### 2. Partial locality map — `computeLocalityMapsPartial(dirtyLeaves, …)`
- Like `computeLocalityMaps` but only relocate `dirtyLeaves`' elements, into a
  contiguous run reclaimed from those leaves' current slots (so it's a closed
  permutation over just those slots — every element that moves, moves *within*
  the set of slots the dirty leaves already occupy). Everything else maps to
  itself.
- Represent the result as a **sparse map** (list of `{from,to}` moves), not a
  full capacity-sized array — this is what keeps undo memory + apply both O(moved).

### 3. Scoped attribute permutation — `AttrGroup::reorderScoped(moves)`
- Decompose the sparse map into **cycles**; rotate each cycle's attribute data in
  place with a one-element temp buffer. O(moved) copies, no full-array realloc.
- Must interoperate with mechanism A's page-bucketed free list (`ElemData`):
  the moved slots are all live (no free-slot churn), and `freemap` is unchanged
  by a pure permutation of live↔live slots, so the page buckets stay valid — but
  verify the `reorder`-rebuilds-free-structures assumption isn't relied on.

### 4. Scoped reference remap
- Only references *to moved elements* change. Two options, choose by Phase-0
  cost:
  - **(a) Bounded full scan** — keep the existing per-domain ref scan but it's a
    cheap `field = map[field]` (one lookup/write). At 5 M this is ~100-200 ms;
    may be acceptable as-is.
  - **(b) Topology-local** — for each moved vert walk its disk (incident edges)
    + radial (incident corners) and patch only those refs; O(moved × valence).
    Needs thawed topo (the compaction path already thaws in
    `computeLocalityMaps`). Only do this if (a) is the residual bottleneck.

### 5. Scoped node-cache remap + sparse undo chunk
- In `applyReorderIncremental`, only remap the caches of nodes whose elements
  moved (the dirty leaves), not all nodes.
- `LogChunkReorder` stores the **sparse** move list; `undo`/`redo` apply the
  inverse/forward scoped permutation. `padToCapacity` becomes a no-op for the
  sparse form (identity outside the move set is implicit). Undo memory drops from
  O(capacity) to O(moved) per step.

## Phases (diagnose-first, gated)

- **Phase 0 — measure the breakdown.** Instrument `applyReorderIncremental` to
  time: attribute permute, reference remap, node-cache remap (per domain). Run at
  150 k / 300 k / (built-up) 1 M via debug_app. Confirms attribute permute
  dominates and tells us whether ref/node scans need scoping. *Gate: a cost table
  pinning each term.*
- **Phase 1 — partial scope, full apply.** Region selection + partial map
  (sparse), but apply via the existing full `reorder_*` (so the map is partial
  but the apply is still O(mesh)). Proves region selection keeps locality good
  (frag stays low) and undo correct, before touching the hot apply path.
  *Gate: frag-ratio held ≈ full compaction; undo exact; cross-validate vs full.*
- **Phase 2 — scoped attribute permute** (`reorderScoped`, change #3) wired for
  the partial map. The main win. *Gate: 5 M apply time drops to ~the residual
  scan; result bit-identical to Phase 1.*
- **Phase 3 — scoped ref remap + node remap + sparse chunk** (changes #4, #5)
  only if Phase-0/2 show the residual scans matter. *Gate: sub-100 ms at 5 M;
  undo memory O(moved).*

## Correctness & verification

- A scoped+partial compaction does **not** reproduce the full compaction's layout
  (it moves a subset), so verification is *invariant*-based, not equality:
  geometry signature unchanged, `validateAndRepair` == 0, the touched region's
  frag ratio drops, `castRay` over a grid returns identical hits (extend
  `test_spatial_reorder_inc`).
- Undo/redo round-trip restores exact element count + geometry (extend
  `test_mesh_reorder::test_growth_reorder_undo` with partial+sparse chunks).
- Cross-validate `reorderScoped` against `AttrGroup::reorder` on the **same** map
  (they must agree element-for-element) — a pure unit test, no spatial needed.

## Risks

- **Cycle permutation vs page allocator (A).** In-place cycles touch raw attr
  pages; must not corrupt `freemap`/page buckets. Pure live↔live permutation
  shouldn't, but `ElemData::reorder` currently *rebuilds* free structures — the
  scoped path must preserve them correctly instead.
- **Reference remap correctness.** Topology-local remap (4b) is easy to get
  wrong (miss a radial/disk ref); the bounded full scan (4a) is safer — prefer it
  unless profiling forces 4b.
- **Sparse undo chunk + cross-step composition.** The full-map `padToCapacity`
  fix (`LogChunkReorder`) handles capacity growth between record and undo; the
  sparse form must preserve that property (moves reference absolute slot indices
  that are still valid after later growth, since growth only appends).
- **Partial-scope quality.** Compacting only the dab region could let global
  fragmentation creep over a long session if regions overlap oddly; the per-leaf
  score + occasional wider sweep (loop-until-dry style) is the backstop.

## Out of scope

- Changing *when* compaction fires (the `auto_defrag` threshold/trigger is
  settled).
- GPU-side layout (the win is CPU gather: brush iteration + buffer fill).

## References

- Mechanism A/B engine + the incremental apply: `source/spatial/spatial.cc`
  (`applyReorderIncremental`, `computeLocalityMaps`, `fragmentationStats`),
  `source/mesh/{mesh.cc,elem_data.h,attribute.h}` (`reorder_*`, `AttrGroup::reorder`).
- Undo chunk + capacity padding: `source/meshlog/meshlog_base.h`
  (`LogChunkReorder`, `compactIfFragmented`, `padToCapacity`).
- Trigger: `scripts/editors/view3d/tools/sculptcore_ops.ts`
  (`SculptPaintOp.finishStroke`), flag `sculptcore.auto_defrag`.
- Diagnostics: debug_app verbs `frag_stats`, `reorder`, `reorder_inc`,
  `time_gather`, `dyntopo_stats`, `auto_defrag`.
