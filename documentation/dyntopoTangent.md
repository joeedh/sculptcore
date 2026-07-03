# Dyntopo topology-bandwidth tangent — disk links, corner links, valence

Notes from a design discussion (2026-07-03) about compressing the mesh's
topology link columns to speed up dyntopo. No code changes; this records the
analysis, the one parity-safe migration insight, and the recommended probe
order for whenever this is picked up.

## Corner next/prev → sentinel bits (evaluated, not recommended for dyntopo)

Replacing `.corner.next` / `.corner.prev` (8 B/corner) with two packed
start/end-of-loop sentinel bits (0.25 B/corner) under an "always contiguous
per face" layout:

- **Storage**: ~97% of those two columns; ~14% of total core mesh bytes on a
  tri mesh (~46 MB per 1M verts, ~230 MB at 5M). Serialization + meshlog topo
  snapshots shrink the same way.
- **Bandwidth**: attribute storage is SoA, so nothing else densifies. The win
  is bounded by the link stream's share of corner-walking loops: ~5–15%
  (recalc_normals, triangulate, serialize). The sculpt brush loop sees
  **zero** — `freezeTopo` already drops these columns and `for_neighbor`
  runs off the ring1 CSR.
- **The tax**: contiguity becomes a hard invariant. Tolerable for triangles
  (allocate corner triples; dyntopo kills/creates whole faces), but n-gon
  edits (box modeling, loop cut, CC refiner output) must block-shift every
  corner column of the face AND re-point `e.c`, both radial cycles of every
  moved corner, and `l.c` — corner ids stop being stable. That's the O(1)
  splice the explicit links buy.
- Two bits (not one) let both directions resolve locally by word-level
  bit-scan (faces ≤ 64 corners resolve within one u64), no `l.c` hop.

Verdict: real as a file-size/peak-memory lever; wrong lever for dyntopo.

## Edge disk links — the ladder

`e.disk` is int4 (prev/next per side, 16 B/edge). An `e_of_v` step reads
`e.vs` (8 B) just to pick the side, then one link (pulls a 16 B line):
~12–24 B per step, all dependent loads (latency-bound chase).

1. **Side-bit embed (free, do anytime)**: steal bit 0 of each link —
   `next = link >> 1, side = link & 1`. Removes the `e.vs` read + compare
   from the direction logic; `vs` is then read only for the payload, and
   exactly the needed half (`vs[e][side^1]`). Local to mesh_iter.h/mesh.cc.
2. **Drop prev (2×)**: only `disk_remove` needs it; find prev by walking the
   cycle (O(valence) ≈ 6) on a kill path already doing radial surgery.
3. **Relative int16 links (4×)**: `alloc_near` + locality reorder keep
   neighbors close in id space; int16 deltas with a sentinel escaping to an
   overflow map. Thinner stream, still a pointer chase.
4. **Slack CSR / per-vertex slabs (the real change)**: `v.e` → (offset,
   count) into pooled power-of-two slabs; insert = append, walk = sequential
   4 B/step scan (prefetchable, no dependent loads). Optionally a parallel
   neighbor-vert slab so smooth kernels never touch `e.vs` at all (this is
   ring1 CSR made mutable — it also dissolves the freeze/thaw dichotomy for
   v↔e adjacency).

## Why dyntopo specifically — and the parity-safe insight

Dyntopo is triangle-only, hammers disk *mutation* as hard as walking, and its
prior perf pass rejected "big levers" precisely because they changed
iteration order (parity/determinism). Per op:

- **Flip criterion (B-K valence rule)**: 4 valences per candidate edge =
  4 full disk walks (~24 dependent 16 B loads) today, per candidate, per
  round — and it only needs a *count*.
- **Unlink** (`disk_remove`): RMWs the prev and next edges' int4s — ~3 random
  dirty cachelines. A slab shift-remove touches 1–2 sequential lines: equal
  or better.
- **Collapse** ring-merge becomes a batch append into the survivor's slab.

**The key insight: the slab can be bit-compatible.** `disk_insert` appends at
the cycle tail; removal preserves relative order with the head advancing to
next. A slab with append-at-tail + shift-remove (memmove ≤ a cacheline at
valence 6) reproduces the *identical* `e_of_v` sequence, head-removal case
included. So the representation change need not alter any iteration-order-
dependent result — the determinism blocker that killed previous reordering
levers does not apply.

**What it won't move**: attribute interpolation, meshlog callbacks, spatial
tree (~22%). The old profile's "apply/flip ~87%" bundles attr interp and
callbacks with the topo splices — the disk share inside it is unmeasured.

## Recommended order

1. **Valence attribute probe** (u8/u16 per vert, ±1 in
   `disk_insert`/`disk_remove`): no structural change, directly captures the
   flip-criterion win. If `bench_dyntopo` moves, the disk structure is
   load-bearing and step 3 is justified.
2. **Measure the disk share** inside apply/flip with throwaway
   `--profile`-gated counters (the established scaffolding pattern; rip out
   after).
3. **Slab migration** with the order-preserving semantics above. The real
   engineering is meshlog (fixed-width TOPO column assumption — the
   `LogChunkTypes::External` chunk seam is the right hook) and
   `mesh_serialize`'s hand-maintained `topoTarget` map, not the mesh code.

Adjacent lever (tri-only): manifold triangle edges have exactly two radial
corners — `radial_next`/`radial_prev` (8 B/corner) could collapse to one 4 B
"mate corner" (boundary = self). Same meshlog/serialize caveats.
