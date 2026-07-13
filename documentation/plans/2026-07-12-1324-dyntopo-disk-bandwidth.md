# Dyntopo disk-link memory-bandwidth plan (2026-07-12)

Goal: improve the memory-bandwidth efficiency of dyntopo's hot topology
paths — chiefly the `e.disk` cycle walks and splices — following the
(corrected) analysis in [`../dyntopoTangent.md`](../dyntopoTangent.md).
Every milestone is gated on measurement: no representation change ships
without an A/B showing it pays, and no expensive milestone starts before the
cheap measurements prove the disk structure is load-bearing.

Status: **COMPLETE** (2026-07-12). M1 shipped (−6% ops); M2 skipped by its
own entry condition; M4 slab built, measured, and reverted at its gate
(preserved on `dyntopo-disk-slab-rejected`); scaffolding ripped. See the
progress log and the resolution banner in `dyntopoTangent.md`.

## Background (one paragraph)

`e.disk` is int4 (prev/next per side, 16 B/edge); an `e_of_v` step loads
`e.vs` (8 B) to pick the side, then one link — all dependent loads, a
latency-bound chase (`EdgeOfVertIter::operator++`, `mesh_iter.h`). Dyntopo
hammers both walking (~nine `EdgeOfVertIter` sites in `dyntopo.h`: collapse
guards, boundary checks, `smoothTangent` ring gathers) and mutation
(`disk_insert`/`disk_remove`, `mesh.h`). The shipped flip criterion is
length+convexity (`flipShortens`) — positions + radial walks, *not* valence
counts. The parity-safe key: a slab with append-at-tail + order-preserving
shift-remove reproduces the *identical* `e_of_v` sequence as today's cycle
splices, so the determinism blocker that killed earlier reordering levers
does not apply. The old profile's "apply/flip ~87%" bundles attr interp and
meshlog callbacks with the topo splices — the disk share inside it is
**unmeasured**. Measuring it is M0.

## Measurement protocol (used by every milestone)

- **Workloads** (existing scripts, `debug_app --script ...`):
  - `tests/scripts/dyntopo_profile_perdab.txt` — 480k-tri cube, 200 draw
    dabs (the standard per-dab workload).
  - `tests/scripts/dyntopo_profile_perdab_big.txt` — the large-mesh variant.
  - `tests/scripts/dyntopo_profile_collapse.txt` — collapse-heavy (the
    collapse guards are among the walk-bound candidates).
  - A `bench_dyntopo`-verb script (single converging dab; reports
    `ops_ms` / `update_ms` plus `splits/flips/rounds` for parity).
- **Primary metric**: `ops_ms` (the remesh + incremental-ownership phase) —
  `update_ms` (spatial) is out of scope. Secondary: end-to-end per-dab time
  from the perdab scripts' `[spatial-prof]` exit breakdown.
- **Discipline**: same-session A/B only (rebuild both binaries, run
  interleaved in one sitting); ≥ 5 runs per leg, report median + min/max;
  RelWithDebInfo, native clang build (`node make.mjs build native`); machine
  otherwise idle. After every sccache build, grep the log for `FAILED`
  (pipe-busy flake can leave stale binaries).
- **Parity check on every leg**: `splits/flips/smooths/rounds` from
  `bench_dyntopo` and final face counts must be *identical* pre/post — every
  change in this plan is supposed to be iteration-order-preserving, so any
  drift is a bug, not noise.
- **Record results** in the progress log at the bottom of this plan.

## M0 — Baseline + disk-share measurement

Answer the doc's open question: how much of `ops_ms` is actually the disk
chase?

1. Record baselines for all four workloads (table in the progress log).
2. Add throwaway `--profile`-gated counters (`CLAUDENOTE:`-prefixed, the
   established scaffolding pattern — ripped out in M5):
   - per-dab counts: `e_of_v` steps, `disk_insert`/`disk_remove` calls,
     radial-walk steps;
   - timing buckets inside the apply path separating topo splice vs attr
     interpolation vs meshlog callbacks vs spatial callbacks;
   - aggregate walk time at the `EdgeOfVertIter` call sites in `dyntopo.h`
     (bucket by site: collapse guards / boundary checks / smooth gathers).
