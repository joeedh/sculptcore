# Scoped / partial compaction for mechanism-B DRAM defrag

## Status

Phase 0 (cost-breakdown profiling) DONE — and it reprioritized the work; Phases 1+
not started. Mechanism B (incremental DRAM compaction) is landed and `auto_defrag`
is now **default-on**. The remaining work is making a stroke-boundary compaction
**O(stroke region)** instead of **O(whole mesh)**, so it stays cheap up to the
5 M-element target.

### Phase 0 results (debug_app `reorder_inc` + `SCULPTCORE_REORDER_PROFILE=1`)

`applyReorderIncremental` cost split (env-gated timing in spatial.cc / elem_data.h),
on dyntopo-churned meshes:

| verts | total | attr_permute | ref_scan | node_remap | free_rebuild |
|---|---|---|---|---|---|
| 82 k  | 114 ms  | 70 (61%) | 24 (21%) | 17 (15%) | 4 |
| 298 k | 733 ms  | 403 (55%) | 181 (25%) | 123 (17%) | 26 |
| 450 k | 2647 ms | 688 (26%) | 160 (6%) | **1500 (57%)** | 299 (11%) |

**Key finding (overturns an assumption): `node_remap` is the worst-scaling term.**
It went 17 → 123 → 1500 ms (a 12× jump for a 1.5× vert increase) and *overtakes*
attr_permute at scale. Cause: the per-leaf OrderedSet rebuild (`unique_verts`/
`unique_faces`) allocates ~one-fresh-set-per-leaf (~1750 at 450 k); the allocation
churn explodes near the RAM ceiling (the next size up OOM'd). `free_rebuild`
(`rebuild_free_structures`) shows the same allocation-churn signature (26 → 299 ms).

`attr_permute` is still big (the `reorder_corners` array rewrite dominates `reorder_X`,
since corners carry the most attribute data), and `ref_scan` is 6–25%.

**Reprioritization:** scoping `node_remap` (only relabel the *touched* leaves' caches,
in place — no fresh OrderedSet alloc) is now **co-priority #1** with the scoped
attribute permute, not the deferred Phase 3 change #5. All three of node_remap,
attr_permute, ref_scan must be scoped to reach O(region); none is negligible at 5 M.
(Caveat: the 450 k node_remap=1500 ms is inflated by memory pressure; the *true*
algorithmic cost is lower, but the allocation churn it measures is real and is exactly
what in-place scoped remap eliminates.)

### Phase 1 results — allocation-free in-place apply (DONE)

Two changes, both eliminating per-call allocation churn (no region scoping yet —
this just makes the *full* apply cheap):

1. **`OrderedSet::remap(fn)`** (new litestl method, `util/ordered_set.h`): rewrites
   stored values in place (reusing `idx_to_val_` + clearing/refilling `val_to_idx_`,
   whose capacity is retained), instead of building a fresh `OrderedSet` per leaf and
   move-assigning. `Map::clear()` made public for this. The node-cache loop in
   `applyReorderIncremental` now calls `d.unique_verts.remap(...)` / `unique_faces.remap(...)`.
2. **`rebuild_free_structures`** (`mesh/elem_data.h`): clears each page bucket in
   place + grows/shrinks the outer vector to `npages`, instead of `page_free.clear()`
   (destroying every inner `Vector`) followed by `ensure_page_buckets()` re-appending
   fresh ones.

Measured drop (debug_app, same churned meshes):

| verts | node_remap | free_rebuild | total |
|---|---|---|---|
| 298 k | 123 → 43 ms (−65%) | 35 → 18 ms | 733 → ~640 ms |
| 450 k | 1500 → 106 ms (−93%) | 335 → 35 ms (−89%) | 2647 → ~1450 ms (−45%) |

(Runs near the RAM ceiling are noisy ±; the directional drop is robust across runs.)
Correctness: `test_elem_alloc`, `test_spatial_reorder_inc`, `test_mesh_reorder`,
`test_spatial_dyntopo`, `test_dyntopo_cascade`, `test_dyntopo_budget` all pass.
Remaining cost is `attr_permute` (~58%) + `ref_scan` (~21%) — the genuine O(mesh)
array rewrite + cross-domain reference scan that only region-scoping (Phase 1b/2/3)
can shrink.

### Phase 1b results — partial scope, full apply (DONE)

New: `SpatialTree::selectFragmentedLeaves(ratioThreshold, out)` (region selection,
scored on **faces** — see metric note below) + `computeLocalityMapsPartial(dirtyLeaves,
…, movedCounts)` (a closed permutation over only the dirty leaves' slots; mostly-
identity full bijection). Applied via the existing full `applyReorderIncremental`.
Debug verb `reorder_partial thresh=R` prints dirty-leaf + moved-element counts.

1. **Correctness ≈ full (whole-mesh churn).** When the stroke churned the whole
   surface (88% of leaves dirty), the partial closed permutation matched full
   compaction: vert ratio 13.58 → **2.495** (full: 2.496), face 21.29 → **1.19**
   (full: 1.10). The partial map is a correct, full-quality compaction.
2. **Scoping proven (localized stroke on a compacted mesh).** Baseline (full
   `reorder_inc`) face ratio 1.087; an 8-dab corner stroke bumped it to 1.178;
   `reorder_partial` selected **31/1398 leaves (2.2%)**, moved **5449/235593 verts
   (2.3%)** — genuinely mostly-identity — built the map in **25 ms** (vs 622 ms full)
   and restored face ratio to **1.095** (≈ baseline). The apply was still 210 ms
   (full path — that is Phase 2's target), but the map now has the mostly-identity
   structure Phase 2 needs.
3. **Metric note (important):** region selection scores on **face** page-spread, not
   vert. Faces are owned by exactly one leaf, so their ratio cleanly measures
   fragmentation; verts are shared across leaves (boundary verts), giving the vert
   ratio an irreducible sharing floor (~2.5 here) that over-selects and is noisy.
   Also: baseline with `reorder_inc` (incremental, keeps the leaf set), NOT `reorder`
   (rebuild path) — rebuild re-partitions leaves and scrambles the compaction↔leaf
   alignment, leaving an inflated ratio that masks the signal.
4. **Correctness gate (test).** `test_spatial_reorder_inc` gained
   `test_partial_matches_full`: an explicit-subset partial map applied via both the
   trusted full-rebuild and the incremental path — asserts valid bijection, geometry
   preserved, both paths agree, castRay invariant, genuinely partial (moved < live),
   and an **inverse round-trip restores the prior layout (the undo contract)**. Passes
   across N∈{8,16,32}, leaf∈{16,64}.

### Phase 2 results — scoped attribute permute (DONE)

`AttrGroup::reorderScoped(elem_map, movedSlots)`: applies the permutation as in-place
**cycle rotations** over only the moved slots (one element temp per cycle, cycles
decomposed once and shared by all attribute columns), instead of allocating a full-size
scratch and rewriting the whole arrays. `ElemData::reorderScoped` wraps it and — keying
on the fact that the partial map is a **closed permutation over LIVE slots** — skips the
O(capacity) freemap permute + `rebuild_free_structures` entirely (the freemap is invariant).
Threaded through optional `moved` spans on `Mesh::reorder_*` and `applyReorderIncremental`
(the cross-domain reference fix-up + node-cache remap stay full — Phase 3).
`computeLocalityMapsPartial` now also emits the per-domain moved-slot lists; the
`reorder_partial` verb gained `scoped=0|1` (default 1).

Measured on the localized stroke (235 k mesh, 2.2% of leaves dirty), `reorder_partial`:

| term | full apply | scoped apply |
|---|---|---|
| attr_permute | 83.4 ms | **6.9 ms (−92%)** |
| free_rebuild | 10.1 ms | **0.0 ms (skipped)** |
| ref_scan | 48.8 ms | 36.0 ms (still full) |
| node_remap | 16.0 ms | 13.0 ms (still full) |
| **apply total** | **158 ms** | **56 ms (−65%)** |

Identical frag result (face 1.178 → 1.095 both). Bit-identical to the full path:
`test_partial_matches_full` now applies tree B via the **scoped** path and asserts it
equals the trusted full-rebuild (geometry + castRay) and that the scoped inverse restores
the prior layout (undo). At 5 M the full attr permute would be seconds; scoped it is
O(region). The residual 56 ms is now **ref_scan (36) + node_remap (13)** — both O(mesh),
Phase 3's target.

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

### 4. Scoped reference remap  ← THE residual after Phase 2 (ref_scan ~64% of apply)
- Only references *to moved elements* change. Two options:
  - **(a) Bounded full scan** — keep the existing per-domain ref scan, a cheap
    `field = map[field]`. This is what Phases 1b/2 still do. ~36 ms at 235 k →
    ~0.8 s at 5 M, so it IS the residual bottleneck and must be scoped.
  - **(b) Moved-set-local** — patch only refs into moved elements. *Refined design
    (Phase-2 analysis):*
    - **Prerequisite — interior-only selection.** `computeLocalityMapsPartial`
      must move only elements *exclusively* inside dirty leaves (face-anchored:
      move face f iff its leaf is dirty; corner/list iff its face is dirty; edge
      iff BOTH its faces are dirty; vert iff ALL its faces are dirty). Then every
      reference into a moved element comes from a moved element or a *boundary*
      (non-moved) element of a dirty leaf — never a clean leaf — so the fix-up is
      complete from the moved sets alone. (The current reach-everything selection
      moves shared boundary verts, whose clean-leaf references would be missed.)
    - **Single-target refs** (`e.vs`, `c.v`, `v.e`, `c.e`, `l.c`, `c.l`, `l.f`,
      `e.c`): iterate the moved set that *owns* the ref and do `field = map[field]`
      (identity-safe). E.g. `for e in emoved: e.vs[e]=vmap[e.vs[e]]`.
    - **Cyclic links** (`e.disk`, `c.next/prev`, `c.radial_next/prev`, `l.next`):
      a moved element's cycle neighbors may be non-moved boundary elements whose
      back-links point at it. **Aliasing hazard:** doing in-place "search neighbor
      for old-index, overwrite" mixes old/new indices and can double-remap, since
      new indices reuse old moved slots. **Fix — collect-then-apply:** in one pass
      read all originals and append `{address, newValue}` writes (own links via
      `map`; non-moved neighbors' back-links located from the moved element's own
      original links), then apply all writes. Reads precede writes ⇒ no aliasing.
    - Gate every domain on `test_partial_matches_full` (scoped vs trusted full
      rebuild + inverse round-trip); every-other-leaf subsets maximize boundary
      coverage.

### 5. Scoped node-cache remap + sparse undo chunk
- In `applyReorderIncremental`, only remap the caches of nodes whose elements
  moved. **Depends on the interior-only selection (change #4 prerequisite):** with
  interior moves, only the dirty leaves' caches change, so the remap loop iterates
  `dirtyLeaves` instead of all nodes. (With reach-everything selection a moved
  boundary vert is cached in clean leaves too, so this would be incomplete.)
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
- **Phase 1 — allocation-free in-place apply (DONE).** Phase 0 reprioritized this
  ahead of the partial map: the two worst terms (node_remap, free_rebuild) were
  pure allocation churn, fixable independent of region scoping. `OrderedSet::remap`
  (new litestl method) rewrites each leaf's `unique_verts`/`unique_faces` in place
  (reusing storage, no fresh per-leaf set); `rebuild_free_structures` clears the
  page buckets in place instead of destroy+realloc. *Gate met: node_remap 1500→106 ms
  (−93%), free_rebuild 335→35 ms (−89%) at 450 k; total −45%; all reorder/dyntopo/
  alloc tests pass. See "Phase 1 results" above.* Remaining cost is now attr_permute
  (~58%) + ref_scan (~21%) — the genuine O(mesh) work the partial map must scope.
- **Phase 1b — partial scope, full apply (DONE).** `selectFragmentedLeaves`
  (face-scored region selection) + `computeLocalityMapsPartial` (closed permutation
  over the dirty leaves' slots, mostly-identity), applied via the existing full
  `reorder_*`. *Gate met: localized stroke → 2.2% of leaves selected, 2.3% of verts
  moved, face frag restored to ≈ baseline; whole-mesh case matches full compaction
  (vert 2.495 vs 2.496); `test_partial_matches_full` proves valid bijection, full↔
  incremental agreement, and exact inverse round-trip (undo). See "Phase 1b results".*
  The map is now mostly-identity but the **apply is still O(mesh)** — Phase 2's target.
- **Phase 2 — scoped attribute permute (DONE)** (`AttrGroup::reorderScoped`,
  change #3) wired through optional `moved` spans on `Mesh::reorder_*` /
  `applyReorderIncremental`. In-place cycle rotations over the moved set; skips the
  freemap rebuild (closed permutation over live slots). *Gate met: attr_permute
  83→6.9 ms (−92%), free_rebuild→0, apply 158→56 ms; bit-identical to the full path
  (`test_partial_matches_full` now drives the scoped apply). See "Phase 2 results".*
  Residual is now ref_scan + node_remap (still O(mesh)) — Phase 3.
- **Phase 3a — interior selection + scoped node-cache remap (DONE).**
  `computeLocalityMapsPartial` now moves only interior elements (face-anchored:
  edge iff both faces dirty, vert iff all faces dirty — via `CornerOfEdgeIter` /
  `EdgeOfVertIter`). `applyReorderIncremental` derives the affected leaves from the
  moved faces' ownership (pre-reorder `f.node`) and remaps only those caches.
  *Result: node_remap 13 → 0.45 ms (−97%); face frag still 1.095; test green.* Ref
  fix-up still full (ref_scan ~56 ms now dominates) → Phase 3b.
- **Phase 3b — scoped reference fix-up (DONE)** (change #4). `Mesh::reorder_*` take
  a `ReorderMoved` (the 5 moved sets + `active`); when active they patch only refs
  into the moved sets. Single-target refs iterate the owning moved set; `e.disk` /
  `c.radial` patch non-moved neighbors' back-links with **structural** slot
  computation (read neighbors before remapping own — no value-search, so no
  aliasing); `c.next/prev` / `l.next` neighbors are always moved (own-remap only).
  Stale-index care: `reorder_lists` reaches `c.l` at `cmap[c1]`, `reorder_faces`
  reaches `l.f` at `lmap[l1]` (those domains already permuted). *Bug found+fixed by
  the test: `CornerOfEdgeIter` stops one corner short, so the interior-selection
  radial walks must be explicit do-while. Result: ref_scan 56 → 3.1 ms (−94%), apply
  67 → 8.75 ms; bit-identical to full rebuild + inverse round-trip (test green).*
- **Phase 3d — app-path integration (DONE).** `MeshLog::compactIfFragmented` (the
  stroke-end auto-compaction the brush path calls) now uses the scoped path:
  `selectFragmentedLeaves` → `computeLocalityMapsPartial` → scoped
  `applyReorderIncremental`. `LogChunkReorder` stores the moved sets + a `scoped`
  flag and replays scoped on undo/redo (the moved slot-set is permutation-invariant,
  so the same sets drive the inverse). *Bug found+fixed via the save_pos/undo/
  assert_pos fidelity check on a dyntopo mesh (the grid test missed it): the scoped
  node-cache remap derived affected leaves from moved-face ownership only, but a leaf
  can cache a vert via `v.node` assignment with no owned face — so it must also pull
  owners of moved verts. After the fix, scoped auto_defrag undo is exact (moved=0 at
  eps 1e-3; the residual ~5e-4 is dyntopo FP rounding, same as the full path).*
- **Phase 3c — sparse undo chunk (DONE).** `LogChunkReorder` for a scoped compaction
  now stores only the moved slot lists + their target values (`mv`/`vval` per domain,
  O(moved)) instead of the full capacity-sized maps. undo/redo reconstruct the full
  bijection transiently (identity + the moves) and replay via the **full**
  `applyReorderIncremental` (undo/redo are rare; the forward already paid O(region)).
  *Undo exact (moved=0 @1e-3); chunk memory drops from O(capacity) (5×cap ints) to
  O(moved) (~10×region ints) — ~20× for the 235 k test mesh, growing with mesh size.*
  Note: the apply-side maps were left full (the build is region-dominated; not worth
  the MapView churn).
- **Phase 3c.1 — redo-hang fix (DONE).** undo→**redo** across a compacted stroke
  hung (tight infinite loop in `add_face_at`'s corner-list walk, via
  `LogChunkTopo::redo`). Root cause: the stroke's last topo chunk captured its
  `end_body` — which includes TOPO connectivity attrs — at `endStep`, *after*
  `compactIfFragmented` had permuted the live mesh, so the chunk held
  **post-reorder** corner indices. redo replays topo chunks *before* `reorder.redo`
  (into the pre-reorder layout), so it wrote post-reorder indices into a pre-reorder
  mesh → corrupt corner cycle → non-terminating loop. (Undo was unaffected:
  `reorder.undo` runs first, and `release` never reads `end_body` connectivity —
  which is why only redo broke.) Fix: extract `MeshLog::finalizeStroke()` (the
  topo-chunk finalize + Created-data refresh formerly inlined in `endStep`) behind a
  per-step `finalized` guard, and call it from `compactIfFragmented` **before**
  applying the reorder, so every topo chunk freezes pre-reorder connectivity;
  `endStep` then no-ops the second finalize. *Verified: no hang; undo AND redo
  bit-exact (`moved=0, worst=0`) across single-dab and multi-dab (`repeat=4`,
  per-dab topo chunks) compacted strokes, repeated A↔B toggles, and double-undo to
  the pre-stroke state. Full reorder/dyntopo/undo ctest suite green.*
  Phase-2 confirms the residual
  (ref_scan ~36 ms + node_remap ~13 ms at 235 k → ~1 s at 5 M) does matter, so this
  is required for the 5 M target. It is the highest-risk phase — it rewrites
  topology references, where a missed/aliased fix corrupts the mesh — so it needs
  the interior-only selection prerequisite + the aliasing-safe collect-then-apply
  pattern, implemented one domain at a time, each gated on `test_partial_matches_full`.
  *Gate: sub-100 ms at 5 M; result bit-identical to Phase 2; undo memory O(moved).*

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
