# Dynamic Topology — Implementation Plan

## Status (2026-06-01)

The whole CPU correctness path is **done and tested**:

- **M1–M4 complete.** Operators (`edge_split`/`edge_collapse`), the CPU driver
  (`source/dyntopo/dyntopo.h`), debug-app verbs, attribute interpolation,
  incremental spatial node-ownership, and meshlog undo/redo across topology
  churn — all committed with unit tests. Three latent upstream bugs were fixed
  along the way (mesh disk/radial change-events, `OrderedSet::remove`, the
  meshlog TEMP-attr leak into undo).
- **M5** (debugging protocol) and **M6** (GPU-assist) — not started; both
  independent of the perf work below.
- **M7** — partial. Profiling is done (`bench_dyntopo` verb) and identified the
  cascade as the perf wall; the `edge_flip` operator landed as its foundation.
  The **perf + tuning** half of M7 is expanded in its own subplan,
  [`dyntopo-m7-cascade.md`](dyntopo-m7-cascade.md), and is the next runnable
  work (the *parity* half of M7 is blocked on M6). `edge_flip` and
  `bench_dyntopo` are profiling-driven additions not in the original M7 below.

## Context

This is the build-out plan for the dynamic-topology (dyntopo) feature whose
design and GPU-offload analysis live in
[`../dynamic-topology.md`](../dynamic-topology.md). Read that first — it
establishes the load-bearing facts this plan assumes:

- Per-dab remeshing is **local and small** (hundreds–low-thousands of edges
  under the cursor), so the CPU path is built first and may suffice; the GPU is
  for *data locality / sync*, not FLOPs.
- The architecture is a **hybrid**: GPU marks candidates + interpolates attrs;
  the **CPU owns topology mutation** (half-edge restitch, freelist, manifold
  checks); a compact list crosses the seam.
- Triangles only. Non-tri faces an operation produces are triangulated. No
  quad-interpolation rules.

### Decisions locked from the design phase

- **Operators**: edge split, edge collapse, optional Delaunay edge flip
  (quality), triangulate. No vertex smoothing inside dyntopo (the smooth brush
  already exists and runs separately).
- **Parallelism**: maximal-independent-set rounds over the candidate queue
  (the conflict-avoidance scheme that also maps to the GPU later).
- **Module location**: a new top-level `source/dyntopo/` for the per-dab driver
  / queue / round scheduler (matches the granularity of `brush/`, `spatial/`,
  `meshlog/`, `remesh/`). The low-level operators stay header-only in
  `source/mesh/utils/`.
- **GPU assist is Vulkan-gated and opt-in**; the CPU path is always
  authoritative (WebGPU/WASM can't share buffers or scatter-read — see the
  design doc §5).

### Module boundary

```
source/mesh/utils/      edge_split.h (new), edge_collapse.h (extend),
                        triangulate.h, delaunay.h          ← primitive ops
source/dyntopo/         dyntopo.{h,cc}    per-dab driver, L_min/L_max policy
                        queue.{h,cc}      candidate marking + queue
                        rounds.{h,cc}     independent-set selection + apply
                        bindings.{h,cc}   reflection registration
source/brush/           execBrush dab loop calls dyntopo pre/post-pass
source/debug/           dyntopo_* verbs + the debug protocol (M5)
```

---

## Critical files and utilities to reuse

- **Operators**: `source/mesh/utils/edge_collapse.h` already has
  `collapseEdge(Mesh&, int edge, …)` that **reconstructs affected faces rather
  than splicing cycles** (the robust strategy) — extend it to return the
  created/killed element ids for the log and node-ownership updates.
  `triangulate.h` / `delaunay.h` for re-tri and flips.
- **Topology mutation**: `Mesh::make_vertex/edge/face`, `kill_*`
  (`source/mesh/mesh.{h,cc}`) — freelist allocs that fire `MeshCallbacks` and
  bump `topo_stamp`.
- **Freeze/thaw**: `source/mesh/mesh_topo_cache.{h,cc}` — `freezeTopo` /
  `thawTopo`; batch a dab's mutations inside **one** thaw.
- **Undo**: `meshlog::LogChunkTopo` (`source/meshlog/meshlog_base.h`) — per
  element `(origin, fate, begin/end body)`.