3. Derive bytes-touched estimates (steps × ~12–24 B) as a sanity cross-check
   against the timing buckets.

**Gate G0**: a table attributing `ops_ms` across topo-splice / walk / attr /
callbacks. Decision rule:
- walk+splice share **< ~10%** of `ops_ms` → do M1 only (it is nearly free),
  skip M2–M4, close the plan;
- **10–20%** → M1 + M2, then re-decide;
- **> ~20%** → full ladder is justified, proceed through M3/M4.

## M1 — Side-bit embed (ladder rung 1)

Steal bit 0 of each disk link: `next = link >> 1, side = link & 1`. Removes
the `e.vs` load from the dependent address chain in the iterator; `vs`
becomes a leaf load read only for payload (`vs[e][side^1]`).

This is an encoding change to a meshlog-persisted TOPO column, not an
iterator tweak. Touch points (from grep, all direct `.disk` readers/writers):

- `mesh_iter.h` — `EdgeOfVertIter` (the win itself).
- `mesh.h` — `disk_insert` / `disk_remove` / `swap_elems` write encoded
  links.
- `mesh.cc` — `validateAndRepair`'s disk-cycle checks + wholesale rebuild
  must emit/verify the encoding.
- `mesh_serialize.cc` — the hand-maintained `topoTarget` remap must remap
  `(id << 1) | side`, not plain ids.
- `mesh_validate.h`, `utils/surface_walk.h`, `mesh_topo_cache.{h,cc}` —
  direct disk readers.
- `source/debug/script.cc` — `bench_dyntopo`'s inline valence loop.
- meshlog: audit that topo snapshots treat the column as opaque bits (then
  no change); any id-remapping path must learn the encoding.
- Sentinel convention: live cycle links are never `ELEM_NONE` (singletons
  self-link), but dead-slot/NONE values need a defined encoding.

**Gates**:
- Full ctest green, emphasis on `test_dyntopo*`, `test_*_undo*`,
  `test_mesh_serialize`, `test_spatial_dyntopo`/`_merge`.
- `save_pos`/stroke/`undo`/`assert_pos` debug-app undo-fidelity script.
- Parity: identical `splits/flips/rounds` on all workloads.
- A/B per the protocol. **Keep if** `ops_ms` improves measurably (≥ ~3%
  beyond run noise) **or** is neutral (the encoding also simplifies the
  iterator); revert if it regresses.

## M2 — Valence attribute probe (conditional on G0)

Only if M0 shows walk-bound sites that need just a *count* (collapse guards
are the candidates — the flip loop does not count valences).

- Add a u16 `.valence` builtin vert attribute maintained ±1 inside
  `disk_insert`/`disk_remove`.
- Undo correctness decision (make explicit before coding): either flag it
  TOPO so meshlog restores it with the disk columns, or recompute after
  undo/repair. `validateAndRepair`'s rebuild must recompute it either way.
- Replace the count-only walks found in M0 with attribute reads.

**Gates**: same test/parity battery as M1. **Keep if** `bench_dyntopo` /
perdab `ops_ms` moves ≥ ~3%; this result also feeds G3 — if a mere count
memoization moves the needle, the disk structure is load-bearing and the
slab is worth designing.

## M3 — Slab design review (gate before the big change)

Entry: G0 (and M2, if run) indicate walk+splice ≥ ~15–20% of `ops_ms` after
M1/M2 land.

Produce a short design doc covering:

- `v.e` → (offset, count) into pooled power-of-two slabs; insert = append at
  tail, remove = order-preserving shift (memmove ≤ a cacheline at valence 6);
  walk = sequential 4 B/step scan.
- **Bit-compat audit**: enumerate everything that establishes disk order
  outside `disk_insert`/`disk_remove` — `validateAndRepair` rebuild,
  `swap_elems`, mesh load/deserialize — and show each is
  sequence-equivalent under the slab (head = slot 0; head-removal advances
  to next automatically).
- Meshlog: the fixed-width TOPO column assumption breaks — design the
  variable-width path on the `LogChunkTypes::External` chunk seam.
