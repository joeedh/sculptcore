# Dyntopo M7 — Taming the Densification Cascade

## Status (updated)

The per-dab **O(mesh) terms are fixed** — the dab is now O(brush region)
end-to-end (~15–18 ms flat vs a full rebuild's 195 ms → 751 ms, speedup growing
with mesh size). Three were found by diagnose-first profiling and one was a
spatial bug:
- per-split `created_edges` O(total-edges) scan → O(valence) (`e1268fe`);
- round-0 candidate scan → caller-injected spatial seed (`9a0ebc2`);
- `tree->update()` regenerated *all* GPU buffers because the incremental
  callbacks never set `RegenGPU` → only the affected GPU nodes now (`c8ddece`);
- `interpAttrs` copied `.spatial.v.node` onto new verts → the tree never split
  → fixed by skipping TEMP attrs (`fc8c7e6`, see **M7.6**).

What remains: **M7.6** (cheaper tree placement — optional optimization), **M7.5**
(real 5 M-tri gate), and the **cascade quality** work below (now a triangle-
budget issue, not a speed one). The original "diagnose P2" framing is resolved:
the cost was these dumb O(mesh) scans, *not* valence or the cascade.

## Context

Dynamic topology is functionally complete (M1–M4 + M3 integration: local refine,
manifold, fully undoable, incremental spatial — see
[`dynamic-topology.md`](dynamic-topology.md)). The remaining **perf** work for
the 5 M-triangle / ≥25 fps target is below.

Profiling (the `bench_dyntopo` debug-app verb, with a `spatial=0/1` toggle)
established three things, two of them negative results that sharpen the path:

1. **The spatial path is not the bottleneck.** Incremental `tree->update()` is
   cheap and local (7–27 ms on 75 k–300 k-face meshes). M3 did its job.
2. **The dab is dominated by the dyntopo remesh itself**, and the per-dab cost
   grows with *total* mesh size even for a small brush:
   | mesh faces | full rebuild | incremental dab (ops) |
   |---|---|---|
   | 75 k | 55 ms | **703 ms** |
   | 170 k | 184 ms | **3 396 ms** |
   | 300 k | 416 ms | **7 283 ms** |
   `spatial=0` (no spatial callbacks) is within noise of `spatial=1`, so it is
   the **dyntopo core**, not the tree.
3. **Two distinct problems hide in that "ops" number** (below). Two fixes were
   tried and rejected by measurement:
   - **Local-frontier candidate gathering** (committed, `8840548`): correct and
     kept, but did *not* move the needle — proving the per-round edge *scan* was
     never the bottleneck.
   - **Valence-equalization flips** (measured, reverted): made it **worse**
     (splits 2 231→3 410). Valence flips optimize *connectivity regularity*, not
     edge *length*, so they flip a short `a-b` to a longer `c-d` diagonal that
     then needs more splitting.

## The two root problems

### P1 — Over-refinement (the cascade)

A radius-0.05 dab at `detail=0.004` produces **~4 700 splits** where the target
edge length needs only **~540**. The 1-triangle→2 split (`edge_split.h`) inserts
a midpoint and connects it to the apex; that **spoke** edge can be *longer* than
the edge that was split (worst on slivers / right-isoceles grid triangles). Long
spoke → split → new sliver → split … an ~8× over-refinement cascade. The fix is
to keep triangles well-shaped *during* refinement so spokes stay short.

### P2 — Per-split cost scaling

Per-split time grows with mesh resolution (≈0.31 ms @75 k → ≈1.2 ms @300 k) even
for similar split counts in a small region. Measured to be independent of the
spatial tree and the candidate scan. Leading hypothesis: **vertex-valence growth
in the dense region** — `Mesh::find_edge` is O(valence²) and disk/radial ops are
O(valence), and finer meshes drive deeper cascades → higher valences. Not yet
proven; a hidden O(mesh) per-op (e.g. an unexpected freeze/thaw) is not ruled
out. P1 and P2 are linked — controlling triangle quality bounds valence too.

