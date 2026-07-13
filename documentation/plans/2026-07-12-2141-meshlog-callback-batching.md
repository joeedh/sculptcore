# Meshlog callback batching plan (2026-07-12)

Goal: cut the meshlog callback path's share of a dyntopo dab — measured at
**~36–38% of `ops_ms`, the single largest bucket** — by attacking the
per-event overhead of the topo-chunk capture pipeline, without changing what
gets captured or when. Successor to
[`2026-07-12-1324-dyntopo-disk-bandwidth.md`](2026-07-12-1324-dyntopo-disk-bandwidth.md)
(branch `dyntopo-disk-bandwidth`), whose M0 profiling identified this as the
real lever after the disk-representation ladder was measured out.

Status: **not started**.

## Background — the measured anatomy of one callback

Numbers from the disk-bandwidth plan's `[disk-prof]` runs (its progress log):
`cb_meshlog` self-time 12.1–23.8 s of 33–66 s ops on the perdab workloads at
**77.66 M invocations / 200 dabs (388 k/dab; 712 k/dab on perdab_big)**, vs
`cb_spatial` at only ~4% for 5.5 M invocations — the meshlog path is ~3×
costlier *per event* than the spatial one, and there are 14× more of them.

What one event costs today (`MeshLog::installCallbacks`, `meshlog_base.h`):

1. **Dispatch**: the mesh op fires a `util::function` (= `std::function`)
   per element per event. On the stroke path the executor's *combined*
   callbacks wrap the meshlog std::functions inside further std::function
   lambdas for 4 of the 15 slots (`applyDynTopoDab`, `brush_executor.h`) —
   two chained type-erased calls.
2. **Forward lambda**: `active_mesh_` null-check, `getTopoChunk()` (checks
   `curEntry().topo_chunk_` — cheap but a pointer chase through the entries
   vector).
3. **`LogChunkTopo::onChange/onCreate/onKill`**: `makeKey(kind, idx)` →
   `idx_to_log_id` hash lookup (`util::Map<int64,int64>` by default). For
   the **already-recorded element this is the whole call** — and it is the
   dominant outcome: dyntopo's rounds re-touch the same region's elements
   over and over within a dab (split → flip → collapse all firing
   change-events on the same verts/edges/corners).
4. **`stampUndoGate`**: for Vert/Face events, an *unconditional*
   `AttrSaver::updateSaved(idx, strokeId, 0xffff)` attr write — repeated
   idempotently on every subsequent no-op event for the same element.

So the hot path is dominated by type-erased dispatch + a hash lookup + a
redundant gate write for events that do **no capture work at all**. First
touches (which pay a `ChunkElemRow::captureFrom` row copy) are the minority
and are real, useful work.

Corollary worth stating: none of this plan touches mesh mutation or
iteration order. The callback pipeline is a pure observer; every rung is
parity-safe *by construction* (unlike the disk plan, where order-parity was
the whole game). The gates still verify it.

## Measurement protocol

Same as the disk-bandwidth plan (see its protocol section), abbreviated:

- Workloads: `dyntopo_profile_perdab.txt`, `dyntopo_profile_perdab_big.txt`,
  `dyntopo_profile_collapse.txt`, `dyntopo_bench_single.txt` (note:
  bench_dyntopo runs **without** meshlog — it is the *control*: rungs here
  should move it ~0%; the perdab stroke workloads are the metric).
- Primary metric: perdab / perdab_big wall (whole-script, interleaved
  same-session A/B, ≥3 pairs, medians; the stroke path has no per-dab ops
  print since the profiling scaffolding was ripped — M0 re-adds a minimal
  `--profile`-gated one).
- Parity on every leg: identical `splits/flips/rounds` (bench) and, for the
  stroke workloads, identical final face counts; undo suites + the
  `save_pos`/stroke/`undo`/`assert_pos` fidelity scripts.
- RelWithDebInfo native clang; grep build logs for `FAILED` (sccache
  pipe-busy flake); machine otherwise idle; < 3% deltas are noise.

## M0 — Decompose the 36%

Re-add throwaway `--profile`-gated counters (`CLAUDENOTE:`-prefixed, ripped
in M4) — the disk plan's `disk_prof.h` pattern, scoped to the callback path:

1. Per-dab event counts bucketed by **(kind × outcome)**: `created`,
   `first-touch` (row captured), `noop` (already recorded), `kill-recorded`,
   `kill-unrecorded`. Hypothesis to confirm: `noop` ≥ ~75% of events.
2. Timing buckets inside one event: dispatch+forward (measured by
   difference), hash lookup, `captureFrom`, `stampUndoGate`. (Per-event
   timers are too heavy for 400 k events/dab — instead time whole phases
   with counters and derive per-outcome costs by linear fit across the
   outcome mix, or sample 1-in-N events.)
3. Also count `cb_spatial` events by outcome for the optional M3.

**Gate G0** (decision rule):
- `noop` share ≥ ~50% of meshlog callback time → M1 is the main dish;
  proceed M1 → M2, re-measure, then decide M3.
- dispatch overhead ≥ ~25% → M2 matters; else it is a cleanliness rung.
- Neither (capture dominates) → the honest outcome is "the work is real";
  pivot the plan to capture-cost reduction (row layout / gate design), which
  needs its own design doc before code.

## M1 — Generation-stamped no-op fast path

Kill the hash lookup + redundant gate write for already-recorded elements.