- `mesh_serialize`: `topoTarget` handling for slab storage.
- Memory budget: pow2 slack (~33% at valence 6→8) vs the 16 B/edge int4 it
  replaces + dropped prev links — net table.
- Optional follow-up (separate A/B, not in the first cut): parallel
  neighbor-vert slab so smooth kernels never touch `e.vs` (mutable ring1
  CSR; dissolves freeze/thaw for v↔e adjacency).

**Gate G3**: user reviews and approves the design before M4 starts.

## M4 — Slab migration (only after G3)

Staged, each stage keeping the tree green:

1. **Parity harness first**: a unit test running randomized
   insert/remove/collapse-merge sequences against both the int4-cycle and
   slab implementations, asserting byte-identical `e_of_v` sequences
   (head-removal and singleton cases included).
2. Core mesh switch (`mesh.h`/`mesh.cc`/`mesh_iter.h` + the M1 touch-point
   list again).
3. Meshlog External-chunk path + undo suites.
4. `mesh_serialize` + save/load round-trip (including a real `.wproj`
   through the NW.js harness).
5. A/B per the protocol at 480k and the big workload; memory footprint
   before/after.

**Gates**: full ctest, undo-fidelity scripts, `.wproj` round-trip, parity
(`splits/flips/rounds` identical), and the A/B. **Ship if** `ops_ms`
improves ≥ ~10% on the perdab workloads with memory within the M3 budget;
otherwise revert and record why.

## M5 — Cleanup + write-up

- Rip out all M0 profiling scaffolding and every `CLAUDENOTE:` (promote any
  still-valuable note to a ≤ 3-line permanent comment).
- Update `dyntopoTangent.md` with the measured disk share and per-rung
  results; update `dynamic-topology.md` if the representation changed.
- Final full ctest + `pnpm test` before committing.

## Out of scope

- **Rung 3 (relative int16 links)** — dominated by the slab on every axis
  (see dyntopoTangent.md); do not implement.
- **Rung 2 (drop prev) as a standalone step** — `disk_insert` needs prev for
  O(1) tail lookup; the slab subsumes it. The tail-pointer variant is a
  contingency only if M3 rejects the slab.
- **Corner next/prev sentinel bits** — file-size/memory lever, wrong lever
  for dyntopo bandwidth (evaluated in the tangent doc).
- **Radial mate-corner compression** (tri-only, 8 B → 4 B/corner) — note as
  a follow-up candidate; same meshlog/serialize caveats, separate plan.
- GPU offload, spatial `update()` costs (~22%, separately tracked).

## Risks

- **Parity/determinism** is the whole game: the previous dyntopo perf pass
  rejected levers that changed iteration order. Every stage carries the
  splits/flips/rounds parity gate; the M4 parity harness runs before the
  representation switch, not after.
- Meshlog's fixed-width TOPO assumption is the largest hidden-cost item —
  it is why M3 exists as a separate design gate.
- Windows timing noise: mitigate with the same-session interleaved protocol
  and medians; treat < 3% deltas as noise.
- sccache pipe-busy flake: grep build logs for `FAILED`; retry after
  `sccache --stop-server` if hit.

## Progress log

(append dated entries + measurement tables here as milestones run)

### 2026-07-12 — M0 baselines (clean binary, RelWithDebInfo native clang)

Protocol notes discovered during setup:

- The perdab scripts' comment about a `[spatial-prof]` exit breakdown is stale —
  that instrumentation was ripped out in the 5M perf pass. Per-dab timing now
  comes from the M0 `[disk-prof]` scaffolding (`--profile`).
- New 4th workload committed: `tests/scripts/dyntopo_bench_single.txt` — same
  480k cube/brush geometry as the perdab script, one *converging*
  `bench_dyntopo` dab (detail=0.004: rounds=9, leftover=0, ~8.9k splits).

Baselines, 5 runs each, median (min–max). Parity was identical on every run:
bench splits=8889 flips=10832 rounds=9, faces 480000→497778.