## What already exists to build on

- **`source/mesh/utils/edge_flip.h`** — `flipEdge` 2-2 flip operator (committed
  `667b42a`), winding-preserving, cb-threaded, with `test_edge_flip`. The
  decision criterion is the caller's; the operator just does the topological
  flip and refuses boundary/non-manifold/duplicate-edge cases.
- **`source/mesh/utils/delaunay.h`** — `inCircumcircle` predicate (reuse for the
  Delaunay flip test) and `fitPlaneNormal`/`planeBasis` (project the local quad
  to 2D for robust in-circle / convexity tests).
- **`source/dyntopo/dyntopo.h`** — `applyBrushDab`, the round loop with the
  independent-set selection and the local frontier. The flip sweep slots in
  right after each round's apply (where the valence version was reverted from).
- **`bench_dyntopo` verb** (`source/debug/script.cc`) — the measurement loop:
  `bench_dyntopo detail=F radius=F center=x,y,z spatial=0/1`, reporting
  `full_rebuild` vs `ops`/`update` and split count. **Every milestone below is
  accepted or rejected by this verb**, the same way `sbrush-verify` gates the
  brush backends.

## Milestones

Each is independently measurable; do them in order — M7.1 de-risks the rest.

### M7.1 — Diagnose P2 (cheap, do first)

