# Dynamic Topology — Design & Implementation Plan

## Goal

Local remeshing of the sculpted surface **under the brush cursor**, in the
style of Blender's DynTopo or ZBrush's Sculptris: geometry below a dab is
subdivided where edges are too long and collapsed where they are too short,
so detail tracks the brush radius without the user pre-subdividing. We work
with **triangles only** — no quad-interpolation rules (they buy nothing for
free-form sculpt), and any non-triangle face touched by an operation is
triangulated. Custom per-vertex attributes (e.g. `float4` color) are
interpolated onto newly created geometry.

**Performance target** (shared with the rest of the engine): usable
sculpting (≥25 fps on a laptop) on a 5-million-triangle mesh carrying two
interpolated `float4` attributes.

---

## Status — post-M7 re-evaluation (2026-06)

**The CPU path is built and the 5 M-tri / ≥25 fps target is met with no GPU
offload.** Dynamic topology shipped through milestones M1–M7 (see
[`plans/dyntopo-m7-cascade.md`](plans/dyntopo-m7-cascade.md)). The sections below
are the original design and remain a faithful record of the reasoning; this
banner records where *measurement* corrected that reasoning and what it means for
the GPU work — which is now **optional**, not required.

**What held.** "Work is local and small per dab" ✓. "CPU mutation stays
authoritative" ✓. "Profile before reaching for the GPU" ✓ — and vindicated: every
a-priori bottleneck guess was wrong.

**What measurement changed:**

1. **The bottleneck was the edge surgery, not the spatial bookkeeping.** §7 named
   "spatial node-ownership updates and VBO repack" as the prime suspects; in fact
   incremental `SpatialTree::update()` is **2–11 ms at 5 M** (M7.6), while the
   round loop dominated. The driver is **cascade depth × per-split cost**, and
   per-split cost is **valence-driven and cache-bound**, not arithmetic.

2. **Edge *flip* is load-bearing for performance, not "optional, quality."** A
   length-criterion flip sweep (M7.2) broke the refinement cascade: a 5 M
   aggressive dab went **1600 → 171 ms**, per-split cost **~16×** lower, max
   valence **30 → 9**, and deep cascades that never converged now converge. It is
   the single biggest lever in the feature — and it is pure CPU. The real-time
   levers are **graded target + flips + tangential smoothing + a per-dab split
   budget** (M7.1a/M7.2/M7.4 + the valve), none of them GPU.

3. **The stages the plan calls cheap-to-offload aren't the bottleneck; the one it
   calls hardest is the one you'd need.** Stage A/B (mark/compact) is cheap once
   localized (round-0 spatial seeding), so offloading it (Wave 1) doesn't touch
   the dominant mutation+cascade cost. Only Stage C (mutation, the "build last"
   wave) is on the hot path — and three facts make GPU mutation a net negative
   now: the **split-budget valve** already bounds per-frame work on the CPU; GPU
   atomic-append ordering is **nondeterministic**, breaking the seeded tests and
   `sculptcore_parity`; and a GPU mutation that bypasses `MeshCallbacks` loses the
   **incremental spatial ownership** (M7.6) and pays a **~68 s** full tree rebuild
   at 5 M instead of 2–11 ms.

4. **Quad input — not dyntopo — is the remaining penalty; the fix is a manual
   triangulate, not auto-triangulation.** Validated in the native (desktop) backend
   on a real ~5 M-tri mesh: an all-triangle mesh sculpts at **~32 fps (≈31.7 ms/
   dab)**, but a *quad/n-gon* mesh is far slower for two structural reasons — (a)
   each dab runs a per-region triangulate prepass, and (b) the all-triangles gate
   (`mesh.n_ngon_faces == 0`, an exact live counter) only fires once the *whole*
   mesh is triangle, so until then every dab scans its region to find n-gons; on
   top of that, incrementally triangulating under the dab leaves the quad-built
   BVH unbalanced. Auto-triangulating on first contact was rejected — a silent,
   whole-mesh topology change mid-stroke is surprising and not cleanly undoable per
   dab. Instead triangulation is a **manual, undoable `litemesh.triangulate` ToolOp
   + header button** that does one clean, *balanced* tree rebuild; a planned
   viewport footer tip ("large mesh would be faster if triangulated") will surface
   it on large non-tri meshes (see the TODO in `tools/sculptcore.ts`). Two CPU
   micro-opts landed alongside: a generation-stamped dense-int membership set
   (`detail::GenSet`) replaced the per-round `Set<int>` hash sets the profiling
   pinned as scan/flip/MIS rehash spikes (zero per-dab allocation), and the
   `n_ngon_faces == 0` gate skips the triangulate prepass wholesale on the common
   all-triangle dab. The app's default per-dab split budget was also corrected
   `0` (unlimited — triggered the round-2 cascade) → **1024**.

