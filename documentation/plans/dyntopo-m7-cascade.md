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

### M7.1a — Graded target edge length / sizing field (suggested — try before flips)

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

### M7.2 — Geometric (Delaunay) flip sweep

Replace the rejected valence criterion with a **length/Delaunay** one: flip the
shared edge `a-b` (apexes `c,d`) iff the local quad is convex (so the flip can't
invert) **and** the flip improves it — either `|c-d| < |a-b|` (simple, directly
shortens spokes) or the Delaunay empty-circumcircle test via
`delaunay.h::inCircumcircle` on the 2D-projected quad (the principled
min-angle-maximizing choice). Interleave one greedy sweep per round over the
frontier region (the structure the valence version already had), threading `cb`
so spatial/meshlog stay consistent. **Accept only if `bench_dyntopo` shows the
split count and `ops` time drop** vs the committed baseline; otherwise revert and
go to M7.3.

### M7.3 — Longest-edge bisection (if flips are insufficient)

Rivara longest-edge bisection: split the **longest** edge of a triangle first,
propagating the split to the neighbour sharing that edge to stay crack-free.
This bounds triangle quality by construction and eliminates the sliver cascade,
at the cost of a larger rewrite of `applyBrushDab`'s candidate selection (the
propagation interacts with the independent-set/frontier structure). Heavier than
M7.2 but the provably-bounded option. Reference: the CBT/LEB literature in
[`../dynamic-topology.md`](../dynamic-topology.md) §6 (note: pure-GPU CBT is *not*
the goal — only the LEB *refinement rule* transfers).

### M7.4 — Tangential smoothing (optional, completes Botsch-Kobbelt)

After split/collapse/flip, relocate region verts toward the area-weighted
centroid of their 1-ring, projected back onto the tangent plane (keep them on
the surface). Equalizes triangle sizes and further suppresses slivers. Gate it
behind a param; verify it doesn't fight the brush deform.

### M7.5 — 5 M-tri acceptance gate

Build a genuine ~5 M-triangle test scene (a high-`subdivs` triangulated cube is
too slow to build; use a procedural sphere/terrain or load one), drive an
interactive stroke with `--profile` (`StrokeProfiler`), and confirm the per-dab
cost stays local and the stroke holds **≥25 fps** on the reference laptop. This
is the milestone the whole feature targets. Also wire a `bench_dyntopo`-style
A/B into CI so the cascade can't silently regress.

### M7.6 — Spatial-tree currency (keep the tree correct + cheap to update)

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
