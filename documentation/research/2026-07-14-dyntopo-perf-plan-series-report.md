# Dyntopo performance plan series — report (2026-07-12 → 2026-07-14)

Four measurement-gated plans, run back-to-back on branch
`dyntopo-disk-bandwidth` (worktree `webgl-app-framework-dyntopo-disk-bandwidth`),
targeting the CPU cost of a dynamic-topology sculpt dab. Every change was
accepted or rejected by same-session interleaved A/B on the perdab workloads
(480k / 805k tri), with bit-exact parity and the full undo battery as
correctness gates.

## Shipped

| change | where | measured win |
|---|---|---|
| **Side-bit-encoded disk links** — `(edge<<1)\|side`; the `e_of_v` iterator rides the embedded side, splices drop their `edge_side` loads; mesh format v4 + order-preserving v3→v4 migration | `mesh_types.h`, `mesh.h`, `mesh_iter.h`, serializer | bench ops −6.1%, perdab wall −5.6% |
| **Chunk-stamp no-op fast path** — dense per-domain generation stamps answer "already recorded by the active topo chunk" without the makeKey/hash lookup or redundant undo-gate re-stamp (65% of the 388k callback events/dab are no-ops) | `meshlog_base.h` (`ElemStampTier`) | perdab_big −3.5/−4.1%, neutral at 480k |
| **Shared meshlog row layouts** — `ChunkElemRow`s no longer recompute their byte layout per captured row; a chunk-owned `RowLayout` per (kind × attr-count) carries offsets/sizes, rows hold a pointer + payload bytes | `meshlog_base.h` (`RowLayout`) | **perdab −18%, perdab_big −16…−25%** |

Cumulative: perdab wall roughly **−25% at 480k** and **−30% at 805k** vs the
series start. Side deliverables: `save_mesh`/`load_mesh` debug verbs, the
`dyntopo_bench_single.txt` workload, side-bit checks in `validateAndRepair`/
`checkTopology`, and stale-comment/doc fixes.

## Rejected — with the reason each is closed

- **Per-vertex slab/CSR disk storage** (full implementation preserved on
  branch `dyntopo-disk-slab-rejected`): passed every correctness gate
  (exact 200-dab counter parity, −20% memory, `.wproj` round-trip) but
  measured **+4–6% slower** — `alloc_near` already keeps a vertex ring's
  int4 links in 1–2 cachelines at ≤1M scale, so the dependent chase is
  cache-resident, not latency-bound, and the slab's per-op costs
  (O(valence) removes, iterator setup) outweigh the sequential-walk gain.
- **Callback dispatch flattening / bulk hooks**: the fanned slots carry
  <3% of events and the whole dispatch pool measures ~5% of wall — every
  variant caps below the 3% noise threshold.
- **Cross-chunk capture dedup** (`Existed && Live` re-records): correct
  final-state algebra, but each chunk's undo/redo runs spatial-tree passes
  that **walk corner loops mid-replay** — deduped elements leave mixed-era
  links and the walks hang. Per-chunk record completeness is load-bearing;
  revival requires moving the tree passes to step scope first.
- **Kill-capture batching / capture column subsets**: under the 3% bar
  post-row-layouts / unmeasurable on the gate workloads (no color/mask/layer
  payload to drop). The partial-gate design (stamp only CO|NO, leave the
  rest to the element store) is sketched in the capture plan's log for when
  a painted/layered benchmark exists.

## Methodology findings (why the series kept its footing)

- **Per-event timers lie 2–3× at 50–700 ns granularity** — the "callbacks
  are 36–38% of ops" figure that motivated plan 3 was ~2× inflated by the
  profiler's own wrapper timers. Env-toggled **ablation stub legs in one
  binary**, interleaved same-session, replaced them and immediately found
  the real hotspot (per-row layout recompute ≈ 24% of wall) that sampling
  had mis-attributed.
- Pre-registered keep/revert rules (≥3% beyond noise; ship bars per rung)
  made the two big reverts (slab, dedup) mechanical rather than judgment
  calls, and both rejections are documented against re-trying.

## Where the remaining time is

After the series, a dab's costs are dominated by genuine work: split/collapse
topological surgery, the candidate scan's length math, and the first-touch
row copies undo actually needs. No known pure-overhead lever remains at
these mesh scales.

## Plans (all COMPLETE, with full measurement logs)

- `plans/2026-07-12-1324-dyntopo-disk-bandwidth.md`
- `plans/2026-07-12-2141-meshlog-callback-batching.md`
- `plans/2026-07-13-2046-meshlog-capture-cost.md`
- design doc `plans/2026-07-12-1520-dyntopo-disk-slab-design.md` (G3-approved,
  rejected at its A/B gate)

Branch state: sculptcore `194f91f` / parent `f9e05c8d`, both pushed; not yet
merged to master.