- **Spatial**: `Spatial_RegenTris` / `Spatial_RegenGPU` flags, `unique_verts`/
  `unique_faces`/`other_*`, `.spatial.{v,f}.node` attrs
  (`source/spatial/spatial_attrs.h`), `SpatialTree::update()`.
- **Attrs**: `BuiltinAttr<…>` + the paged SoA `AttrData<T>`
  (`source/mesh/attribute*.h`) — interpolation walks the attr group generically.
- **Driver harness**: `source/debug/` `Scene` + `script::run`, `StrokeProfiler`
  (`source/debug/profile.h`), gdb pretty-printers (`tools/gdb/`).
- **GPU compute (M6)**: `source/vulkan/vk_compute.{cc,h}`
  (`prepareDab`/`recordDab`/`readbackVerts`), `source/brush/compiler/`,
  `source/brush/kernels/ir/intrinsics.cc` (where atomics must be added).

---

## Milestones

Each milestone is independently testable end-to-end through `debug_app`
(headless JSON asserts where possible; screenshots where visual). Match the
existing `tests/scripts/` + `tests/test_debug_script.cc` pattern.

### M1 — Primitive operators + integrity tests

- Add `source/mesh/utils/edge_split.h`: insert a midpoint vertex, split the
  ≤2 incident triangles, interpolate all vertex attrs at `t=0.5`, return
  created/killed ids. Use the **`mesh-topo-op` agent** — it scaffolds the
  operator *and* a randomized integrity-checked test under `tests/` together.
- Extend `collapseEdge` to (a) check the **link condition** and refuse
  non-manifold collapses, (b) report created/killed ids, (c) interpolate the
  surviving vertex's attrs from the collapsed pair (default: keep `v_keep`,
  optional midpoint).
- Tests: randomized split/collapse/flip storms on a subdivided cube + sphere;
  after each op assert manifoldness, Euler characteristic deltas, no orphan
  edges/corners, attr finiteness. Run under the leak-tracking allocator.

**Exit**: `node make.mjs test` passes new `test_edge_split` / `test_edge_collapse`
randomized suites.

### M2 — CPU dyntopo driver (Wave 0 core)

- `source/dyntopo/`: `DynTopoParams { L_min, L_max, mode∈{subdivide,collapse,both} }`
  derived from brush radius (e.g. `L_max = detail_size`, `L_min = 0.4·L_max`).
- `queue`: mark candidate edges within the dab's sphere — split if
  `len > L_max`, collapse if `len < L_min`. Seed deterministically.
- `rounds`: select a maximal independent set (no shared face; for collapse, no
  shared 1-ring vertex), apply in parallel, repeat until the queue drains
  (cap rounds; log if capped — no silent truncation).
- Per-dab control flow: `thawTopo` once → open `LogChunkTopo` → rounds →
  close log → mark touched nodes dirty. Wire into the `brush::CommandExecutor`
  dab loop as a pre-pass (before the deform kernel) behind a brush flag.
- **debug_app verb** `dyntopo` (`detail=F min=F mode=both`) to toggle/configure,
  and have `stroke` / `stroke_path` honor it. Add `assert_manifold`.

**Exit**: `dyntopo detail=… ; stroke_path …` on a coarse cube produces a
locally-refined, manifold mesh; `dump_state` vert/face counts grow under the
brush and nowhere else; `assert_manifold` passes.

### M3 — Attribute interpolation + spatial incremental ownership

- Generic attr interpolation across the whole `AttrGroup` for new verts (covers
  the two `float4` targets from the perf budget). Split = lerp; collapse =
  weighted by the survivor policy.
- **Incremental** node ownership: instead of a full node tri-regen, insert new
  verts/faces into the owning leaf's `unique_*` sets and update
  `.spatial.{v,f}.node` as ops apply; only set `Spatial_RegenTris/GPU` on
  leaves that actually changed. This is the suspected hot spot (design doc §2) —
  measure node-regen cost before and after with `StrokeProfiler`.

**Exit**: a stroke over a vertex-colored region keeps colors smooth across new
geometry (screenshot + JSON attr dump); profiler shows per-dab spatial-update
cost flat as the queue grows.

### M4 — Undo / redo across topology churn

- Ensure every created/killed element in a dab lands in the dab's
  `LogChunkTopo`; respect the **no-edit-between-detach/reattach** invariant.