| workload | metric | median | min–max |
|---|---|---|---|
| dyntopo_bench_single | ops_ms | 80.2 | 76.3–101.5 |
| dyntopo_bench_single | update_ms | 19.2 | 18.6–20.1 |
| dyntopo_profile_perdab | wall s | 91.0 | 85.0–115.5 |
| dyntopo_profile_perdab_big | wall s | 206.0 | 194.8–233.2 |
| dyntopo_profile_collapse | wall s | 4.75 | 4.46–6.29 |

(Wall times include mesh build + 200 dabs + per-dab tree update; first-run-of-a-
batch inflation is visible in the maxes. The instrumented binary with `--profile`
*off* measured ops_ms median ~74.5 over 5 steady-state runs — disabled-
scaffolding overhead is within run noise.)

### 2026-07-12 — M0 disk-share attribution (`[disk-prof]`, `--profile` runs)

Buckets are *exclusive* self-time (nested timers subtract), so e.g.
`split(self)` = splitEdge minus its attr-interp + callback time = topo surgery
+ local bookkeeping. Walk buckets time the `EdgeOfVertIter` loops at each
dyntopo.h site. Shares below are of the inclusive `ops` total for that run
(the remesh call; spatial `update()` excluded). Caveat: timer overhead
inflates the hot buckets ~5–10% (tens of millions of timed scopes); shares are
gate-grade, not A/B-grade.

| bucket | bench_single (101ms) | collapse (2.53s) | perdab (65.6s) | perdab_big (127.7s) |
|---|---|---|---|---|
| cb_meshlog | — (no meshlog) | 29.7% | 36.3% | 37.1% |
| scan_walk (e_of_v) | 12.8% | 22.7% | 19.5% | 18.7% |
| split(self) | 39.0% | 0.9% | 12.7% | 12.2% |
| flip_apply(self) | 14.2% | 3.9% | 10.1% | 10.0% |
| collapse(self) | — | 24.3% | 7.6% | 8.1% |
| cb_spatial | 8.7% | 2.7% | 3.7% | 3.9% |
| attr_interp | 10.9% | 7.1% | 3.2% | 3.1% |
| flip_collect (e_of_v) | 6.1% | 1.4% | 3.1% | 3.0% |
| scan(self) | 2.3% | 4.0% | 2.9% | 2.8% |
| guard_walk (e_of_v) | ~0 | 1.4% | 0.2% | 0.2% |
| feature_walk (e_of_v) | ~0 | 3.3% | 0.2% | 0.3% |
| mis+ops(self) | 6.0% | 2.1% | 0.7% | 0.6% |

Counts (perdab_big, per dab): 2.19M `e_of_v` steps, 176k radial steps, 66k
`disk_insert`, 34k `disk_remove` (~61 MB of link traffic/dab by the ~12–24 B
model). Cross-check: 2.19M dependent-chase steps × ~50 ns ≈ the measured
~120 ms/dab of scan_walk — the *latency*-bound model fits; a pure-bandwidth
model (61 MB at >10 GB/s ≈ 5 ms) does not. This confirms dyntopoTangent.md's
premise: the cost is the dependent `vs`+`disk` chase, not byte volume.

**Gate G0 decision.** Pure `e_of_v` walk share = scan_walk + flip_collect +
guard + feature ≈ **22–29%** of ops on the three stroke workloads; adding the
splice-containing buckets (split/collapse/flip_apply self) brings walk+splice
to **~50%**. That is far above the 20% bar → **the full ladder is justified;
proceed M1 → (M2?) → M3/M4.**

- **M2 note**: the collapse guards / boundary checks the valence probe would
  memoize are *tiny* (guard+feature ≤ 0.5% on the perdab workloads, ≤ 4.7%
  even on the collapse-heavy one) — M2's own entry condition ("walk-bound
  sites that need just a count") is **not met**; skip M2 unless M1/M3 change
  the picture. The dominant walk site is the candidate scan, which needs edge
  *lengths* (payload reads), not counts — exactly what the slab (M3/M4) and
  the side-bit embed (M1) target.
- **Out-of-plan finding worth recording**: `cb_meshlog` is the single largest
  bucket on the real stroke workloads (~36–37% of ops; 388k–712k callback
  invocations *per dab*). The disk ladder cannot touch it; a separate
  meshlog-callback-batching investigation would attack the top cost.