Throwaway `--profile`-gated instrumentation (per the CLAUDE.md "temporary
scaffolding" rule, ripped out after): count `thawTopo` calls per dab, and a
valence histogram + `find_edge` call/iteration count over a dab. Determine
whether the per-split scaling is **valence-driven** (then M7.2/M7.3 fix it for
free) or a **hidden O(mesh)** op (fix that directly — likely a freeze/thaw or an
accidental full-mesh walk). Acceptance: a one-paragraph finding + the offending
cost named. Remove all scaffolding.

### M7.1a — Graded target edge length / sizing field — DONE (commit ce69b76)

**Implemented and it works.** `DynTopoParams.grade` relaxes `l_max`/`l_min`
outward by `(1 + grade * dist/radius)` in the `consider` candidate test; the
frontier loop already expands outward so it was a few lines. `grade=0` = uniform
(default, tests unchanged). Exposed on the `dyntopo`/`bench_dyntopo` verbs + a UI
slider; `bench_dyntopo` gained `maxValence` + a graded `leftover` check.
Measured (subdivs 120, r=0.05, detail=0.005, still `leftover=0`):

| grade | splits | max valence |
|---|---|---|
| 0 (uniform) | 1512 | 27 |
| 2 | 117 (13×↓) | 15 |
| 4 | 21 (72×↓) | 10 (regular = 6) |

Cuts over-refinement 13–72× *and* removes the high-valence hubs — the cascade at
its source. Open follow-ups: tune the default grade for sculpting feel; the grade
shape is linear in `dist/radius` (try smoothstep/quadratic); and flips (M7.2) may
still help the residual rim slivers. Original write-up below.



The over-refinement and the high-valence hubs come largely from the **hard
boundary** between the uniformly-fine brush region and the coarse surrounding
mesh: an edge straddling that cliff keeps getting split trying to reach the fine
goal while its far end stays coarse, spawning slivers and piling valence onto the
boundary verts. Removing the cliff attacks the cascade at its *source* (it cuts
the number of splits), rather than repairing slivers after the fact (M7.2/M7.3) —
so evaluate it first.

**Idea (suggested):** make the target edge length a spatially-graded field that
is fine at the brush center and **relaxes (grows) outward**, blending into the
surrounding mesh's natural edge length. Equivalently: recursively expand the
affected vertex set outward and **relax the edge-length goal at each recursion
step**, so each successive ring is refined to a coarser target. The refinement
then self-terminates once the relaxed goal matches the ambient edge length, and
the result is a smooth size gradient instead of a fine/coarse step.

**Cheap to implement** — it falls out of the structure already in
`applyBrushDab`: the `frontier` set already expands outward one ring per round.
Replace the constant `p.l_max` in the candidate test with a per-edge
`targetAt(edgeMidpoint)` that grows with distance from `center` (or step `l_max`
up per frontier wave / round). Almost no new code — the round loop already *is*
the recursive outward expansion the idea wants; it just needs the goal to relax
as it goes.

**Why it helps**: far fewer splits than uniform-fine (the relaxed outer rings
need little); no high-valence boundary hubs (the gradient spreads the new verts);
and a smooth blend into the surrounding mesh, which also makes consecutive
overlapping dabs compose cleanly. **Knob**: the gradient slope is a
density/quality trade — too steep still slivers at the brush rim, too shallow
refines a wider area. Gate with `bench_dyntopo` (split count + a min-angle
readout). May pair with M7.2 flips for the residual rim slivers, or make them
unnecessary.

### M7.2 — Geometric flip sweep — DONE (it works; cascade broken)

Implemented the **length criterion** (the simple, monotone half of the plan):
each round, after the splits, sweep the in-region interior edges around the
touched (frontier) verts and flip `a-b`→`c-d` iff the new diagonal is strictly
shorter (`|c-d| < |a-b|`, 0.998 eps so a pass can't cycle) **and** the quad
`a-c-b-d` is convex in its average plane (`a,b` on opposite sides of `c-d` — else
the flip folds geometry; `flipEdge` is purely topological and doesn't check
this). Length-only is monotone: it can never lengthen an edge, so unlike the
rejected *valence* criterion it cannot manufacture split work. Collect-then-apply
(never flip while walking a disk); each helper re-validates so a flip
invalidating a later candidate is safe; flipped apexes re-enter the frontier.
Lives in `dyntopo.h` (`detail::flipQuad` / `detail::flipShortens` + the round
loop); `DynTopoParams.do_flips` (default **on**), `DynTopoStats.flips`, a
`bench_dyntopo flip=` knob + `flips=` readout, the `dyntopo` verb `flip=`, and a
UI checkbox.

**Measured (A/B, flip off→on).** It doesn't just trim splits — by keeping
triangles well-shaped it holds valence near-regular, which makes the cascade
*converge* and cuts per-split cost too:

| mesh / target | splits | rounds | leftover | maxValence | ops |
|---|---|---|---|---|---|
| 475 k, 0.3× sp — off | 5069 | 50 (CAP) | 197 | 60 | 41 ms |
| 475 k, 0.3× sp — **on** | 1771 | 16 | **0** | **9** | 21 ms |
| 475 k, 0.2× sp — off | 16875 | 50 (CAP) | 2199 | 97 | 168 ms |
| 475 k, 0.2× sp — **on** | 4536 | 21 | **0** | **10** | 58 ms |
| **5 M**, 0.5× sp — off | 6664 | 30 | 0 | 30 | 1600 ms |
| **5 M**, 0.5× sp — **on** | 4197 | 11 | **0** | **9** | **171 ms** |

The off rows below the cliff hit `max_rounds` and never converge (leftover
grows, valence 60–97); the on rows converge in a third the rounds with valence
~9–10. At 5 M the aggressive dab drops **9.3×** (1600→171 ms) and per-split cost
**~16×** (0.65→0.04 ms/split — low valence makes every disk/`find_edge` op
cheap). Gated by `test_dyntopo_cascade` (now an off/on/baseline comparison) so
the flip pass can't silently regress.

M7.3 (longest-edge bisection) is **not needed** for the cascade — flips resolved
it.

**Split-budget safety valve — DONE.** `DynTopoParams.max_splits` (0 = unlimited;
default) caps the splits a single dab applies; on reaching it the dab stops and
sets `DynTopoStats.budget_hit`, leaving the still-out-of-band edges for the next
dab (a moving brush re-touches the region). So a one-shot heavy refine degrades
to bounded latency instead of a long frame, and the region still fully converges
across dabs. Verified by `test_dyntopo_budget` (a 201-split refine at budget 50
spreads over 6 dabs, ≤50 each, converging to the same mesh) and the bench
(475 k aggressive dab: unbudgeted 55 ms ops → `max_splits=1000` **15 ms**, BUDGET
flagged, valence still ~10). Calibrate the budget to the frame target:
≈frame_ms / ms-per-split (~0.04 ms/split at 5 M with flips). Exposed as a
`bench_dyntopo max_splits=` knob (+ `BUDGET` readout), the `dyntopo` verb
`max_splits=`, and a UI slider. With flips + this valve, **≥25 fps @ 5 M is
reachable for both steady-state sculpting and a budgeted heavy refine.**

### M7.3 — Longest-edge bisection (if flips are insufficient)

Rivara longest-edge bisection: split the **longest** edge of a triangle first,
propagating the split to the neighbour sharing that edge to stay crack-free.
This bounds triangle quality by construction and eliminates the sliver cascade,
at the cost of a larger rewrite of `applyBrushDab`'s candidate selection (the
propagation interacts with the independent-set/frontier structure). Heavier than
M7.2 but the provably-bounded option. Reference: the CBT/LEB literature in
[`../dynamic-topology.md`](../dynamic-topology.md) §6 (note: pure-GPU CBT is *not*
the goal — only the LEB *refinement rule* transfers).

### M7.4 — Tangential smoothing — DONE (completes Botsch-Kobbelt)

The 4th operator. After the flip sweep each round, region verts slide toward the
**area-weighted centroid of their 1-ring, with the normal component removed**
(tangential only — equalizes triangle sizes / kills slivers without shrinking the
surface or smoothing away sculpted detail). Vertex normal + centroid are computed
live from the 1-ring (`detail::smoothTangent` — stored normals go stale across
splits); the update is simultaneous (Jacobi, order-independent → deterministic);
each move is clamped to half the shortest incident edge so a thin triangle can't
fold; **boundary / non-manifold verts are left fixed**. Position-only, so no
callback — the region's leaves are already bounds-dirty from the splits.

`DynTopoParams.do_smooth` (default **off** — it's a quality nicety, not a
perf/correctness fix, and it nudges geometry so it wants interactive validation
against the brush deform) + `smooth_lambda` (0.5). `DynTopoStats.smooths`;
`bench_dyntopo smooth=`/`smooth_lambda=` knobs + an edge-length **CV** readout
(stddev/mean — the uniformity metric); `dyntopo` verb knobs; UI checkbox +
strength slider.

**Measured (flips on, smooth off→on).** It improves uniformity and, by
equalizing edge lengths, actually does *less* split work:

| | edge-len CV | min tri area | splits | maxValence | leftover |
|---|---|---|---|---|---|
| off | 0.302 | 1.95e-5 | 1760 | 10 | 0 |
| **on** | **0.262** | **3.30e-5** | 1351 | 8 | 0 |

Lower CV = more even triangles; the **larger min area** confirms slivers shrank
(not folded — the clamp holds). Still converges (leftover 0), in fewer rounds.
Gated by `test_dyntopo_smooth` (CV-on < CV-off, both converge, min area > 0 so no
fold). The Botsch-Kobbelt quartet (split / collapse / flip / smooth) is complete.

### M7.5 — 5 M-tri acceptance gate — MEASURED (cliff since resolved by M7.2)

> **Update (post-M7.2):** the "cascade cliff" below was the geometric spoke
> cascade, and the M7.2 flip sweep removed it. The same 5 M aggressive dab that
> hit the cliff now converges in 11 rounds at valence 9, **171 ms** (was 1600 ms
> flip-off / never-converging deeper). Steady-state sculpting (tens–hundreds of
> splits/dab) is comfortably real-time at 5 M; a one-shot heavy refine (~4 k
> splits) is 171 ms — bounded and convergent, and the per-dab split budget
> (`max_splits`, now implemented — see M7.2) pins it under one frame by spreading
> the refine across dabs. The original mixed verdict and the diagnosis that led
> to M7.2 are kept below for the record.



Gate built and run. A real **5.05 M-tri** mesh (`make_cube subdivs=650` +
`triangulate`; the flat face is fine — geometry shape doesn't affect remesh cost)
benched with `bench_dyntopo` (new `rebuild=0` flag fires several independent dabs
on one expensive build; new `rounds` readout). Two clear results:

**1. Spatial maintenance scales — PASS.** Incremental `tree->update()` is
**2–11 ms at 5 M**, fully local (M7.6 confirmed). The tree is never the
bottleneck.

**2. The dyntopo remesh has a hard cascade cliff — PARTIAL.** Per-dab cost tracks
**cascade depth (round count)**, NOT split count, and is super-linear and erratic.
Mapping `ops` vs the target/spacing ratio (base spacing ≈ 0.0015 at 5 M; 0.005 at
475 k) shows a cliff at **target ≈ 0.5× base spacing**:

| target / spacing | rounds | leftover | maxValence | ops |
|---|---|---|---|---|
| ≈0.6× | 17 | 0 (converged) | 17 | **7 ms** |
| ≈0.3× | 50 (CAP) | 299 | 69 | 37 ms |
| ≈0.2× | 50 (CAP) | 2416 | 115 | 182 ms |
| ≈0.16× | 50 (CAP) | 5943 | 140 | 524 ms |

(475 k figures; the same cliff reproduces at 5 M — a radius-0.05 / 0.0008 dab is
6.5 k splits / **7 s**.) Below the cliff the spoke cascade (P1) runs away: the dab
hits `max_rounds`, never converges (leftover *grows*), and valence explodes to
140 — **even with grade=2**. Grading tames *moderate* refinement but cannot rescue
an aggressive *refine-from-coarse* dab.

**Verdict.** ≥25 fps @ 5 M holds for **steady-state sculpting** — a dab that
maintains an existing target edge length does moderate per-dab refinement
(target ≳ 0.6× current spacing) and stays real-time (7–45 ms). It does **not**
hold for a one-shot *aggressive refine* (target ≪ spacing), which cascades. That
one-shot case is the remaining blocker and is squarely **M7.2 (geometric/Delaunay
flips)** / **M7.3 (longest-edge bisection)** — the spoke-shape fixes — plus a
safety valve: a **per-dab split/round budget** that defers overflow refinement to
later dabs so a pathological dab degrades to bounded latency instead of a 7 s
freeze (the `max_rounds` cap bounds rounds but still allows 500 ms+ and leaves the
region under-refined).

**CI regression gate — DONE.** `tests/test_dyntopo_cascade.cc` (ctest, no GPU): a
seeded dab on a fixed grid asserts the graded config converges (leftover 0), its
split count + max valence stay under deterministic ceilings (308 / 18 today,
~1.5× headroom), and that grading cuts splits >4× and removes the high-valence
hubs vs uniform (which here cascades, leftover>0, valence ~57). Catches a silent
cascade/grading regression. (A perf-ms gate would be flaky; the split count is
deterministic given the seed.)

### M7.6 — Spatial-tree currency (keep the tree correct + cheap to update) — DONE (split side)

**Both halves of the locality shortcut implemented and measured.**

1. **O(1) anchor placement** — `add_face` now calls `find_anchor_leaf` (a leaf
   already owning one of the new face's verts) and files the face directly into
   it via `add_face_at`, skipping the root→leaf centroid descent. The build path
   (no owned neighbour yet) still descends. The anchor vert is by construction
   within ~half an edge of the new geometry, so placement lands in the right leaf
   or an immediate neighbour; `regen_node_bounds` tightens the loosened AABB from
   the new tris during `update()`.
2. **Deferred batched rebalance** — `add_face_at` does **not** split inline; it
   records over-full leaves in `rebalanceCandidates_`. `applyDeferredRebalance()`
   (top of `update()`, before the tris phase) splits each once. `split_node`
   already recurses, so one call turns a leaf that gained ~1500 verts into a
   balanced subtree — replacing N threshold-crossing re-inserts.

Measured (`bench_dyntopo`, leaf_limit 256, r 0.05, **grade 2** = converged so the
comparison is clean):

| mesh | baseline ops/total | M7.6 ops/total |
|---|---|---|
| 170 k | 8.9 / 10.4 ms | **5.3 / 6.2 ms** |
| 475 k | 8.8 / 9.5 ms | **7.8 / 8.1 ms** |

`update` time *fell* too (one batched split beats repeated inline splits +
intermediate GPU regens). Both scale with the brush region, not total mesh.
`test_spatial_dyntopo` updated: placement is eager/correct immediately after the
dab (ownership complete), the split is driven by `applyDeferredRebalance()`.

**Merge side — DONE (M7.6b).** The rebalance pass now also folds **under-full
sibling leaves** back up after collapse-heavy strokes. `remove_vert` records a
shrinking leaf's parent in `mergeCandidates_` (the inverse of
`rebalanceCandidates_`); `applyDeferredMerge()` runs every `mergeCadence_`-th
`update()` (default 8 — **not** per dab, per the user) and, for each parent whose
two leaf children together own fewer than `leaf_limit/2` verts (hysteresis vs the
split threshold, so no split/merge thrash), turns the parent back into a leaf,
re-absorbs the subtree's faces via `add_face_intern`, and frees the children.
Merges **cascade up** the chain in one pass (the worklist re-pushes the
grandparent). Node removal is O(1) swap-remove (`free_node`) preserving castRay's
`node->index == nodes[index]` invariant; an orphan-recovery step re-assigns any
subtree vert referenced only by faces outside the subtree to the merged leaf, so
ownership coverage stays complete. Verified by `test_spatial_merge` (625→37 verts,
13→3 leaves, ownership complete + idempotent) and a 10-stroke debug-app session
(manifold intact, no crash). `re-split` of a stale split-plane after merges is
still future and low-priority (the normal `add_face_at` path re-splits on growth).
Original write-up below.

Dyntopo must keep the spatial tree current as it adds/removes geometry — node
ownership (`.spatial.{v,f}.node`, each leaf's `unique_verts`/`unique_faces`),
leaf AABBs, and the GPU VBOs. This is wired through `SpatialTree::
getSpatialCallbacks()` (`onFaceCreate` → `add_face`, `onFaceKill`/`onVertKill` →
`remove_face`/`remove_vert`), with `tree->update()` regenerating the dirty
leaves' tris/bounds/GPU afterward.

**Bug found and fixed (commit fc8c7e6).** `interpAttrs` (used by `splitEdge` to
seed the new midpoint vertex's attributes) skipped TOPO links but *not* TEMP
attrs, so it **copied `.spatial.v.node` from a parent vertex**. Every new vert
inherited the parent's leaf id, so `add_face` filed it under `other_verts`
instead of `unique_verts`; `node_needs_split` counts `unique_verts`, undercounted
forever, and **the tree never split** — the whole refined region collapsed into
one giant leaf (no spatial locality; the "tree isn't updating" symptom). Fix:
`interpAttrs` skips TEMP as well as TOPO. The latent gap: `test_spatial_dyntopo`
only checked *face* ownership; it now also checks vert ownership + that the tree
rebalanced. **Lesson for the cheap path below: the new vert's node id is
authoritative tree state — it must be *set deliberately by the placement logic*,
never inherited from interpolation.**

**Current placement cost.** `add_face` descends root→leaf by centroid
(`add_face_intern`, O(log n) per face) and calls `split_node` inline when a leaf
crosses `leaf_limit` (re-inserting the leaf's faces, O(leaf_limit) each — and a
leaf gaining ~1500 verts in one dab crosses the threshold repeatedly, so it
splits several times, re-inserting each time). This is correct and currently
~part of the ~15 ms/dab "ops", but it is the descent + repeated-split cost the
next optimization targets.

**Cheap approach to explore (the locality shortcut).** Dyntopo's new geometry is
spatially adjacent to existing geometry whose node is already known, so the
root descent is avoidable:

1. **O(1) anchor placement.** A new vert `vm = midpoint(A,B)`; `A`,`B` already
   carry `.spatial.v.node`. Add `vm` *directly* to `A`'s leaf (a
   `tree->add_face_at(f, anchorLeaf)` that updates `unique_*` + sets the node id
   + marks the leaf dirty), skipping `add_face_intern`'s descent. The anchor for
   a new face is the leaf of any of its already-owned verts. Placement is
   "rough" (the parent's leaf, not the geometrically tightest one — leaf AABBs
   loosen slightly), but `vm` is by construction within ~half an edge of `A`, so
   it's the right leaf or an immediate neighbour. This is the *intentional*
   version of what the bug did by accident (set `vm.node = A.node`) — except it
   also files `vm` in that leaf's `unique_verts`, keeping the accounting correct.
   The build path (`buildAll`, no existing neighbours) keeps the root descent.
2. **Deferred / batched rebalance.** Do **not** `split_node` inline during the
   dab. Let leaves grow past `leaf_limit`, then run one rebalance pass over the
   touched leaves at dab (or stroke) end — split each over-full leaf once into a
   balanced subtree. This replaces N threshold-crossing re-inserts with one
   `O(leaf_size log leaf_size)` split, and batches the cost. Over-full leaves
   during the dab just make brush queries on them iterate a few more verts —
   bounded, and gone after the pass.

**Cost / tradeoffs.** O(1) placement removes the O(log n) descent per face;
deferred rebalance turns repeated inline splits into one batched split per
touched leaf — both scale with the brush region, not total mesh or dab density.
Risks: looser leaf AABBs until the rebalance pass (mitigated by it running every
dab); the rebalance pass must stay seed-deterministic (parity goal); and
`remove_*` already needs no descent, so collapses are unaffected. Gate behind a
measurement: only adopt if `bench_dyntopo`'s ops time drops without regressing
`test_spatial_dyntopo`'s ownership + rebalance assertions.

## Measurement protocol

For every change, report from `bench_dyntopo` (subdivs ∈ {80, 120, 160},
`radius=0.05`, `detail` ≈ 0.5× local edge, `spatial=0`):
- **split count** vs the theoretical minimum (region area ÷ target-triangle
  area) — the cascade ratio; today ≈8×, target ≤2×.
- **ops ms** and its growth slope vs mesh size — target near-flat for a fixed
  brush region.

## Risks / open questions

- **Flips may still net-negative** (M7.2): each flip costs a kill+make of two
  faces. If the cascade reduction doesn't outweigh the flip cost, M7.3 (LEB) is
  the fallback — it avoids creating slivers rather than repairing them.
- **Determinism**: any flip/LEB ordering must stay seed-deterministic (the
  `test_dyntopo` determinism + the CPU/GPU parity goal depend on it). Frontier
  `Set` iteration order is the thing to pin down.
- **Undo cost**: flips/LEB add more topology ops per dab → larger `LogChunkTopo`
  chunks. Undo is a cold path, but watch the per-stroke memory.
- **P1/P2 coupling**: if M7.1 finds P2 is valence-driven, M7.2/M7.3 fix both at
  once; if P2 is a hidden O(mesh) op, fix it independently first or the dab
  stays O(mesh) regardless of cascade quality.