- One stroke = one undo step (share `beginStep`/`endStep` across dabs, as the
  interactive path already does).
- Tests: stroke → undo → assert mesh byte-identical to pre-stroke signature;
  redo → assert identical to post-stroke; randomized stroke/undo/redo storms.

**Exit**: `stroke_path …; dump_state a.json; undo; dump_state b.json` with
`b.json` == pre-stroke reference; randomized undo storm passes under the
leak allocator.

### M5 — Debugging protocol (see dedicated section below)

Deterministic seeding + per-op JSONL trace + an optional stdin/socket REPL
mode on `debug_app`, plus dyntopo introspection verbs. Built here because M6+
debugging (parallel conflicts, CPU↔GPU parity) is impractical with
batch-script-and-rerun alone.

**Exit**: `debug_app --serve` accepts line commands, single-steps the dyntopo
queue, and emits a replayable `dyntopo-trace.jsonl`; a capped-round or
non-manifold event is localizable from the trace without a rebuild.

### M6 — GPU-assisted marking + interpolation (Wave 1, Vulkan-gated)

- Add atomic intrinsics to the sbrush stack (`intrinsics.cc` + lexer/parser/
  `emit_wgsl`) **or** a hand-written WGSL marking kernel (design doc §7 Path A
  vs B; start with B to de-risk, migrate to A for verification parity).
- GPU kernel marks split/collapse candidates against resident `co_`/`mask_`,
  atomic-appends a compact candidate list, CPU reads it back; new-vert attr
  interpolation runs on GPU since attrs are resident. CPU still mutates.
- Gate behind the Vulkan backend; WebGPU/WASM keeps the M2 CPU marking path.

**Exit**: a parity harness (M7) shows GPU-marked and CPU-marked candidate sets
agree on the same scene; per-dab bus traffic is a compact list, not a region
re-upload (verify via buffer-size counters).

### M7 — Parity, perf, and tuning

> The **perf + tuning** track is expanded, with profiling findings, into
> [`dyntopo-m7-cascade.md`](dyntopo-m7-cascade.md) (the per-dab cost is dominated
> by an ~8× over-refinement cascade, not the spatial tree). The **parity** track
> below is blocked on M6.

