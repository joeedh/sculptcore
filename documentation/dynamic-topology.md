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
- **Edge flip** (optional, quality): Delaunay-style flip to improve valence/
  triangle shape after split+collapse. Reuse `delaunay.h`.
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

```
begin dab:
  thawTopo() once
  open LogChunkTopo
  while queue not empty:
    select independent set
    parallel: split / collapse / flip (collapseEdge, edge_split, delaunay)
    interpolate attrs on new verts
    update node ownership incrementally (unique_verts/faces, .spatial.*.node)
  close LogChunkTopo
  refreeze (or stay thawed for the stroke; measure)
  mark touched nodes Spatial_RegenTris / Spatial_RegenGPU
SpatialTree::update()  → tris, normals, VBO repack
```

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

### Wave 0 — CPU-only, correct first

Independent-set split/collapse/flip queue; batch-per-dab (thaw once);
re-triangulate affected faces; interpolate attrs on new verts; `LogChunkTopo`
undo; incremental node-ownership updates; `Spatial_RegenTris/GPU`. Drive it
end-to-end through the `debug_app` script harness; add a randomized
integrity-checked test per operator under `tests/` (the `mesh-topo-op` agent
scaffolds operator + test together).

**This wave alone may hit the perf target**, because the work is local. Do not
assume the GPU is needed — **profile first** with `StrokeProfiler`
(`--profile`). The prime suspects are spatial node-ownership updates and VBO
repack, *not* the edge surgery. If a node-granular regen dominates, add
incremental ownership maintenance before reaching for the GPU.

### Wave 1 — GPU the decision + attributes (stages A/B/D)

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

### Wave 2 — GPU-assisted mutation (only if profiling demands it)

Push collapse-set selection (independent set / coloring) or a lazy-update-table
collapse pass onto the GPU. This is where the literature's conflict-resolution
machinery lives. **Do not build this until Waves 0–1 are measured and found
wanting.** The half-edge restitch and freelist allocation stay on the CPU
regardless.

---

## 8. Bottom line

- Build the **straight C++ version first**; per-dab remeshing is local and
  small, and it may well suffice.
- "Offload to the GPU" really means **GPU for mark/compact/interpolate/normals
  (mostly already there), CPU for topology mutation** — a hybrid, using both.
- **Full-GPU topology mutation is the only genuinely hard piece**, blocked on
  (a) missing atomics/append infra and (b) parallel conflict resolution for
  collapse; build it last, behind profiling.
- **CBT/LEB is the reference for a pure-GPU adaptive layer but does not fit
  free-form tri remeshing** — note it, don't adopt it as the core.
- Mind the **Vulkan-vs-WebGPU asymmetry**: the browser path can't share buffers
  or scatter-read, so the CPU mutation path must remain authoritative and
  always available.
