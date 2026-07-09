# Parallelize deferred node split + GPU batch regen (spatial)

**Goal:** cut `SpatialTree::update()` wall time under dyntopo sculpting by
parallelizing its two dominant phases — `applyDeferredNodeSplit()` (~41–45%)
and `regen_gpu_node()` (~22–24%) — without breaking tree/GPU-partition
invariants, undo parity, or WASM (single-threaded) builds.

Related docs: `sculptcore/documentation/spatial.md` (update pipeline, GPU
partition invariants), `sculptcore/documentation/plans/dyntopo-m7-cascade.md`
(deferred split/merge origin, M7.6).

## Baseline (measured 2026-07-08)

Temporary `CLAUDENOTE:`-tagged scaffolding in `source/spatial/spatial.cc`
(namespace `prof`, prints at process exit) breaks down `update()`. Workload:
200 dyntopo draw dabs (radius 0.15, strength 0.3, detail = base edge / 2),
one `update()` per dab, headless `debug_app`.

Repro:

```
node make.mjs build native
build/native/source/debug/debug_app.exe --script <scratchpad>/dyntopo_profile_perdab.txt
```

(Scripts currently in the session scratchpad; M0 moves them into
`tests/scripts/`.)

480k-tri base mesh (120k-tri run in parens, as % of update() total):

| phase                          | total ms | share       |
| ------------------------------ | -------- | ----------- |
| update() total (200 calls)     | 3935     | —           |
| deferred split                 | 1778     | 45% (41%)   |
| regen_gpu_node (7056 calls)    | 937      | 24% (22%)   |
| bounds regen                   | 448      | 11% (12%)   |
| normals phase (parallel)       | 276      | 7% (10%)    |
| ensure_node_tris (parallel)    | 212      | 5% (9%)     |
| assign_gpu_nodes               | 124      | 3% (3%)     |
| slice update phase (parallel)  | 35       | 1% (1%)     |
| draw-batch loop + merge        | 61       | 2% (2%)     |

Key facts the plan builds on:

- Deferred split cost is work-conserving (batched-per-stroke vs per-dab
  cadence does the same total work), so the share is real for interactive
  sculpting.
- Under dyntopo nearly every dirty leaf takes the full-regen path (7056
  regens vs 1421 slice updates) — the already-parallel slice path is
  irrelevant; `regen_gpu_node` is the right target.
- Worst single-update spikes come from deferred split (42 ms) and
  regen (5.6 ms/call max).
- `litestl::alloc` is thread-safe (thread-local `MemList` + mutex), so
  worker-thread allocation is not a blocker.

## M0 — sub-phase profiling (decides M2's strategy)

Extend the existing `prof` scaffolding one level down. All additions stay
`CLAUDENOTE:`-tagged; M4 removes everything.

- **M0.1 `split_node` attribution.** Timers + counters across
  `applyDeferredNodeSplit`: (a) vert unassign + mean pass, (b)
  `triangulateFace` re-triangulation, (c) `add_face_intern` re-file descent
  (including recursive inline splits — count them separately), (d) orphan-vert
  recovery walk, (e) `alloc_node` / `create_data` / `delete_data`
  bookkeeping. Counters: candidates per update, recursive splits triggered,
  faces re-filed, verts re-owned.
