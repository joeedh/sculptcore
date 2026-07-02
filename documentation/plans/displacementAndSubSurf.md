# Displacement & Subsurf — Implementation Plan

Implements [`../final-displacement-architecture.md`](../final-displacement-architecture.md)
(which see for all design rationale; this file is sequencing, tasks, and
gates). Companion design docs:
[`../sculpt-layers-design.md`](../sculpt-layers-design.md),
[`../dyntopo-vdm-region-hybrid.md`](../dyntopo-vdm-region-hybrid.md),
[`../tangent-displacement-issues.md`](../tangent-displacement-issues.md).

## Status (2026-07-02)

Planned. Nothing started.

---

## How to run this: plan structure and worktrees

**Recommendation: three sub-plans in this one document, not one monolithic
sequence and not separate plan files.**

- The work has two genuinely independent tracks — the **VDM/displacement
  track** (VdmStore, splatter, render paths, region partition) and the
  **subsurf/multires track** (refiner, grids store, level materialization) —
  that touch almost disjoint modules. Forcing them into one milestone ladder
  serializes work that has no data dependency.
- But they share seams (the layer compositor, `AttrUse::SCULPT_LAYER`, the
  frame provider, per-face bounds), so those must land **first, once, on a
  single track** — otherwise the two tracks each invent half of the shared
  layer and merge painfully. That shared slice is Workstream F below.
- One *document* (this one) rather than three files, because the convergence
  milestones (X) reference both tracks and the gates need one place to live.
  This mirrors how `dynamic-topology.md` ran M1–M7: milestone-gated, one doc.

**Worktrees: two, created only after Workstream F merges to master.**

- Worktree `displacement` → Workstream V. Touches `source/vdm/` (new),
  `source/brush/`, `source/spatial/` (bounds/dirty hooks), app render path.
- Worktree `subsurf` → Workstream S. Touches `source/subdiv/` (new),
  app-side level UI. Near-zero overlap with V after F is in.
- Workstream F itself is **not** worktree-parallel — it is the shared
  foundation and should be one branch, reviewed and merged before the split.
- Workstream X (convergence) runs back on a single track after V and S merge.
- Per the submodule conventions, each worktree carries matching-name branches
  in the parent repo and sculptcore, committed together.

Suggested staffing shape: F solo → V and S in parallel (primary + agent
worktree) → X solo.

---

## Decisions locked from the design phase

- Texels never enter the spatial tree; the tree gets per-face `max|D|`
  bounds padding, tile-dirty hooks, and carrier routing only.
- VDM renders via the fragment shader by default; compute tessellation is an
  opt-in tier whose amplified verts never enter the mesh/tree.
- Multires = canonical grids store (implicit topology, disk-backable) + a
  materialized `mesh::Mesh` per active edit level, LRU-cached. The
  finest-mesh + skip-iterator model is rejected (architecture report §4.2).
- Beyond-edit-level density = GPU stencil-table SpMV, bit-consistent with the
  CPU discrete-CC refiner. No analytic patches, no Phong tessellation.
- Polygon vertex layers default to DELTA/world storage; TANGENT is the
  subsurf-multires representation (and later polygon opt-in).
- VDM carrier: UV atlas first, Ptex backend when multires lands, both behind
  the `VdmStore` parameterization seam.

## Module map (new code)

```
source/displace/   layer compositor, SculptLayerSettings sidecar, clamp/predicate state
source/vdm/        VdmStore: tiles, pyramids, atlas + ptex backends, tile-delta undo
source/subdiv/     CC refiner + stencil tables, grids store, level materialization
source/frames/     frame provider (smoothed normal + cross-field azimuth)  [or fold into displace/]
```

Existing modules touched: `mesh/` (`AttrUse::SCULPT_LAYER`, `.detail.carrier`,
boundary `BC_LAYER_REGION` bit), `spatial/` (bounds padding, dirty hooks),
`brush/` (dab dispatch, splatter), `meshlog/` (VDM tile-delta bracketing),
`gpu/` + app renderengine (fragment path, amplification pass).

---

## Workstream F — shared foundation (single track, merge before splitting)

### F1 — Sculpt-layer attribute category + compositor skeleton

- `AttrUse::SCULPT_LAYER` + `SculptLayerSettings` sidecar (mode, space,
  parent, weight, enabled, frozen, clampFrac), serialized.
- Layer compositor in `source/displace/`: evaluate base + ordered DELTA
  vertex layers into the positions the tree/draw consume; incremental
  (dab-region-scoped) re-evaluation.
