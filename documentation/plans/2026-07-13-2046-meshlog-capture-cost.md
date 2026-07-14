# Meshlog capture-cost plan (2026-07-13)

Goal: reduce the cost of the meshlog topo chunk's **row captures** — the
`ChunkElemRow` copies taken at first touch / create / kill — which the two
prior plans measured as the largest remaining non-surgical cost in a dyntopo
dab (~18% true ops share, nearly all of it capture after the callback plan's
M1 landed). Three ideas are investigated and prototyped **independently**,
each behind its own gate:

- **P1 — per-event-class column subsets**: capture only the columns the
  topo-undo mechanism actually owns, through a precompiled copy plan.
- **P2 — cross-chunk dedup**: stop re-capturing an element the same *step*
  already captured, for the one record class where that is provably safe.
- **P3 — lazier/cheaper kill capture**: measure whether the kill-time rows
  are worth specializing at all.

Predecessors:
[`2026-07-12-1324-dyntopo-disk-bandwidth.md`](2026-07-12-1324-dyntopo-disk-bandwidth.md),
[`2026-07-12-2141-meshlog-callback-batching.md`](2026-07-12-2141-meshlog-callback-batching.md)
(both complete; this plan is the "G0 third branch" the latter anticipated).

Status: **not started**.

## Background — what capture is and what it costs