**Forward GPU guidance (supersedes the §7 staging and §8 bottom line):**

- GPU offload is **no longer a performance requirement.** Treat it as an
  optimization for one scenario only: the Vulkan compute-brush path where the mesh
  is already GPU-resident and a CPU round-trip is *measured* as a stall.
- If you build anything, build **Stage D (attribute interpolation for new verts)
  first, and maybe only** — embarrassingly parallel, **deterministic** (no
  atomics), no new infra, and it directly serves the "2 × `float4` interpolated"
  goal. Still gate it on a measured stall; it is cheap on the CPU too.
- **Demote Wave 1** (GPU mark/compact) to "only if `co_`/`mask_` readback is a
  measured stall on the resident-mesh path." If built, **sort the compacted list
  by edge id** before independent-set selection or determinism/parity is lost.
- **Do not build Wave 2** (GPU mutation) outside a throwaway spike: unnecessary,
  determinism-breaking, and unable to feed M7.6's incremental ownership without
  re-implementing it on the GPU (else the 68 s rebuild).
- The highest-ROI optimization left is **CPU, not GPU: data locality — see §9.**

---

## 1. Framing: the workload is local and small

The single most important design fact: **dyntopo only remeshes geometry the
dab overlaps.** At a 5 M-tri target, one dab touches a handful of BVH leaves
and on the order of hundreds–low-thousands of edges. The remeshing
*arithmetic itself is tiny and local* — it is **not** throughput-bound the
way the brush-deform or normal-recompute passes are.

This reframes the "offload to the GPU?" question. There are three possible
motivations and only one is about raw compute:

| Motivation | Real? | Notes |
|---|---|---|
| Remeshing needs GPU FLOPs | **No** | Work is local/small per dab |
| Mesh lives on the GPU (compute-brush path) so CPU remeshing forces a re-upload / round-trip | **Yes** | This is the actual reason to care |
| Keeping spatial VBOs + normals coherent after a topo edit is the real cost | **Yes** | And this is *already* a GPU concern |

So the real question is **where the authoritative mesh lives during a stroke,
and how to keep CPU topology surgery from stalling the GPU brush/render
pipeline** — not whether edge-split/collapse can run as a compute kernel. The
answer this plan lands on is a **hybrid**: the GPU does the embarrassingly
parallel *decision* and *attribute* work; the CPU owns the *topology
mutation*; a compact seam carries data between them.

---

## 2. Existing building blocks to reuse

The codebase already contains most of what a CPU implementation needs.

- **Topology mutation primitives** (`source/mesh/mesh.{h,cc}`):
  `make_vertex` / `make_edge` / `make_face` / `kill_vertex` / `kill_edge` /
  `kill_face`. These are freelist allocations that bump `topo_stamp` and fire
  `MeshCallbacks` hooks; the spatial tree listens and marks affected nodes
  dirty. Adding and removing geometry at runtime is **cheap and already
  wired**.
- **Edge collapse** (`source/mesh/utils/edge_collapse.h`): `collapseEdge(Mesh&,
  int edge, …)` already exists and, notably, **reconstructs the affected faces
  rather than splicing the radial/disk cycles in place** — exactly the
  deferred/rebuild strategy the parallel-remeshing literature recommends for
  robustness. Extend this rather than writing collapse from scratch.
- **Triangulation** (`source/mesh/utils/triangulate.h`) and
  **Delaunay** (`source/mesh/utils/delaunay.h`): use for re-triangulating
  faces left by an operation and for optional edge-flip quality passes.
- **Edge split**: no dedicated util yet — add `source/mesh/utils/edge_split.h`
  (the `mesh-topo-op` agent is the right tool: it scaffolds a new
  `source/mesh/utils/` operator plus a randomized integrity-checked test under
  `tests/`).
- **Undo** (`source/meshlog/meshlog_base.h`): `LogChunkTopo` already records
  per-element `(origin, fate, begin/end body)` and replays creates/kills +
  attribute swaps. Created geometry is undoable today.