- Brush integration: sbrush `save`-driven meshlog undo already covers
  attribute layers — verify with a stroke test writing to a layer instead of
  positions.
- Gate: ctest unit tests (composition order, enable/weight, interp under a
  scripted dyntopo split); debug_app stroke + undo/redo round-trip on a
  2-layer stack; wasm↔native parity.

### F2 — Per-face displacement bounds + carrier tag plumbing

- FACE-domain `float` bound attribute + FACE-domain `.detail.carrier` int
  (TEMP, like `.spatial.*`); `regen_node_bounds` pads leaf AABBs by the max
  bound of owned faces.
- Dirty hook API: `markFacesDisplacementDirty(span<int>)` → leaf
  `Spatial_RegenBounds`.
- Gate: `test_spatial_gpu_partition`-style unit test asserting padded AABBs
  contain `base + bound` extremes; castRay hit parity on a synthetically
  bounded mesh.

### F3 — Frame provider

- Smoothed vertex normal + cross-field azimuth (KCPS 4-RoSy eigensolve or
  heat-diffused seed — start with the cheap variant) computed on demand for a
  static face set, stored as per-vertex attributes.
- Deterministic across backends (this is the synchronization anchor — add a
  bit-stability test, wasm vs native).
- Gate: frame continuity test on sphere/cube fixtures (singularity count =
  Poincaré–Hopf expectation); parity test.

**Merge F to master. Create the two worktrees.**

---

## Workstream V — VDM track (worktree `displacement`)

### V1 — VdmStore core (atlas backend)

- Tile store keyed by corner-UV atlas: tile table, allocation, `sample(face,
  u, v)` (bilinear), mip + magnitude-bound pyramids, coarse-mip → per-face
  bound export (feeds F2).
- Tile-delta undo channel with begin/end bracketing hooks (consumed in V2).
- Serialization: tile container reusing the autosave lz4 path.
- Gate: ctest round-trips (write/sample/serialize/undo); bound-pyramid
  correctness vs brute force.

### V2 — VDM brush splatter

- Dab path: tree query for faces under brush (existing) → filter
  `.detail.carrier == VDM` → rasterize dab footprint into UV tiles, falloff
  evaluated in world space from `base + VDM(texel)` (the `pbvhTexPaint.md`
  pattern, vec3 payload). Seam handling: rely on region/seam feature edges +
  one-texel skirt copies rather than texpaint's extrusion-quad guard.
- Tangent inversion through the F3 frame; total-magnitude clamp to
  `α·ρ_min` with hysteresis (clamp state lives in the compositor).
- MeshLog: tile deltas bracketed inside the dab's step; undo/redo restores
  texels + bounds + dirty state atomically.
- Gate: debug_app verb `vdm_stroke` + save_pos-style texel snapshot asserts;
  undo fidelity; wasm↔native parity on a scripted stroke.

### V3 — Fragment render path

- Bind tile array + face table into the material draw shader; sample VDM,
  derive the shading normal in-shader from displacement derivatives; frame
  attributes flow through the existing requested-attrs contract (UV + frame =
  two more slots). Remember the litemesh gotcha: any new spatial-side shader
  needs its TS WGSL port or it silently no-renders.
- Dirty-tile GPU upload path (no `regen_gpu_node` on VDM-only dabs).
- Gate: headless screenshot A/B (flat base + known VDM vs analytically
  displaced reference mesh, SSIM threshold); litemesh attr-render
  integration test extended with the frame slot.

### V4 — Region partition + promotion

- Per-dab eligibility predicate (fold bound + overhang angle) over the brush
  region; `promoteRegion(faces)`: subdivide, seed verts from `base + VDM`,
  flip carrier, emit `MeshCallbacks`, promote region-boundary + UV-seam edges
  to `BC_LAYER_REGION` feature edges.
- Hysteresis thresholds as tunables; demotion explicitly deferred to X.
- Gate: scripted stroke that forces a fold → promotion fires, no cracks
  (boundary-vert pinning test), undo reverts promotion + texels together;
  dyntopo feature preservation test across a VDM seam.

### V5 — App wiring & polish

- Layer-stack UI (data-API paths, `pnpm gen:paths`), carrier overlay debug
  draw, feature-flag gating, docs update, strip `CLAUDENOTE:`s.

## Workstream S — subsurf track (worktree `subsurf`)

### S1 — CC refiner + stencil tables

- Uniform Catmull-Clark refiner over `mesh::Mesh`: one splitting step (n-gon
  → quads) then regular refinement; sharp/boundary crease rules from
  `EDGE_SHARP`. Output per level: vert count, grid coords, and the cached
  stencil table (each fine vert = fixed sparse combination of cage verts).