- `LogChunkTopo` gains per-domain dense stamp buffers (the dyntopo `GenSet`
  pattern: `Vector<uint32>` sized to element capacity + a generation counter
  bumped per chunk instance — zero clearing cost per dab). `onChange` fast
  path: `stamp[idx] == gen` → return immediately (one dense array read; no
  `makeKey`, no hash, no gate write). Slow path (first touch): existing
  logic + set the stamp.
- `onCreate` sets the stamp; `onKill` must consult the map as today (it
  needs the record; kills are a small minority) but clears/keeps the stamp
  consistently — define the Created-then-killed and Existed-killed stamp
  semantics explicitly (dead slots may be re-allocated within the same
  chunk by undo-replay-adjacent paths; the stamp must not leak a stale
  "recorded" verdict onto a reused index — clear the stamp in `onKill`,
  and `onCreate` always takes the slow path to build its record anyway).
- `stampUndoGate` moves inside the slow path only (its purpose — gating the
  brush element store — needs one stamp per element per stroke, not one per
  event). Verify against the gate's contract in `stampUndoGate`'s comment
  (element-store row suppression) — the gate must still be stamped on the
  FIRST event of each element per step, which the slow path guarantees.
- Memory: 4 B × capacity × 3 hot domains (vert/edge/corner), owned by the
  MeshLog (not per chunk — chunks share the buffer, generation partitions
  them). ~12 B/elem transient; at 800 k edges ≈ 10 MB ceiling, acceptable;
  buffers live only while a log exists.

**Gates**: full ctest (emphasis `test_dyntopo_undo*`, `test_meshlog_topo`,
`test_dyntopo_stroke_undo`, `test_dyntopo_undo_nonnewest`), both undo
fidelity scripts, parity, A/B. **Keep if** perdab wall improves ≥ ~3%
beyond noise (expected: well above — this targets the dominant outcome).

## M2 — Flatten the dispatch chain

- Replace the executor's combined-callback lambdas (`applyDynTopoDab`) with
  a small concrete fan struct: one `MeshCallbacks` whose slots call
  `meshlog` then `spatial` through *plain member pointers* captured once —
  no std::function-inside-std::function. (The `MeshCallbacks` seam itself
  stays std::function for now; this rung only removes the double wrap.)
- Optional sub-step, only if M0 shows the remaining single dispatch is hot:
  give `MeshCallbacks` a parallel "direct sink" interface (an abstract
  `IMeshEventSink*` with final impls for meshlog/spatial/fan) used by the
  topology ops when set — one virtual call instead of a type-erased one.
  This touches every `fire(cb->onX, idx)` site in `mesh.cc`; keep it inside
  the existing null-check helper so the diff stays mechanical.

**Gates**: same battery. **Keep if** ≥ ~3% or neutral-with-simplification
(the fan struct is simpler than the current lambda composition either way).

## M3 — Bulk event hooks (conditional on post-M1/M2 re-measure)

Only if the re-measured callback share is still ≥ ~10% of ops and dispatch
(not capture) remains the cost:

- Add optional span hooks to `MeshCallbacks` (`onVertsChange(span<const int>)`,
  …) that default to per-element fan-out; convert the highest-count firing
  sites (the splice snapshot blocks in `make_edge`/`kill_edge`/
  `relink_edge_verts`, and `splitEdge`/`collapseEdge`'s created-element
  reporting) to single bulk calls. The meshlog sink loops internally —
  same events, one dispatch.
- The spatial callbacks (~4–6%) can adopt the same hooks here if the M0
  spatial-outcome counts justify it; otherwise explicitly out of scope.

**Gates**: same battery; keep bar ≥ ~3%.

## M4 — Cleanup + write-up

- Rip all M0 scaffolding / `CLAUDENOTE:`s (promote keepers to ≤ 3-line
  comments).
- Record per-rung results in the progress log; update `dyntopoTangent.md`'s
  resolution banner ("next lever" section) with the outcome; note the new
  callback architecture in `dynamic-topology.md` §9 if M2/M3 changed it.
- Full ctest + `pnpm test` before the final commit.

## Out of scope

- Changing *what* is captured (row layout, per-domain gating policy, chunk
  granularity) — that is capture-cost work, gated behind G0's third branch.
- The spatial tree's incremental-ownership logic itself (only its dispatch
  may ride M3).
- The element store / brush deform save-gate design (only the redundant
  re-stamping in M1).
- Undo/redo *replay* costs — this plan is about forward capture only.

## Risks

- **Stamp-vs-map divergence** (M1): the stamp is a cache of "has a record
  in the current chunk"; any path that mutates `idx_to_log_id` without the
  stamp (record drop in `onKill` for Created elements, chunk deactivation
  via `pushTopoChunk`, index reuse after raw `ElemData::release` during
  undo) must invalidate coherently. Generation-bump-per-chunk removes the
  deactivation case wholesale; `onKill` clears the stamp; undo/redo never
  runs while a chunk is capturing.
- The `stampUndoGate` de-duplication changes when (not whether) the brush
  gate is stamped; the created-vert/face refresh logic in `endStep`
  (`refreshCreatedVertData/FaceData`) and the element-store suppression
  contract must be re-read before coding — the undo fidelity scripts and
  `test_dyntopo_stroke_undo` are the safety net.
- Perdab wall is a noisier metric than the old `ops_ms` (which lived in
  bench_dyntopo, meshlog-free); M0's minimal per-dab `--profile` timing in
  the stroke path is what the A/Bs read. Keep it tiny and rip it in M4.

## Progress log

(append dated entries + measurement tables here as milestones run)