- **Spatial change detection** (`source/spatial/`): `make_*`/`kill_*` callbacks
  set `Spatial_RegenTris` / `Spatial_RegenGPU` on touched nodes;
  `SpatialTree::update()` rebuilds only those (tris → normals → partition →
  VBO regen).
- **GPU normal recompute**: the Vulkan path already has a `GpuNormalPass`
  (`source/vulkan/`, `source/debug/gpu_stroke.{cc,h}`) that recomputes
  vertex/face normals after a dab.

### Frictions to design around

1. **`freezeTopo()` conflict.** During a stroke the mesh is topo-frozen (live
   TOPO link pages freed, a CSR 1-ring snapshot kept; see
   `source/mesh/mesh_topo_cache.{h,cc}`). Every `make_*` auto-thaws. We do
   **not** want a thaw per edge-op — **batch all of a dab's mutations, thaw
   once, mutate, refreeze.**
2. **Node ownership maintenance.** New verts/faces must be inserted into the
   owning leaf's `unique_verts` / `unique_faces` (and `other_*` for boundary
   geometry), and the `.spatial.{v,f}.node` attributes updated, or the next
   `update()` mis-renders. Today regen is node-granular; per-dab churn wants
   *incremental* ownership updates rather than a full node tri-regen — this is
   a likely real bottleneck and should be profiled early (see §7).
3. **Undo invariant.** No topology edits between `detachAttr()` /
   `reattachAttr()` — the meshlog stash does not track element-count changes.
   Wrap a dab so all topo mutation happens inside one log chunk, outside any
   detached-attr window.

---

## 3. Pipeline decomposition

Split a dab into stages and place each where it belongs:

| Stage | Parallel? | Home | Why |
|---|---|---|---|
| **A. Mark** edges to split (len > `L_max`) / collapse (len < `L_min`) under the cursor | Embarrassingly | **GPU** (or CPU) | Pure per-edge predicate; mirrors the existing brush compute pass |
| **B. Compact** marked edges into a tight list | Scan / atomic append | **GPU** → readback | Only a small list crosses the bus |
| **C. Mutate** topology (alloc, restitch, triangulate) | Hard (conflicts, dyn-alloc) | **CPU** | Pointer-chasing, freelist, manifold/link-condition checks |
| **D. Interpolate** custom attrs onto new verts | Embarrassingly | **GPU or CPU** | Trivial once positions are known; GPU if attrs already resident |
| **E. Spatial regen** (tris, slices, VBO repack) | Per-node parallel | **GPU-resident already** | `regen_gpu_node` repacks VBOs |
| **F. Normal recompute** | Embarrassingly | **GPU already** | `GpuNormalPass` exists |

The key observation: **A, B, D, E, F are already GPU-friendly or already on
the GPU. Only C is genuinely hard to put on the GPU — and C is the cheapest in
absolute work.** That asymmetry is the entire argument for the hybrid.

---

## 4. The CPU mutation core (stage C)

This is the part that stays on the CPU permanently. It's cheap, it's
correctness-critical, and the GPU-friendly representations that would replace
it (see §6) don't support free-form collapse.

### Operator set (tris only)

- **Edge split**: insert a midpoint vertex; each of the (≤2) incident triangles
  becomes two. Interpolate attrs at `t = 0.5`. Mild conflict surface: two edges
  of the *same* triangle split simultaneously conflict.
- **Edge collapse**: merge `v_kill` into `v_keep` via `collapseEdge`; must
  satisfy the **link condition** (the intersection of the two endpoints'
  vertex-link must be exactly the two opposite verts) to stay manifold.
  Adjacent collapses conflict heavily.
- **Edge flip** — **load-bearing for performance, not optional** (see the post-M7
  banner). A length-criterion flip (flip the shared edge to its shorter diagonal
  when the quad stays convex) run each round breaks the split-spoke cascade; M7.2
  measured it as the single biggest perf lever (5 M dab 1600 → 171 ms, valence
  30 → 9). The monotone length criterion was chosen over the Delaunay one
  (`delaunay.h::inCircumcircle`) because it can never *lengthen* an edge and so
  can't manufacture split work — the valence criterion did, and was rejected.