- **M0.2 `regen_gpu_node` attribution.** (a) old-buffer dispose, (b)
  `collect_subtree_leaves` + sizing, (c) `createBuffer` calls, (d)
  `fill_leaf_slice` / `fill_leaf_attr` fill loops, (e) slice-table build.
  Plus a histogram-ish min/avg/max of per-owner `total_verts` (fill-work
  granularity for M1's load balancing).
- **M0.3 workloads into the tree.** Move the two profiling scripts into
  `tests/scripts/` (`dyntopo_profile_perdab.txt`, `_big` variant) so runs are
  reproducible outside this session. Add a collapse-heavy variant
  (`mode=both`, coarser detail on a fine mesh) so M2 also sees the
  merge-cadence path under load.
- **Gate:** attribution table for both functions on the 480k workload; M2
  option chosen from the data.

### M0 results (measured 2026-07-09, 480k workload)

Scripts landed as `tests/scripts/dyntopo_profile_perdab{,_small,_big}.txt` +
`dyntopo_profile_collapse.txt`; scaffolding moved to the shared
`source/spatial/spatial_prof_temp.h`. 200 updates, instrumented build (totals
run a bit hot vs the uninstrumented baseline — attribution shares are the
signal):

| phase                         | total ms | share of parent  |
| ----------------------------- | -------- | ---------------- |
| update() total                | 5182     | —                |
| regen_gpu_node (6585 calls)   | 1818     | 35% of update    |
| — fill loop (fill_leaf_slice) | 1679     | **92% of regen** |
| — dispose                     | 75       | 4%               |
| — createBuffers               | 49       | 3%               |
| — collect+size                | 9        | <1%              |
| deferred split (1849 splits)  | 1283     | 25% of update    |
| — triangulateFace+calc_center | 543      | **42% of split** |
| — refile pure descent         | 537      | **42% of split** |
| — alloc/data bookkeeping      | 24       | 2%               |
| — unassign+mean               | 19       | 1%               |
| — orphan recovery             | 7        | <1%              |

Counters: 1964 candidates → 1849 top-level splits + 497 recursive; 1.42M faces
re-filed; 787k verts unassigned; only 734 orphans. Per-owner `total_verts`
avg 7299 / max 11250, ~7 slices per owner → per-slice fill jobs load-balance
fine. `regen_node_tris` never fired inside regen (the tris phase already
covers it); `fill_leaf_attr` never ran (legacy color path on this workload).

**Decisions.** M1 as planned (fill loop is 92% and embarrassingly parallel).
M2: `add_face_intern` never reads the `tris` span and `triangulateFace`
*always* succeeds (fan fallback) — the entire re-triangulation at all three
call sites (split_node, merge_node, initial build) is dead work → M2.a first
(drop it; keep `calc_center` bit-identical so routing parity holds),
re-measure, then decide whether M2.b's candidate parallelism is still needed
for the ≥2× gate.

## M1 — `regen_gpu_node`: serialize-allocate + parallel fill

Split the function into a serial planning stage and a pure parallel fill
stage, mirroring the proven `update_gpu_node_slice` pattern (allocation
serial, disjoint-slice writes parallel).

- **M1.1 refactor into plan/fill.** Serial per owner: dispose old buffers,
  collect subtree leaves, `regen_node_tris` any leaf still flagged
  (keeps mesh-topology reads out of the parallel stage), size + `createBuffer`
  pos/nor/attr buffers, build the `LeafSlice` table, resolve `srcRefs`
  (attribute lookups) once, clear leaf flags, bump `builtAttrsVersion`.
  Output: a flat work list of `(owner, slice, srcRefs)` fill jobs.
- **M1.2 one unified parallel fill pass.** In `update()`, run a single
  `litestl::task::parallel_for` over *regen fill jobs + surviving
  `update_gpu_node_slice` jobs* (they're the same slice-write shape). Body is
  pure: `fill_leaf_slice` + `fill_leaf_attr` into disjoint sub-ranges; no
  flag writes, no manager calls (same discipline as the existing slice-pass
  comment block). Keep the `NO_PARALLEL_FOR` serial fallback so WASM builds
  are unchanged.
- **M1.3 serial epilogue.** `update_buffer` flags, `gpuLayoutGen++`,
  `drawBatchUpdated` — after the join, exactly like today's slice epilogue.
- **Ordering note:** owner dedup must happen before the parallel pass; today
  duplicate-owner suppression relies on the first regen clearing leaf flags
  mid-loop, which is racy once fills overlap.
- **Gate:** `node make.mjs test` green (esp. `test_spatial_dyntopo`,
  `test_spatial_merge`, `test_spatial_gpu_partition`); `dyntopo_draw` /
  `dyntopo_refine` screenshots unchanged; `assert_manifold` workloads pass;
  A/B on the 480k workload shows regen phase ≥3× faster and no regression in
  any other phase. Buffer contents must be bit-identical to serial (fills are
  pure gathers — verify once with a checksum diff run, then drop the check).

### M1 results (measured 2026-07-09, 480k workload)

**Gate met.** Regen phase (plan + regen share of the unified fill pass) went
1911 ms → 553–665 ms across three runs = **2.9–3.5×** (run-to-run machine
variance is ±15%; regen/fill-job counts are identical every run — 6585 plans,
45845 fill jobs — so the pipeline is deterministic). Serial plan cost is
100–120 ms; the unified parallel pass runs 453–573 ms doing work that took
1679 ms serially (3.1–3.7× on the fixed 6-thread `litestl::task` pool — the
pool size, `LITESTL_WORKERS_COUNT`, is a compile-time 6 on a 16-core machine;
raising it is a separate follow-up with repo-wide blast radius).

Verification: per-update FNV checksums over every GPU buffer are bit-identical
between `SC_FILL_SERIAL=1` and the parallel pass on both the 120k and 480k
workloads (200 updates each). `ctest`: same 4 pre-existing failures as the M0
baseline commit (`test_debug_script`, `test_live_stroke`,
`test_dyntopo_multistep_gpu`, `test_bsmooth` — all fail identically on the
un-touched M0 tree in this environment), everything else green, including
`test_spatial_dyntopo` / `test_spatial_merge` / `test_spatial_gpu_partition`.
`dyntopo_draw` / `dyntopo_refine` run clean. Other update() phases bracket
their baselines within variance (bounds regen 899–1114 vs 945 baseline).

Implementation notes: the missing-buffer GPU-node loop is folded into the same
plan/fill path (it was a serial `regen_gpu_node` sweep after the slice pass);
`regen_gpu_node` itself is now plan + serial fill and remains the fallback for
failed in-place slice updates. Slice-update jobs whose owner got planned for a
full regen are dropped before the parallel pass (their slice pointers may be
stale and the regen fill rewrites the whole buffer).

## M2 — deferred node split

Strategy is picked from M0.1 data. Ranked options, cheapest-risk first:

- **M2.a serial micro-opts (do regardless if M0 confirms).**
  - `split_node` re-triangulates every owned face via `triangulateFace`, but
    the leaf already holds current `data->tris` — re-file from the cached
    tris and skip re-triangulation (they were regenerated by `ensure_node_tris`
    or are marked `Spatial_RegenTris`; only re-triangulate flagged leaves).
  - `FaceProxy::calc_center()` per face → derive centroid from the cached
    tris in the same pass.
  - Batch `OrderedSet` (`unique_verts`/`unique_faces`) inserts where the
    2026-06 dyntopo profile showed churn.
- **M2.b candidate-level parallelism.** Split candidates are disjoint leaf
  subtrees; per-candidate work only touches (1) its own subtree nodes, (2)
  its own verts'/faces' `.spatial.{v,f}.node` column entries (disjoint), (3)
  the shared `nodes` vector / `node_idmap` via `alloc_node`, and (4)
  `leafCacheDirty_`-style flags. Plan:
  - `thawTopo` + candidate list snapshot stay serial (already at top).
  - Guard `alloc_node` with a small mutex *or* per-task id-block reservation
    (atomic fetch-add over a pre-grown `node_idmap`); M0's
    recursive-split counter says how many nodes a candidate allocates and
    therefore how contended a plain mutex would be. Splits do large work
    between allocations, so a mutex is likely fine — measure first.
  - Dirty flags: set per-candidate results into a per-task scratch, OR into
    the shared flags after the join.
  - **Determinism:** parallel completion order makes node ids/`nodes` order
    nondeterministic. The 2026-06 dyntopo perf pass rejected "big levers" on
    parity grounds — so this option ships only with a deterministic
    renumbering epilogue: after the join, renumber each candidate's new
    subtree nodes in candidate order (serial, O(new nodes); remap
    `node_idmap` + the candidate's own `.node` column entries), restoring
    run-to-run identical trees. If renumbering turns out to cost a
    meaningful fraction of the win, fall back to M2.a-only and re-evaluate.
- **M2.c reduce downstream cost instead.** Every split marks
  `Spatial_RegenGPU`, feeding M1's regen bill. If M0 shows a large share of
  regens caused purely by splits of *clean* geometry (rebalance, not brush
  edits), teach `split_node` to preserve/reslice the owner's existing GPU
  buffers instead of flagging a full rebuild. Only pursued if M0 data
  supports it.
- **Gate:** same test matrix as M1 plus `test_dyntopo_cascade` / `_budget` /
  `_smooth`; `bench_dyntopo` split-count parity vs baseline (identical
  splits/flips/rounds); deferred-split phase ≥2× faster on the 480k
  workload with no downstream-phase regression.

## M3 — integration measurement

- Re-run all three workloads (120k, 480k per-dab, collapse-heavy) + one
  larger run (~1–2M tris, subdivs=300) and record before/after tables here.
- Verify interactive feel on the NW.js app (native backend) with a live
  dyntopo stroke — headless scripts can't catch frame-loop stalls
  (per `documentation/debugging.md` profiling guidance).
- Full `node make.mjs test` + `sbrush-verify` untouched-path sanity.

## M4 — cleanup

- Rip out **all** `CLAUDENOTE:` profiling scaffolding in
  `source/spatial/spatial.cc` / `spatial_gpu.cc` (the `prof` namespace, the
  phase scopes, sub-phase timers), replacing anything still worth keeping
  with ≤3-line permanent comments per the repo comment rules.
- Update `documentation/spatial.md` update-pipeline section (which phases are
  parallel, the plan/fill split, the split renumbering pass if M2.b shipped).
- Keep the profiling scripts in `tests/scripts/` (they're cheap and useful
  for regression A/Bs).

## Risks / invariants to respect

- **GPU partition invariants** (spatial.md): every face rendered exactly
  once; `.spatial.{v,f}.node` coverage complete (`sum unique_verts == count`).
  The orphan-vert recovery in `split_node` is the guard — it must stay
  inside the per-candidate unit in M2.b.
- **`GPUManager` is not thread-safe** and stays main-thread-only: M1 keeps
  every `createBuffer`/`alloc::Delete` of buffers in serial stages. (The
  per-thread-manager + `merge()` design was considered and shelved — Buffer
  holds a `GPUManager&`, and old-buffer deletion notifies backend observers,
  so it needs more surgery for little extra win over serialize-allocate.)
- **WASM builds are single-threaded** — all new parallel sections need the
  existing `NO_PARALLEL_FOR` fallback shape.
- **Frozen topology:** the split pass thaws before touching face/loop links;
  M1's plan stage must call `regen_node_tris` only from the serial stage for
  the same reason.
- **Parity:** meshlog/undo tests and cross-backend A/Bs assume deterministic
  spatial behavior; any nondeterminism introduced by parallelism must be
  erased before the phase ends (M2.b renumbering), not papered over in tests.
