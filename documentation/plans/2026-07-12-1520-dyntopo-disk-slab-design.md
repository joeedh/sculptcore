# Dyntopo disk-slab design (plan M3 of 2026-07-12-1324-dyntopo-disk-bandwidth)

Status: **awaiting G3 review** — no code until approved.

Replace the per-edge `e.disk` int4 cycle links with per-vertex incident-edge
slabs: `v.e` becomes `(offset, count)` into a pooled power-of-two arena, a
walk becomes a sequential 4 B/step scan, and the edge-domain disk column
disappears entirely. M1's side-bit encoding is *reused* as the slab entry
format, so the walk still needs no `e.vs` load.

Entry condition (measured, post-M1 perdab shares): pure `e_of_v` walk ≈ 22%
of `ops` (scan_walk 19.1% + flip_collect 2.8% + guards 0.4%), splice-bearing
self buckets ≈ 29%. Well above the 15–20% bar.

## 1. Representation

- **Arena**: one `Vector<int32>` pool per mesh with power-of-two size-class
  free lists (a slab allocator; classes 2,4,8,…). Growth reallocates the
  vertex's block into the next class and frees the old block to its class
  list. Freed blocks are recycled; the arena itself only grows (compaction is
  a later, optional pass — offsets are private to `v.disk`, so it is safe).
- **Entry**: `diskPack(edge, side)` — identical to M1's link encoding. A walk
  reads `arena[off + i]`, decodes `(e, side)`, and `e.vs[e][side ^ 1]` stays
  a leaf/payload load.
- **Per-vertex column**: `.vert.disk` = int2 `(offset, class<<28 | count)`
  (TOPO; replaces `.vert.e`). Head = slot 0. `v.e`-style reads become a
  helper (`first_edge(v)` = `diskEdge(arena[off])`, `ELEM_NONE` when
  count = 0); `EdgeOfVertIter` keeps its interface but iterates `i < count`.
- **`e.disk` (16 B/edge) is deleted.** `edge_side()` survives (splices and
  payload reads still use `e.vs`).

## 2. Mutation semantics (sequence-preserving by construction)

- `disk_insert(e, v)`: append `diskPack(e, side)` at `off + count`; grow to
  the next class first when `count == cap`. Appending at the end is exactly
  today's insert-at-cycle-tail.
- `disk_remove(e, v)`: linear-scan the slab for the entry (valence ≈ 6 → one
  cacheline), then `memmove` the tail down one slot. Order of survivors is
  preserved; removing slot 0 promotes slot 1 to head — exactly today's
  head-removal advancing `v.e` to `next`. Cost: 1–2 sequential lines vs
  today's ~3 random dirty lines (prev/next int4 RMWs).
- Collapse ring-merge: batch append into the survivor's slab (order =
  today's sequence of disk_inserts).
- **Invariant** (already true today): no `EdgeOfVertIter` walk may span a
  splice of the same vertex. Slab growth additionally invalidates that
  vertex's offset, same rule covers it. The dyntopo flip sweep already
  collects-then-applies for this reason.

## 3. Bit-compat audit — everything that establishes disk order

| site | today | under slab | sequence |
|---|---|---|---|
| `disk_insert` | append at cycle tail | append at slab end | identical |
| `disk_remove` | unlink, head→next | shift-remove, slot1→head | identical |
| `validateAndRepair` pass 5 | re-`disk_insert` every edge in edge-index order | same loop, slab appends | identical |
| `thawTopo` / `FrozenTopo::rebuildLinks` | rewrites cycles from `vert_edges` CSR (captured in walk order) | writes slabs from the same CSR — or see §6 (the slab *is* the CSR; freeze/thaw for v↔e dissolves) | identical |
| `reorder_edges` | remap ids inside int4s + patch back-links | remap ids inside arena entries in place | identical (order untouched) |
| `reorder_verts` / `VertexData::swap_elems` | moves `v.e` cells; disk untouched | moves `(off,count)` cells; arena untouched | identical |
| mesh load | disk columns restored verbatim | slabs rebuilt by per-vertex appends in file order (§5) | identical |
| meshlog undo/redo | disk cells restored verbatim per element row | slab spans replayed per touched vert (§4) | identical |

Key liberation: **arena offsets are not identity** — nothing outside
`v.disk` references them. Only the per-vertex *edge sequence* must be
preserved. Undo, serialization, and defrag may all re-allocate blocks freely
as long as they replay sequences in order.

## 4. Meshlog — the variable-width path

Today both `.vert.e` (4 B) and `.edge.vs.disk` (16 B) ride the generic
fixed-width `ChunkElemRow` snapshots. Under the slab, a vert's connectivity
is variable-width and the row cell `(off, count)` is meaningless across
undo (offsets are not stable identity).

Design — a per-topo-chunk **slab span log** beside the existing rows,
following the `LogChunkTypes::External` pattern (opaque payload, replayed by
the chunk's undo/redo virtuals), but owned by `LogChunkTopo` since it must
interleave with row swaps:

- New `AttrFlag::TOPO_EXTERNAL` on `.vert.disk`: `ChunkElemRow`
  capture/restore skips the column (like NOCOPY), the span log owns it.
- Capture: on a vert's **first** disk change inside the chunk (the existing
  `onVertChange` fires from every splice caller), append
  `(origIndex, begin_count, begin_entries…)` to a chunk-local CSR
  (`Vector<int>` blob + `(vert, blobOff, count)` index). At chunk seal,
  capture the end sequences the same way (mirrors begin/end body semantics
  of Created/Existed rows).