- **Tangential smoothing** (M7.4): after the flips, slide region verts toward
  their 1-ring's area-weighted centroid in the **tangent plane only** — equalizes
  triangle sizes / kills slivers without shrinking the surface, and by evening
  edge lengths actually *reduces* split work. Boundary verts fixed; each move
  clamped so a thin triangle can't fold. Completes the Botsch-Kobbelt quartet.
- **Triangulate**: any non-tri face an operation produces is triangulated via
  `triangulate.h`.

### Parallelism: independent-set rounds

Blender's dyntopo runs the topology edits essentially serially (it parallelizes
only the vertex *deform*), and that serialization is its known bottleneck. We
can do better even on the CPU, with the same structure that later maps to the
GPU:

1. Build the candidate edge queue (stage A/B).
2. Select a **maximal independent set** of candidate edges — no two sharing a
   face or (for collapse) a 1-ring vertex.
3. Mutate that set in parallel.
4. Repeat until the queue drains. Converges in a handful of rounds because the
   active region is small.

This is the same conflict-avoidance the GPU simplification literature uses
(independent sets / graph coloring / lazy update tables); building it on the
CPU first means the hard correctness work is done once and reused.

### Per-dab control flow

As built (M7), with the cascade-taming operators the original sketch omitted:

```
begin dab:
  thawTopo() once
  open LogChunkTopo
  seed round 0 from the in-region spatial leaves' verts (local, not a mesh scan)
  while frontier not empty and rounds < max_rounds:
    gather candidates over the frontier; target edge len is GRADED (M7.1a)
    select a maximal independent set
    split / collapse the set (edge_split, collapseEdge); interpolate attrs
    FLIP sweep over the touched edges        ← M7.2, breaks the cascade
    tangential SMOOTH the touched verts       ← M7.4, evens triangle sizes
    incrementally update node ownership (add_face_at / merge — M7.6)
    if splits >= max_splits: stop (budget valve); the next dab finishes the region
    frontier ← endpoints touched this round
  close LogChunkTopo
  stay thawed for the stroke (keepTopoThawed); refreeze at stroke end
SpatialTree::update()  → deferred leaf rebalance, tris, normals, VBO repack
```

The flip + smooth + graded-target + budget were *not* in the original sketch and
are exactly what made the 5 M target reachable — the GPU was not.

---

## 5. Why full-GPU topology mutation is the hard part

Both the codebase survey and the literature converge here.

- **Dynamic allocation.** GPU mesh mutation needs atomic-append for new verts/
  edges/faces. The stack has **no atomics exposed today** — not in the sbrush
  intrinsic table (`source/brush/kernels/ir/intrinsics.cc`), not in the DSL,
  and there are no append/consume buffers. WGSL and Vulkan both *support*
  `atomicAdd` on `var<storage, read_write>`; append is `idx =
  atomicAdd(&count, 1); arr[idx] = …` against a pre-sized buffer. A bounded but
  real infra addition.
- **Conflict resolution.** Split is mild (rewrites 2 incident tris); collapse
  is brutal (deletes a vertex, merges a 1-ring, must preserve manifoldness, and
  adjacent collapses conflict). Naïve parallel collapse "did not succeed in
  preventing self-intersections" in the literature; the working approaches use
  independent sets / graph coloring or **lazy update tables** (record per-edge
  intent, apply in a second pass to break the data dependency).
- **Half-edge mutation on the GPU is genuinely painful.** Our mesh is a full
  radial/disk half-edge-class structure (corners with `radial_next/prev`, edges
  with a 4-way `disk`). Concurrently mutating circular linked lists is the worst
  case. GPU-friendly schemes avoid it entirely by operating on indexed triangle
  soup (and rebuilding adjacency) or on a hierarchical subdivision encoding
  (§6) — neither of which is a mutable half-edge graph. Note our spatial VBOs
  are *already* expanded triangle soup, convenient for the renderer but **not**
  the structure you'd mutate.
- **Determinism (post-M7).** The seeded independent-set selection, the per-op
  tests, and `sculptcore_parity` all require reproducible results. GPU
  atomic-append yields **nondeterministic ordering**, which changes the selected
  set and therefore the resulting mesh. Even a GPU *compaction* (Stage B) reorders
  the candidate list and would have to be **re-sorted by edge id** before
  selection to stay reproducible.