- Gate: ctest against hand-checked fixtures (cube, n-gon fan, creased cube);
  stencil-evaluated positions ≡ direct recursive subdivision, bit-exact.

### S2 — Grids store

- Per-quadrant-after-one-split grids (Ptex `__faceindex` convention):
  per-level frame-relative `float3` displacement arrays + per-level custom
  attributes, implicit topology, O(1) stride neighbor indexing.
- Flat-array layout designed for paging (chunked, offset-table headed);
  serialization; no disk-backing *implementation* yet, just the layout that
  permits it.
- Gate: store round-trip tests; neighbor-indexing tests incl. grid-boundary
  crossings into adjacent grids.

### S3 — Level materialization + LRU

- Build the active level's `mesh::Mesh` from stencils + composited
  displacement; build its spatial tree; LRU cache of (mesh, tree) per level
  with eviction. Measure bulk tree build at target densities — if finest-level
  build time is unacceptable, a bulk-build fast path becomes an S3 subtask
  (risk flagged in the architecture report §4.2).
- Level-switch op + app UI stub.
- Gate: switch L↔L+1 round-trip is lossless (positions bit-stable through
  materialize→writeback with no edits); switch latency budget measured and
  recorded.

### S4 — Multires sculpt loop

- Sculpt on the level mesh with the existing executor (TANGENT storage);
  stroke-end writeback re-expresses level positions into the store's
  frame-relative deltas (the meshlog `endStep` created-vert refresh pattern).
- Automatic upward ride verified (edit level L, finer detail follows);
  explicit down-refit op (least-squares reproject, deferred/non-live).
- Undo: meshlog on the level mesh + store writeback replay on redo.
- Gate: debug_app multires stroke scripts — ride-along invariance (sculpt at
  L, assert finest-surface detail preserved), undo/redo fidelity across a
  level switch, wasm↔native parity.

### S5 — GPU stencil amplification (fine display)

- Compute pass: stencil SpMV coarse→render level into transient buffers +
  smoothed-frame pass; indirect draw. Cache across frames when the coarse
  level is clean. This pass is deliberately VDM-agnostic here; V's tessellated
  tier reuses it in X.
- Gate: screenshot A/B — GPU-amplified level N vs CPU-materialized level N,
  pixel-identical modulo fp; perf budget per frame recorded.

## Workstream X — convergence (single track, after V + S merge)

- **X1 — VDM on multires**: VDM layer on the finest level; clamp policy per
  base kind (subsurf = hard clamp / add-a-level prompt, no promotion).
- **X2 — Ptex backend** for VdmStore (per-face grids + adjacency + skirts),
  sharing S2's grid conventions; fragment path binds per-face table.
- **X3 — Tessellated render tier**: V3's shader + S5's amplification =
  true-displacement opt-in; per-region selection from compositor state.
- **X4 — Cross-carrier bakes**: VDM→vertex-layer extraction, geometry→VDM
  demotion (explicit op), external VDM export (frame-synchronized).
- **X5 — Disk-backed grids store** (activate S2's layout: paging/eviction).
- Final step: strip remaining `CLAUDENOTE:` comments across all three
  workstreams; update `projectIndex.md`, root docs, and this plan's status.

---

## Testing strategy

- Every milestone lands with ctest units + a debug_app script where geometry
  behavior is involved; wasm↔native parity asserts on anything
  brush/undo-visible (reuse the `sculptcore_parity` boot path app-side).
- Render milestones (V3, S5, X3) gate on headless screenshot A/B via the NW.js
  harness (`--gen-scene` + `--screenshot`), not eyeballs.
- Frame-provider determinism (F3) is a standing parity test — it silently
  breaks bakes if it drifts.
- Undo fidelity uses the `save_pos`/`assert_pos` pattern, extended with a
  texel-snapshot analogue for VDM (V2).

## Risks / open questions

1. **Bulk spatial-tree build time** at finest multires levels (S3) — may need
   a dedicated fast path; measure early, before S4 depends on it.
2. **Cross-field combing** (F3): signed frames need branch cuts; v1 places
   singularities naïvely and accepts transitions — revisit if VDM seam
   artifacts show.
3. **Clamp/hysteresis tuning** (V4): `α`, `θ_max`, demote margin need real
   strokes; ship as debug-tunable settings first.
4. **Grids granularity** decision (per-quadrant, architecture report §9.3) is
   assumed by S2 — confirm before S2 starts.
5. **WebGPU storage-buffer limits** for stencil tables + tile face tables at
   production sizes (S5/V3) — validate budgets in the M0-style spike of each.