- Undo: after the row swaps, for each logged vert (reverse order) free its
  current block and rebuild from the begin sequence (fresh allocation, same
  order). Redo: replay end sequences. Verts created inside the chunk get
  their slab dropped on undo (they die anyway); verts killed get their slab
  rebuilt on undo from the begin sequence.
- Memory: ~4 B × valence × 2 per touched vert per chunk — comparable to the
  16 B/edge disk rows it replaces (each touched edge today logs 2×16 B disk
  cells across its two endpoint rows' edges).

This is the plan's "largest hidden cost" item; it is contained to
`meshlog_base.h` (`ChunkElemRow` skip-flag + `LogChunkTopo` span log) and
does not disturb the Elems store or Reorder chunks (reorder remaps ids, not
offsets; the span log stores ids, so `LogChunkReorder` must remap the span
log's edge ids too — same hook where it remaps row TOPO cells today; if it
does not remap row cells (rows are position-keyed), then no change).
**Verify during M4 stage 3**: exactly what LogChunkReorder rewrites.

## 5. mesh_serialize — format v5

- EDGE domain: `.edge.vs.disk` column no longer exists (writer stops
  emitting; `topoTarget` entry dropped).
- VERTEX domain: `.vert.disk` is flagged TEMP-like for the writer (never
  raw-dumped). Instead v5 appends a trailing **vert-disk stream** section
  (after the sculpt-layer table): `uint32 totalLen` + per live vert, in
  dense vert order, `uint32 count` + `count × int32` edge ids (remapped
  through emap, walk order, *unencoded* ids — sides are recomputed on load
  from `.edge.vs`, keeping the file format encoding-agnostic).
- Load: after domains are built, walk the stream and `disk_insert` per vert
  in order (or write slabs directly) — order-preserving.
- Migration v4→v5 in `migrate()`: walk each v4 disk cycle (in the
  SerialMesh's own columns, as the v3→v4 case does) and synthesize the
  stream; drop the disk column. v3 files chain through the existing case.
- Size: 4 B count + 4 B × valence per vert ≈ 28 B/vert vs today's 48 B/vert
  of disk cells + 4 B `v.e` — files shrink too.

## 6. Freeze/thaw convergence (scope decision for G3)

`FrozenTopo.vert_edges` *is* this slab, immutable. Two options:

- **v1 (proposed)**: keep freeze/thaw as-is; `freeTopoPages` drops
  `.vert.disk` + the arena, `FrozenTopo::build` captures the CSR from slab
  walks, `rebuildLinks` rebuilds slabs from it. Smallest diff; the slab is
  still 2× captured while frozen.
- v2 (follow-up): the slab replaces `FrozenTopo.vert_edges` outright —
  freeze just stops mutating it; thaw is a no-op for v↔e. Dissolves the
  dichotomy (the tangent doc's carrot) but couples the arena to frozen-mode
  RAM accounting. Not in the first cut.

## 7. Memory budget (tri mesh, E ≈ 3V)

| | today | slab |
|---|---|---|
| `e.disk` | 48 B/vert (16 B/edge) | — |
| `v.e` / `.vert.disk` | 4 B/vert | 8 B/vert |
| slab entries | — | 24 B/vert (2E × 4 B) |
| pow2 slack (~33% at val 6→8) | — | ~8 B/vert |
| arena freelists / headers | — | ~1 B/vert amortized |
| **total** | **52 B/vert** | **~41 B/vert (−21%)** |

Plus the meshlog per-chunk deltas shrink (§4) and files shrink (§5).

## 8. Expected win / ship bar

Targets scan_walk (19%) + flip_collect (3%) — dependent ~50 ns/step chases
becoming prefetchable sequential scans (expect ≥2×, i.e. ~10% of ops) — and
the splice self-time inside split/collapse/flip_apply (~29% bucket share;
splices go from ~3 random dirty lines to 1–2 sequential ones). Plan M4 ship
bar: **≥ ~10% ops_ms** on the perdab workloads with memory within the table
above; parity harness (M4 stage 1) proves byte-identical `e_of_v` sequences
before the core switch.

## 9. Optional follow-up (separate A/B, not in the first cut)

Parallel neighbor-vert slab: store `(nbrVert)` beside each entry (8 B/step)
so smooth/collapse ring gathers never touch `e.vs` at all — mutable ring1
CSR; also dissolves `ensureRing1`. Only worth it if post-M4 profiling still
shows the `vs[e][side^1]` payload loads hot.

## 10. Open questions for G3

1. §6 scope: agree v1 (keep freeze/thaw) for the first cut?
2. §5 stream format: unencoded ids + recomputed sides on load (proposed) vs
   storing packed entries verbatim (faster load, format leaks the encoding)?
3. `.vert.disk` packing: `(offset, class<<28|count)` int2 (proposed) vs
   three plain ints (simpler, +4 B/vert)?
4. Arena compaction: out of scope for M4 (proposed) — acceptable that a long
   session's arena high-water mark persists until save/load?