### 2026-07-12 — M1 side-bit embed: SHIPPED (kept)

Encoding: live disk links store `diskPack(edge, side) = (edge << 1) | side`
(helpers in `mesh_types.h`); dead slots stay raw `ELEM_NONE`. The iterator
carries `(e, side)` state — one `e.vs` load to seat, zero per step — and
`disk_insert`/`disk_remove` drop their `edge_side(prev/next, v)` vs loads
entirely (the embedded side replaces them).

Touch points beyond the plan's list: the remesh module carries ~12 more
manual disk-walk sites (curvature, density, preremesh ×4, feature_tag ×2,
quantize_ilp, reproject, singularity_adjust ×2) plus 9 test-side validators —
all mechanically decoded. `validateAndRepair` / `checkTopology` now also
verify the embedded side bit against `e.vs` (stale-bit corruption is caught).
Meshlog needed **no** change (rows are opaque byte snapshots — audited).
`mesh_serialize` bumped to format v4 with an order-preserving v3→v4 link
re-encode migration; the writer's `topoTarget` remap is packed-aware for the
disk column. New `save_mesh`/`load_mesh` debug verbs (permanent) back the
round-trip gates.

Gates (all pass):

- ctest: no regressions (95→identical set; 3 pre-existing environmental
  failures in this worktree: `test_live_stroke`/`test_bsmooth` need the WGSL
  backend configure flag, `test_debug_script` known-failing).
- Undo fidelity: `repro_single_undo` / `repro_live_undo` moved=0 worst=0;
  post-migration v3 mesh also survives stroke→undo→assert_pos clean.
- Parity: bench `splits=8889 flips=10832 rounds=9` identical every run;
  perdab 200-dab `[disk-prof]` counters **bit-identical** (250,252,767
  `e_of_v` steps, 7,395,036 inserts, 3,584,820 removes).
- Serialize: v4 round-trip clean; genuine v3 file (written by a pre-M1
  binary) migrates with `problems=0` under the side-bit-checking validator.

A/B (same-session interleaved, medians):

| workload | M0 | M1 | delta |
|---|---|---|---|
| bench_single ops_ms (7 runs) | 74.0 | 69.5 | **−6.1%** |
| perdab wall s (3 runs) | 56.0 | 52.9 | −5.6% |
| collapse wall s (5 runs) | 3.59 | 3.53 | −1.7% |

≥3% beyond noise on the primary metric → **keep** per the M1 rule.

### 2026-07-12 — M2 skipped; M3 design doc out for review (gate G3)

- **M2 (valence attribute): SKIPPED** by its own entry condition. Post-M1
  profiled perdab shares: guard_walk 0.18% + feature_walk 0.18% of ops — the
  count-only walk sites the probe would memoize are noise. The dominant walk
  (scan, 19%) needs edge *lengths*, which a valence count cannot serve.
- Post-M1 shares (profiled perdab, 200 dabs): scan_walk 19.1%,
  flip_collect 2.8%, split/collapse/flip_apply self 29.0%, attr 3.4%,
  cb_meshlog 38.2%, cb_spatial 4.0%. Walk+splice ≈ 51% → M3 entry condition
  met. (Cross-session absolute times vary ~2×; only shares and interleaved
  A/Bs are compared.)
- **M3 design doc**:
  [`2026-07-12-1520-dyntopo-disk-slab-design.md`](2026-07-12-1520-dyntopo-disk-slab-design.md)
  — representation, sequence-equivalence audit, meshlog span-log design,
  serialize v5, memory budget (−21%), ship bar, and 4 open questions.
  **Awaiting user review/approval (G3) before M4 starts.**

### 2026-07-12 — G3 approved; M4 built, measured, and **REVERTED** at the gate

G3 was approved (unencoded-id v5 stream, v1 freeze scope). M4 was implemented
in full and passed every correctness gate before failing the perf gate:

- Stage 1 parity harness (`test_disk_slab_parity`): slab vs a verbatim
  cycle-splice reference — byte-identical sequences over directed cases +
  32k randomized insert/remove/collapse-merge ops.