Per perdab dab (480k mesh, from the callback plan's census): ~33 k
first-touch rows + ~13 k created records + ~4.6 k kill-unrecorded rows,
each paying `ChunkElemRow::captureFrom` (`meshlog_base.h`): `layoutFor`
walks **every** attribute of the domain computing offsets, then a per-column
`type_dispatch` switch + memcpy — ~0.7 µs per first-touch row sampled
(≈ 2–3× timer-inflated; the ablation legs below get the true number). The
same full-row copy repeats **per dab**: chunks are per-dab (the redo-hang
fix), so a brush lingering on a region re-captures the same elements' rows
in every dab's chunk.

Three structural facts shape the prototypes:

1. **Undo ownership is split.** Vert `co/no` (+ face `no`) of *Existed*
   elements are owned by the topo chunk row once `stampUndoGate` fires (the
   element store is suppressed for them) — so position must stay in any
   vert subset. Other data columns (color/mask/custom/select/boundary
   flags) have their own owners or are derived; which is exactly what M0
   must pin down before P1 cuts anything.
2. **Record classes have different replay contracts.** `Existed && Live`
   rows are *swap toggles* (no end_body; undo/redo swap live↔stored).
   `Created && Live` rows have end-bodies frozen at their chunk's seal —
   later rewires are deliberately owned by *later chunks' Existed records*
   (the created-vert redo fix). Any dedup must respect that split.
3. **Capture reads must happen pre-mutation/pre-free** (kill callbacks fire
   before release) — kill capture cannot be deferred past the kill, only
   made cheaper.

## Measurement protocol

As the predecessor plans, with the hard-won amendment: **no per-event
timers** (at 50–700 ns granularity they inflate 2–3×). Attribution comes
from:

- **Ablation legs**: CLAUDENOTE-gated build toggles that stub a specific
  cost (e.g. `captureFrom` body → no-op) — correctness-broken, timing-valid
  when the run never replays undo. Interleaved same-session wall vs the
  un-stubbed leg bounds that cost's true share. One stub leg each for:
  whole captureFrom, kill-only captures, layoutFor-only.
- **Counters** (kept from the callback plan's census shape): rows captured
  by (domain × record class), bytes copied per row.
- Workloads: `dyntopo_profile_perdab.txt` (480k) and `_big` (805k) walls,
  interleaved ≥3 pairs, medians; `dyntopo_bench_single.txt` is the
  meshlog-free control (must stay ~0%).
- Parity every leg: bench `splits/flips/rounds` identical; undo suites
  (`test_dyntopo_undo*`, `test_meshlog_topo`, `test_dyntopo_stroke_undo`,
  `test_dyntopo_undo_nonnewest`, `test_mesh_reorder`) + both
  `tools/repro/repro_*_undo.txt` fidelity scripts.
- RelWithDebInfo native clang; grep builds for `FAILED` (sccache flake);
  < 3% deltas are noise. Keep bar per prototype: **≥ ~3%** perdab or
  perdab_big wall, beyond noise.

## M0 — Investigation: ownership matrix + true capture bounds

No behavior changes. Two deliverables in this plan's progress log:

1. **Column-ownership matrix** per domain (vert/edge/corner/list/face):
   for every builtin + expected custom column, which mechanism restores it
   on undo — topo-chunk row (TOPO links; gated `co/no`), element store
   (brush-gated data), derived/recomputed post-undo (boundary overlays,
   spatial TEMP), or nothing (NOCOPY zero-restore semantics). Sources:
   `stampUndoGate` + `AttrSaver` gating, `ChunkElemRow`
   capture/writeTo/swapWith flag handling, `refreshCreatedVertData/FaceData`
   (which columns it refreshes and why), `mandatoryBuiltinFlags`, boundary
   `recomputeDirty`. **Each P1 cut must cite this matrix.**
2. **Ablation bounds**: the three stub legs above, run interleaved on
   perdab + perdab_big → true ms shares for total capture, kill capture,
   and layout overhead. These decide the prototype order and P3's fate.

**Gate G0**: proceed with any prototype whose ablation bound exceeds ~3% of
wall; drop (with numbers) any that doesn't. Expected from current data:
total capture well above; kill-only likely marginal.

## M1 — Prototype P2: step-scoped dedup for `Existed && Live` re-captures

Ordered first because it eliminates *whole rows*, not bytes, and it is a
direct generalization of the shipped chunk-stamp mechanism.

**Claim to verify, then exploit**: if element X has an `Existed && Live`
record in an earlier chunk of the *same step*, a later chunk's
`Existed && Live` record for X is redundant. Undo replays chunks in
reverse: with the later record absent, the earlier chunk's swap still
restores X's step-begin state; redo re-swaps to the post-step state. No
replay step *reads* another element's intermediate state (restores are
writes by index), so the intermediate capture served nothing.
**Counter-case that stays per-chunk**: X `Created` in chunk N then rewired
in chunk N+1 — the N+1 `Existed` record is load-bearing (Created
end-bodies freeze at their chunk's seal; later rewires must be owned by
later chunks — the created-vert redo fix). Kills likewise unaffected.

Prototype:

- Second stamp tier beside the shipped per-chunk stamps: per-**step**
  generation, set **only** when an `Existed && Live` record is created,
  checked in the change fast path *after* the per-chunk stamp misses.
  Cleared by `onKill` (both tiers); step gen bumps in `beginStep`, on mesh
  switch, and on mid-step reorders (`compactIfFragmented` /
  `reorderForLocality`), exactly like the chunk tier.
- `Created` elements never set the step stamp (their later rewires must
  re-record as Existed in later chunks — which sets the step stamp from
  then on, correctly: subsequent rewires dedup against *that* record).
- The undo gate (`stampUndoGate`) semantics stay per-first-touch-per-step —
  unchanged by construction (it already fires only on the slow path).

**Correctness gates**: the full battery **plus a new targeted unit test**
(`test_meshlog_multichunk_dedup.cc`, `test_meshlog_topo` style): scripted
ops re-touching the same elements across explicit `pushTopoChunk`
boundaries — including create-then-rewire-across-chunks and
touch-then-kill-across-chunks — asserting undo/redo round-trips restore
byte-identical rows and that redo after undo converges over 3 cycles.
**Keep if** ≥ ~3% on either perdab workload. Expected: the biggest rung —
most first-touches in a lingering stroke are re-touches of prior dabs'
regions.

## M2 — Prototype P1: capture plans + per-class column subsets

Two layers, measured separately:

1. **Mechanical (semantics-free): precompiled capture plan.** Cache per
   `AttrGroup` (keyed on its attr list, invalidated on append) a flat
   `{offset, size, srcColumn}` array so `captureFrom`/`writeTo`/`swapWith`
   become straight memcpy loops — no per-column `type_dispatch`, no
   `layoutFor` re-walk per row. Benefits every capture including P3's.
   Ship on its own if ≥ ~3% (or neutral-with-simplification is fine here —
   it deletes per-row work by construction; A/B still required).
2. **Semantic: per-record-class column subsets**, strictly per the M0
   matrix. First candidate: `Existed && Live` rows restricted to
   TOPO links + gated data columns (vert `co/no`, face `no`) + `select` —
   dropping color/mask/custom payloads *if and only if* the matrix shows
   another mechanism owns them for Existed elements. `Created` and `Dead`
   rows stay full-row (realloc/writeTo must reconstruct the whole element).
   The subset is a second cached plan per domain; the row records which
   plan built it so `writeTo`/`swapWith` replay the same subset (the
   existing `count_` prefix-guard generalizes to a plan id).

**Gates**: full battery + the M1 unit test extended with custom attr
layers (a color + a custom float layer on all domains) asserting undo
restores them for created/killed elements and leaves Existed elements'
unowned columns to their real owner. **Keep** each layer independently at
≥ ~3% (layer 1 also on simplification grounds if neutral).

## M3 — Prototype P3: kill-capture cost

Entry-gated on M0's kill-only ablation bound ≥ ~3% (current estimate says
it is *below* — expect this to close as "measured, not worth it", which is
a valid outcome). If it clears:

- Kill rows ride the M2 capture plan automatically (cheaper copies).
- The remaining idea worth prototyping: corner/list burst kills inside
  `kill_face` share one face's context — a per-face batched capture
  (single plan lookup + contiguous staging append) instead of per-corner
  rows. Only the copy mechanics change; the record-per-element model and
  replay order stay identical.

## M4 — Cleanup + write-up

- Rip all ablation stubs, counters, and `CLAUDENOTE:`s (promote keepers to
  ≤ 3-line comments).
- Progress log: per-prototype tables + keep/drop verdicts. Update the
  callback plan's closing note and `dyntopoTangent.md`'s banner with where
  the capture share landed; document the new stamp tier / capture plans in
  `dynamic-topology.md` §9 if shipped.
- Full ctest + `pnpm test` before the final commit.

## Out of scope

- Chunk granularity (per-dab seal) and end-body timing — load-bearing for
  redo; P2 works *within* those contracts, never relaxes them.
- Element-store / brush gate redesign (only cited via the M0 matrix).
- Undo *replay* performance; serialization; WASM-side anything.
- Dispatch/batching (closed by the callback plan's measurements).

## Risks

- **P2 is the semantic rung**: the safety argument leans on "replay never
  reads other elements' intermediate state" and on the Created/Existed
  ownership split. The dedicated multi-chunk unit test exists precisely to
  break it before the workloads do; the create-then-rewire and
  touch-then-kill cross-chunk cases are the ones that bit before
  (redo-hang, created-vert corruption memories).
- **P1's subsets are only as correct as the M0 matrix**; anything ambiguous
  (custom layers, sculpt-layer deltas, VDM/multires columns) defaults to
  "keep in the row". The matrix must explicitly cover a mesh with sculpt
  layers + color + custom attrs, not just the bench cube.
- Mid-step attr append (boundary lazily creates layers): capture plans key
  on the attr list and rebuild on mismatch; rows carry their plan id (the
  existing `count_` guard pattern).
- Measurement honesty: sampled per-event numbers are banned here; if an
  ablation leg can't isolate a cost, say so in the log rather than
  estimating.

## Progress log

(append dated entries + measurement tables here as milestones run)

### 2026-07-13 — M0a: column-ownership matrix

Sources read: `AttrSaver` (attr_saver.h — the gate maps kernel-written attrs
to flag bits; `.strokeid.<domain>` stamp column is NOCOPY|TEMP|NOINTERP),
generated-kernel gating (`emit_cpp.cc` — each kernel registers exactly the
attrs it writes: CO/NO/COLOR/MASK + CUSTOM bits for layer handles, then
`needsData`→ element store append →`updateSaved`), `stampUndoGate`
(0xffff = after a topo touch the row owns *all* gated flags for that
element+stroke), `ChunkElemRow` (captureFrom/swapWith skip NOCOPY; writeTo
zero-fills NOCOPY = reset semantics), `refreshCreatedVertData/FaceData`
(re-reads non-TOPO/non-NOCOPY data columns of Created rows at endStep),
boundary.cc (source flags persistent, derived layers TEMP), spatial attrs
(TEMP|NOINTERP|NOCOPY), brush temporaries (`.brush.orig.*`, `.brush.dab.gen`
TEMP|NOCOPY), sculpt-layer rest snapshot TEMP (layer channels persistent).

| ownership class | columns | undo owner |
|---|---|---|
| topo row, sole owner | all TOPO links; `select` (all domains); `.list.size`, `.face.list_count`; boundary **source** flags (seam/sharp/projected…) | ChunkElemRow realloc/writeTo/swap |
| gate-transferred | vert `co`/`no`, face `no`, color/mask, kernel-writable custom layers — **for topo-touched elements** (0xffff stamp) | topo row (else element store) |
| element store | same data columns for non-topo-touched elements | LogChunkElems per stroke |
| derived (capture is wasted bytes) | EDGE_POLYGROUP/UVCHART/DIRTY, VERT_DIRTY/CLASS (TEMP), `.boundary.*.dirty` markers | `recomputeDirty` / markers |
| NOCOPY (already skipped) | `.spatial.{v,f}.node`, `.strokeid.*`, `.brush.orig.*`, `.brush.dab.gen`, sculpt rest snapshots | tree passes / reset-to-zero |

**Consequences for the prototypes**:
- P1's semantic subset for `Existed && Live` rows is only large if paired
  with a **partial gate**: `stampUndoGate` must stamp only the flags the row
  captured (e.g. CO|NO) so color/mask/custom stay element-store-owned — the
  0xffff conservatism exists to avoid two-owner ordering conflicts, and a
  by-flag ownership partition preserves that argument (each flag has exactly
  one owner). Without the gate change, only the TEMP-derived boundary
  columns are safely skippable (small).
- Created/Dead rows must stay full-row (reconstruction), as planned.
- `refreshCreatedVertData` re-reads data columns through the row's own
  layout — subset plans must keep it working for Created rows (full-row, so
  unaffected).

### 2026-07-13 — M0b: ablation bounds; gate G0 decided

One binary, env-toggled legs, 3 interleaved passes on perdab (wall s,
medians): baseline **45.5**; no-capture (`SC_ABLATE_CAPTURE=1`) **32.5**;
layout-only (`=2`, offsets+resizes but no copies) **43.4**; no-kill-capture
**43.1**.

| bound | ms | share of wall |
|---|---|---|
| total `ChunkElemRow` capture (A−B) | ~13,000 | **~28.6%** |
| … of which `layoutFor` + per-row resizes (C−B) | ~10,900 | **~24%** |
| … of which the actual byte copies (A−C) | ~2,100 | ~4.6% |
| kill-time captures (A−D, subset of total) | ~2,400 | ~5.3% |

The ablation says the earlier per-event estimates *under*-counted capture:
with end-body finalize captures included, ~63 k rows/dab each pay a full
attr-list walk (`ref.data->elemSize` deref per column) plus two Vector
resizes just to recompute a layout that is **identical for every row of a
domain**. The copies themselves are minor.

**Gate G0 (execution reorder)**: P1-layer-1 — the precompiled shared
per-domain row layout — is promoted to the first prototype (~24%-of-wall
bound, zero semantic change). P2 dedup stays second (it removes whole rows,
so its win compounds with whatever layer 1 leaves). P3 clears its entry bar
today (5.3%) but is expected to shrink under layer 1 — re-measure after,
before writing any kill-specific code. P1-layer-2 (subsets + partial gate)
re-measured last against the residual copy cost (~4.6% bound).