- **Parity**: `sbrush-verify`-style A/B — run identical strokes through
  CPU-only and GPU-assisted dyntopo, canonicalize the result mesh (sorted
  vertex/face signature, like `edge_collapse.h`'s `faceKey`) and diff.
- **Perf**: build a 5 M-tri scene, drive interactive strokes with
  `--profile`; confirm ≥25 fps and localize any hitch (suspects: VBO repack,
  node ownership, freeze/thaw). Add throwaway SPIKE instrumentation per
  CLAUDE.md, then rip it out.
- **Tuning**: leaf rebalancing when a leaf's face count grows past target
  (Blender-style post-stroke node split), `L_min/L_max` hysteresis to avoid
  split/collapse thrash on a stationary cursor.

**Exit**: 5 M-tri interactive sculpt holds ≥25 fps on the reference laptop;
CPU/GPU parity diff is empty modulo fp tolerance.

---

## The debugging protocol question

**Recommendation: yes, but a lightweight one — not a full DAP / custom wire
protocol.** Three tiers, in priority order; build tiers 1–2 in M5, treat tier 3
as optional.

### Why the current tooling isn't enough for dyntopo

The existing harness (batch text script → final JSON dump + PNG, gdb for
crashes) was built for *deterministic, single-shot* brush strokes. Dyntopo
breaks both assumptions:

- **Many ops per dab, stateful.** A dab applies dozens of split/collapse/flip
  ops across several independent-set rounds. A bug (non-manifold result,
  orphaned corner, attr NaN, capped round) surfaces *between* ops; a
  before/after dump of the whole dab can't localize it.
- **Convergence debugging needs mid-stroke inspection.** Finding the offending
  op by editing a script and re-running from scratch loses the exact queue /
  round state. You want to *pause at op N, query, step*, not rebuild.
- **CPU↔GPU parity (M6+)** needs the two paths driven over the identical scene
  with structured, diffable per-op output.

### Tier 1 — Deterministic seeding + per-op JSONL trace (highest value, lowest cost)

Make the queue/round scheduler take an explicit seed (no `Math.random`/wall
clock — the engine already forbids those). Behind a `dyntopo.trace` flag, emit
one JSONL record per topology op: `{dab, round, op, edge, verts_before,
verts_after, created:[…], killed:[…], manifold_ok, attr_finite}`. This is the
single highest-leverage item — it turns "the mesh is broken after this stroke"
into an evenly-indexed, replayable event log, exactly like the SPIKE-log
pattern in CLAUDE.md turned the perf hitch into labeled lines. Post-mortem
analysis needs no stepping and no protocol.

### Tier 2 — stdin/socket REPL mode on `debug_app` (medium cost, high value)

Extend the existing script engine — which already has a clean verb dispatcher
(`execVerb`) and an in-process `script::run` — into an interactive line
protocol: `debug_app --serve [--port N | stdin]` reads one verb per line and
writes a JSON response per command, so an external driver (a test, or Claude)
interleaves commands and inspects state **without re-running from scratch**.
This is a small extension, not a new protocol: reuse the verbs, add a transport
and a response channel. New introspection verbs:

- `dyntopo_step [n=1]` — apply n queued ops, return the trace records.
- `dump_topo edge=E` / `vert=V` — local 1-/2-ring as JSON.
- `dump_queue` / `dump_round` — pending candidates, the chosen independent set.
- `break_on event=non_manifold|capped_round|attr_nan` — pause + return control.
- `assert_manifold` / `assert_euler` — inline predicates.

Prefer **stdin/stdout newline-delimited JSON** over a socket for v1 (simpler,
no port management, matches how tests already call `script::run`); add a TCP/
named-pipe transport only if a GUI client materializes.

### Tier 3 — drive the interactive Vulkan loop remotely (optional)

The live-path-only perf hitches (CLAUDE.md "periodic hitch") only reproduce in
`--interactive`. A command channel that injects synthetic strokes into the
interactive loop and streams back per-frame / per-op `StrokeProfiler` splits
would let an external driver reproduce and bisect a live hitch headlessly.
Build only if M7 surfaces a hitch that batch runs can't reproduce — otherwise
the setup-script-plus-`--interactive` handoff already documented suffices.

### Explicitly out of scope

- A full **Debug Adapter Protocol** implementation or a separate GUI debugger
  client — far more than this needs; gdb + pretty-printers already cover crash
  inspection (`tools/gdb/sculptcore.py`, `sc-break-bad-topo`).
- A **binary wire protocol** — JSONL is deterministic, diffable, and
  human-readable; binary buys nothing at these data rates.
- Reusing the **NW.js CDP** path (`nwjs/cdp.mjs`) — that debugs the JS app, not
  the native `debug_app`; the stdin REPL is the native analog.

---

## Testing strategy

- **Unit (headless, GPU-free)**: `tests/test_edge_split.cc`,
  `tests/test_edge_collapse.cc`, `tests/test_dyntopo.cc` via in-process
  `script::run` — randomized op storms + manifold/Euler/attr asserts under the
  leak allocator.
- **Script fixtures**: `tests/scripts/dyntopo_*.txt` committed alongside each
  fixed bug (the documented Claude workflow), with `assert_manifold` /
  `assert_verts` / `dump_state` golden JSON.
- **Visual**: `screenshot` golden PNGs for refinement-pattern regressions
  (epsilon diff).
- **Parity (M7)**: CPU vs GPU canonical-signature diff.
- **Perf (M7)**: 5 M-tri interactive `--profile` run, ≥25 fps gate.

## Risks / open questions

- **Node-ownership churn** is the most likely perf cliff; M3's incremental
  update must avoid full node regen. If it can't, fall back to coarser
  per-stroke (not per-dab) regen and measure.
- **Collapse robustness**: the link condition is necessary but degenerate/
  near-coplanar fans can still produce slivers; the Delaunay flip pass and a
  minimum-angle guard mitigate. Keep collapse refusable and logged.
- **Freeze/thaw cost per dab** — if a single thaw/refreeze per dab is too
  expensive at 5 M tris, consider staying thawed for the whole stroke and
  re-freezing on stroke end; measure in M2.
- **GPU atomics infra** (M6) is the largest single lift; Path B (raw WGSL)
  de-risks it before committing to the DSL extension (Path A).
- **Determinism under parallel rounds**: independent-set selection must be
  order-stable given the seed, or the JSONL trace and CPU/GPU parity become
  non-reproducible.