- Core switch kept `v.e` as the head cache (144 call sites untouched),
  deleted `e.disk` (16 B/edge), walked slabs via the iterator; meshlog got a
  per-chunk **span log** (sequences, not offsets — undo/redo rebuild slabs);
  serialize went to v5 with a vert-disk stream + v4→v5 migration. A latent
  ordering wart was fixed on the way: endpoint `onVertChange` now fired
  *before* the splice (mirroring the pre-mutation edge snapshots), which the
  span log needs and row capture silently tolerated.
- Gates passed: full ctest (same 4 pre-existing env failures), undo suites +
  fidelity scripts, **exact 200-dab counter parity** (250,252,767 `e_of_v`
  steps), v3/v4→v5 migration `problems=0`, `.wproj` save/load through the
  NW.js native-backend harness, memory **−20%** (9.55 MB vs 12.0 MB of disk
  storage at 747k edges, after raising `kMinClass` to 8 entries — class-4
  blocks stranded a freed block per interior vert and measured **+10%**).

**A/B (same-session interleaved, M1 leg vs slab leg):**

| workload | M1 | slab | delta |
|---|---|---|---|
| bench_single ops_ms (7 pairs, median) | 68.4 | 65.4 | −4.3% |
| perdab wall s (2 pairs) | 48.5 / 47.5 | 50.2 / 49.5 | **+4%** |
| perdab_big wall s (1 pair) | 101.6 | 108.0 | **+6%** |

**Why it lost** (profiled slab perdab vs M1 perdab, shares of ops):
scan_walk got *slower* (+11% self-time at identical step counts) — with
`alloc_near` clustering a vertex's ring edges, the int4 cycle links of a
ring already sit in 1–2 cachelines, so the dependent chase is largely
L1/L2-resident at 480k–805k and was **not** latency-bound; the slab's
sequential scan buys nothing there while paying iterator setup (slot +
arena-base loads per ring) 22.4M times/200 dabs. Splices went from O(1)
link RMWs to O(valence) scans (`split(self)` +7%), and the meshlog span
log added ~9.5M callback invocations (+4% cb_meshlog). The subdivide-only,
meshlog-free bench still won −4.3% — the win exists but is swamped by the
slab's per-op overheads on real strokes.

**Decision (per the M4 rule: ship only if ≥ ~10% better):** REVERTED. The
branch tip is reset to M1 + the M3 doc; the complete, gate-green slab
implementation is preserved on branch **`dyntopo-disk-slab-rejected`**
(commit 90eb763) with the parity harness, in case a future 5M+/out-of-cache
profile changes the calculus. The durable lesson for dyntopoTangent.md:
after M1, the disk representation is *not* the lever — the meshlog callback
path (~36–38% of ops, unchanged by any of this) is.

### 2026-07-12 — M5 cleanup: PLAN COMPLETE

- All `[disk-prof]` scaffolding ripped (`disk_prof.h` + every CLAUDENOTE call
  site), plus the pre-existing dead `splitEdgeTime`/`flipTime` accumulators
  and commented print in `runDyntopoRemesh`'s hot loop.
- Kept (permanent): the M1 encoding + its doc comments, the
  `dyntopo_bench_single.txt` workload, the `save_mesh`/`load_mesh` debug
  verbs (+ debugApp.md rows), and the strengthened side-bit checks in
  `validateAndRepair`/`checkTopology`.
- `dyntopoTangent.md` got a resolution banner (measured shares, rung 1
  shipped, rung 4 rejected + why); `dynamic-topology.md` notes the encoding.
- Final gates: bench parity still `splits=8889 flips=10832 rounds=9`; full
  ctest 92/96 with only the 4 pre-existing environmental failures
  (`test_debug_script`, `test_live_stroke`/`test_bsmooth` [WGSL backend not
  configured in this worktree], `test_dyntopo_multistep_gpu` [passes
  standalone]); parent `pnpm test` run before the final commit.

**Net result of the plan**: dyntopo ops ~6% faster (M1, shipped), the disk
question measured and closed, and the top cost identified for a future plan:
meshlog callback batching (~36–38% of ops).
