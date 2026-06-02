# Dyntopo M7 — Taming the Densification Cascade

## Context

Dynamic topology is functionally complete (M1–M4 + M3 integration: local refine,
manifold, fully undoable, incremental spatial — see
[`dynamic-topology.md`](dynamic-topology.md)). What remains is the **perf** work
needed for the 5 M-triangle / ≥25 fps target. This subplan covers it.

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