- **Incremental spatial ownership (post-M7).** What makes a dab cost 2-11 ms
  instead of a **~68 s** full rebuild at 5 M is the incremental ownership
  maintenance (M7.6: `add_face_at` / deferred rebalance / merge) driven by CPU
  `MeshCallbacks`. A GPU mutation path bypasses those callbacks and must either
  re-implement ownership maintenance on the GPU or eat the rebuild — a cost the
  original Stage E ("spatial regen - GPU-resident already") badly understates.

### Backend asymmetry (important)

- **Vulkan** compute can share buffers with the renderer and do **per-vertex
  scatter readback** (`prepareDab` / `recordDab` / `readbackVerts` in
  `source/vulkan/vk_compute.{cc,h}` and `source/debug/gpu_stroke.*`).
- **WebGPU** compute (`source/webgpu/wgpu_compute.{cc,h}`) is **batch-only**:
  full-buffer readback, no buffer sharing with a separate-API renderer. The
  browser/WASM path therefore pays a CPU round-trip for any GPU-assisted
  dyntopo.

→ The CPU mutation path must stay the **authoritative, always-available**
implementation; GPU assist is opt-in per backend (mirroring the existing
`IWasmInterface` WASM/native split).

---

## 6. Prior art and what transfers

- **Blender DynTopo / PBVH-BMesh** — closest functional analog: spatial nodes
  own vert/face lists, regen on edit, parallel deform / serial topo edits.
  Validates our architecture; its serial-edit weakness is what the
  independent-set rounds improve.
  ([roadmap](https://developer.blender.org/T73934),
  [refactor PR](https://projects.blender.org/blender/blender/pulls/104613))
- **GPU mesh *simplification* (parallel half-edge collapse)** — collapse-only
  on the GPU via **lazy update tables** and **embedded-tree collapsing**. The
  lazy-table idea is the cleanest way to make collapse parallel on either CPU
  or GPU.
  ([Springer](https://link.springer.com/chapter/10.1007/978-3-319-29817-7_10),
  [embedded tree collapsing](https://link.springer.com/article/10.1007/s00371-016-1242-z),
  [I3D 2007](https://dl.acm.org/doi/10.1145/1230100.1230128))
- **Adaptive remeshing for real-time deformation** (e.g. Palfinger,
  "Continuous remeshing") — alternate one small deform step with one
  split/collapse/flip pass; exactly the sculpt-dab loop. Good reference for
  operator ordering (split → collapse → flip → smooth) and stability.
  ([Wiley](https://onlinelibrary.wiley.com/doi/10.1002/cav.2101),
  [PaMO arXiv](https://arxiv.org/pdf/2509.05595))
- **Concurrent Binary Trees / Longest-Edge-Bisection (Dupuy)** — the most
  elegant *fully-GPU* adaptive tessellation: a bitfield binary heap, threads
  split/merge leaves with pure bitwise ops, no dynamic allocation, no half-edge
  mutation; the 2024 follow-up maps a bisector primitive onto each half-edge of
  an arbitrary polygon mesh. **But** LEB is a hierarchical subdivision of a
  *fixed base mesh* — no collapse below the base, base topology fixed,
  free-form sculpted detail doesn't map to a bisection tree. **Poor fit as the
  core**, but the reference design if we ever want a pure-GPU adaptive layer.
  ([CBT paper](https://onrendering.com/data/papers/cbt/ConcurrentBinaryTrees.pdf),
  [libleb](https://github.com/jdupuy/libleb),
  [CBT for game components 2024](https://arxiv.org/abs/2407.02215))

---

## 7. Staged plan

> **Post-M7:** the staging below is superseded by the banner at the top. Wave 0 is
> **DONE** and **met the target on its own**; Wave 1 is demoted to a measured-stall
> gate; Wave 2 is not recommended. The wave text is kept for the record, annotated.

### Wave 0 — CPU-only, correct first — **DONE (met the target)**

Independent-set split/collapse/flip queue; batch-per-dab (thaw once);
re-triangulate affected faces; interpolate attrs on new verts; `LogChunkTopo`
undo; incremental node-ownership updates; `Spatial_RegenTris/GPU`. Drive it
end-to-end through the `debug_app` script harness; add a randomized
integrity-checked test per operator under `tests/` (the `mesh-topo-op` agent
scaffolds operator + test together).

**This wave alone may hit the perf target**, because the work is local. Do not
assume the GPU is needed — **profile first** with `StrokeProfiler` (`--profile`).

> **Post-M7 correction.** This wave *did* hit the target — but the suspect named
> here was wrong. Spatial node-ownership / VBO repack turned out **cheap**
> (incremental `update()` 2–11 ms @5 M, M7.6); **the edge surgery — the round
> loop — was the cost**, dominated by the split-spoke *cascade*. The fixes were
> all CPU and none were in this original sketch: graded target (M7.1a), the
> **flip sweep** (M7.2, the big one), tangential smoothing (M7.4), incremental
> ownership + deferred rebalance/merge (M7.6), and a per-dab split-budget valve.
> The "profile first" discipline was right; the a-priori guess was not.

### Wave 1 — GPU the decision + attributes (stages A/B/D) — **demoted**

> **Post-M7.** Marking/compaction (A/B) is cheap once round 0 is seeded from the
> spatial leaves, so offloading it does **not** touch the dominant mutation+cascade
> cost — only the **Stage D** attribute interpolation is worth keeping, and only
> on the Vulkan resident-mesh path where the readback is a *measured* stall. If you
> do compact on the GPU, **sort the candidate list by edge id** before selection or
> the seeded determinism (and `sculptcore_parity`) is lost.

Add an `atomicAdd` / append-buffer capability. Two options from the brush-
compute survey:
- **Path A (parity-preserving):** extend the sbrush DSL with atomics
  (new intrinsics in `intrinsics.cc`, lexer/parser/emit support) so the
  marking kernel keeps bit-for-bit C++↔WGSL verification.
- **Path B (fast prototype):** hand-write a raw WGSL/SPIR-V marking kernel and
  wire its bindings + readback directly, bypassing the DSL.

The GPU marks split/collapse candidates against the resident `co_`/`mask_`
buffers, atomically compacts a candidate list, and reads back only that compact
list; the CPU does the mutation; attribute interpolation for new verts runs on
the GPU since the attrs are already resident. This removes the
"re-upload the whole region" round-trip — only the candidate list and the
new-vert payload cross the bus. Gate the whole assist behind the Vulkan backend
(WebGPU keeps the CPU round-trip; see §5).

### Wave 2 — GPU-assisted mutation — **not recommended (post-M7)**

> **Post-M7.** Don't build this outside a throwaway research spike. It is
> unnecessary (the target is met on CPU and the split-budget valve already bounds
> per-frame work), determinism-breaking (atomic ordering vs. seeded selection +
> parity), and it cannot feed M7.6's incremental ownership without a GPU
> re-implementation — otherwise it pays the ~68 s full rebuild. Collapse was never
> the bottleneck anyway; the split *cascade* was, and flips fixed it on the CPU.

Push collapse-set selection (independent set / coloring) or a lazy-update-table
collapse pass onto the GPU. This is where the literature's conflict-resolution
machinery lives. **Do not build this until Waves 0–1 are measured and found
wanting.** The half-edge restitch and freelist allocation stay on the CPU
regardless.

---

## 8. Bottom line

Updated after M7 shipped (the original bullets held except where measurement
corrected them — see the banner):

- The **straight C++ version was built and sufficed.** Per-dab remeshing is local
  and small, and the 5 M / ≥25 fps target is met with **no GPU offload**.
- The real-time levers were all **CPU and all in the operator/round design**, not
  the bus: a **graded target** (M7.1a), a **length-criterion flip sweep** (M7.2 —
  the single biggest win, it breaks the spoke cascade), **tangential smoothing**
  (M7.4), **incremental spatial ownership + deferred rebalance/merge** (M7.6), and
  a **per-dab split-budget valve** for graceful degradation. Flip is *not*
  optional — it is load-bearing.
- **GPU offload is now optional**, justified only by the Vulkan resident-mesh
  round-trip, and only when *measured*. If anything: **Stage D (attr interp)
  first** — deterministic, no atomics, serves the `float4` goal. **Stage A/B**
  only behind a measured readback stall, with a determinism-preserving sort.
  **Full-GPU mutation: don't** — unnecessary, nondeterministic (breaks parity),
  and it forfeits the incremental ownership that turns a 68 s rebuild into 2–11 ms.
- **CBT/LEB** remains the reference for a *separate* pure-GPU uniform-tessellation/
  LOD layer over a fixed base — a renderer concern, **not** free-form sculpt
  dyntopo. Note it; keep it off this critical path.
- Mind the **Vulkan-vs-WebGPU asymmetry**: the browser path can't share buffers
  or scatter-read, so the CPU mutation path must remain authoritative and always
  available (it now *is* the whole path).
- **The optimization that's actually left is CPU data locality** (§9): per-split
  cost is cache-bound on a 5 M mesh, and a GPU can't help pointer-chasing. That is
  where the next perf attention should go.

---

## 9. Data locality & incremental defragmentation

### 9.0 Why this is the optimization that's left

M7's per-split cost was **erratic — 0.03–1.0 ms for structurally similar work** on
the 5 M mesh, with no correlation to split count. That variance is a **DRAM
locality** signal, not an algorithmic one. The mesh is paged SoA
(`ATTR_PAGESIZE = 4096`); `make_*` draws each new element from a global freelist,
so a split drops the new vert/edges/faces into whatever holes exist — typically
far from the parent. After a stroke, a spatial leaf's geometry is smeared across
distant pages, and every disk/radial walk and `find_edge` (the per-split hot ops,
both O(valence) pointer chases) straddles cache lines. **A GPU cannot help a
pointer-chasing, cache-bound workload; tightening locality is the highest-ROI
lever remaining, and it is pure CPU.**

Two distinct wins hide here, and they want different tactics:

- **Clustering** — put a leaf's elements on shared/adjacent pages so a brush
  iteration or a split's 1-ring walk stays in-cache.
- **Hole reclaim** — collapses leave dead slots; reclaiming them shrinks the
  working set (fewer pages touched, better TLB/cache reach).

The existing `reorderForLocality()` / `computeLocalityMaps()` / `applyReorder()`
(`source/spatial/spatial.{h,cc}`) already do the **global** version: build a
per-domain permutation grouping each element next to its node-mates, apply it, and
rebuild the tree. It is **O(mesh) and deterministic** — the meshlog reorder chunk
replays it for undo — so it is the off-the-hot-path baseline, **not** a per-dab
tool. Everything below is about getting most of its benefit *incrementally*.

### 9.1 The hard constraint: moving an element means fixing every reference to it

Relocating element `x → x'` requires rewriting **every index that points at `x`**.
Those indices live in a dense web:

- **TOPO links** — `vert.e`, `edge.vs[2]`, `edge.disk[4]` (side-bit encoded:
  `(edge << 1) | side`, see `diskPack` in `mesh_types.h` and
  `dyntopoTangent.md`), `corner.{v,next,prev,radial_next,radial_prev,l}`,
  `list.f`, `face.l`.
- **Spatial ownership** — each leaf's `unique_verts/faces`
  `OrderedSet`s hold element indices (the `.spatial.*.node` attrs hold **node
  ids**, not element indices, so they are *immune* to element moves — a useful
  asymmetry).
- **Snapshots** — meshlog `LogChunkTopo` rows and the frozen-topology CSR 1-ring
  cache hold indices.

This web is why the global reorder rebuilds the tree (drops + regenerates the
`OrderedSet`s) and runs outside frozen state, logged. The practical unit of
incremental work is therefore **"remap a *closed* set atomically,"** and the
spatial leaf is the natural closed set: a leaf's **interior** (`unique_`) elements
reference only other interior elements or the leaf's **boundary** (`other_`)
elements. So you can relocate a leaf's interior into a contiguous range and patch:

1. interior↔interior links — covered by the remap itself;
2. links **from boundary elements into** the moved interior — bounded by the leaf
   *perimeter*, O(√leaf), the only non-trivial part;
3. the leaf's own `OrderedSet`s.

Everything else (the `.node` attrs, other leaves' interiors) is untouched. That
O(√leaf) boundary patch is what makes per-leaf compaction affordable, and it is
the same bookkeeping M7.6's `merge_node`/`split_node` already perform when they
re-file a leaf — which is the key to the cheapest cadence below. Any in-stroke
move must be **logged** (extend the reorder chunk) and must **invalidate or update
the CSR snapshot** if topology is frozen.

### 9.2 Prevention beats cure — locality-aware allocation (do this first)

The cheapest defrag is to not fragment. Give the allocators a **placement hint**:

- **Parent-adjacent allocation.** `splitEdge` knows the parent vert/edge; allocate
  the new element from the nearest free slot **on the parent's page** (or an
  adjacent one), falling back to the global freelist head only when that
  neighborhood is full. A split's new geometry then lands beside the geometry it
  subdivides — automatically, at ~zero cost (a hint + a short local scan of a
  per-page free bitmap).
- **Per-leaf page arenas.** Each spatial leaf owns a small set of pages and a free
  cursor; its splits draw from that arena. The deferred-rebalance split (M7.6) is
  the natural moment to assign/refresh a leaf's arena (it is already re-filing the
  leaf). When an arena fills, grab a fresh page rather than scattering.

Prevention can't reclaim *existing* fragmentation or collapse holes, but it
sharply slows the accrual — so it is the foundation the cures sit on.

### 9.3 Cure, by cadence — and defrag can run essentially whenever

Because a remap is just data movement with reference fixup, it can be scheduled
freely. The menu, cheapest/most-local first:

- **During the dab — piggyback on the M7.6 rebalance (cheapest real defrag).**
  `split_node`/`merge_node` already walk and rewrite a leaf's whole geometry when
  they re-file it. Relocating those elements into a fresh contiguous range *at the
  same time* is nearly free — the references are already in hand and the boundary
  patch is already being done. This defrags exactly where churn concentrates,
  exactly when the structure is already being touched. Strongly recommended.

- **At dab end — region compaction.** The dab's dirty leaves are known
  (`nodeSplitCandidates_` + the touched frontier). Run the per-leaf remap (§9.1)
  over just those, **gated on a fragmentation metric** (e.g. the page-span of a
  leaf's `unique_verts` ÷ its element count) so it fires only for genuinely
  scattered leaves. O(dab region); fits inside the frame the split-budget valve
  already bounded.

- **At stroke end — stroke-region compaction.** The mesh thaws at stroke end; the
  union of stroke-touched leaves is a natural, larger batch with a little slack (no
  per-frame deadline). Amortizes the per-element move cost once per stroke and logs
  **one** reorder chunk for the whole region. A good default if per-dab proves too
  granular or its logging churn is undesirable.

- **Idle / background — amortized GC cursor.** Between strokes, advance a
  persistent cursor that relocates a *bounded* number of elements per idle frame
  toward the global `reorderForLocality` target, pausing/aborting cleanly when a
  stroke begins. Trends the entire mesh toward optimal locality without ever
  blocking. The cursor order must be **deterministic** (so the result is
  independent of how it was sliced across frames) and stroke-safe. This is the
  classic incremental/GC compaction and the right home for *global* drift that the
  local cadences don't reach.

- **Page-hole compaction — footprint reclaim.** Track per-page live count; when a
  page falls below a threshold (collapse holes), evacuate its few survivors into
  the partial page nearest each survivor's leaf, then free the page. Reduces page
  count + working set and composes with clustering (pick the *nearest* target page,
  not just any). Runs incrementally — one sparse page per idle frame.

- **Scheduled full reorder — the nuclear fallback.** The existing
  `reorderForLocality` on an explicit "optimize," on load, or every N strokes,
  ideally off the interactive thread. The backstop when incremental upkeep drifts.

### 9.4 Recommended layering & what to measure

A practical stack, in priority order:

1. **Locality-aware allocation (§9.2)** — ~free, prevents most new fragmentation.
   Do this before any cure; it changes the slope, not just the level.
2. **Piggyback compaction on the M7.6 rebalance (§9.3a)** — near-zero marginal
   cost, defrags where churn is highest.
3. **Idle GC cursor + page-hole reclaim (§9.3 d/e)** — mops up global drift and
   collapse holes without touching the interactive path.
4. Hold the explicit **dab-end / stroke-end** passes (§9.3 b/c) in reserve for if a
   metric shows 1–3 aren't keeping up.

**Gate every layer on measurement — locality work is invisible except in the
tail.** Bring back the throwaway per-dab/per-frame SPIKE instrumentation
(`StrokeProfiler`, the temporary-scaffolding pattern) plus a cheap per-leaf
**page-span** metric. Defrag is working when **per-split time *variance* collapses**
and **mean per-split cost stops creeping up with stroke length**. Don't add any of
this blind; add the metric first, confirm the locality signal, then turn on the
cheapest layer that flattens it.

**Determinism & undo apply throughout:** every relocation must keep results
reproducible (the `reorderForLocality`/`buildAll` determinism the parity test and
the meshlog reorder chunk depend on) and be logged if it happens inside an
undoable stroke. The split-budget valve pairs naturally here — it frees frame
headroom that a bounded locality pass can spend.
