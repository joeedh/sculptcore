# Displacement & Subsurf — Implementation Plan

Implements [`../final-displacement-architecture.md`](../final-displacement-architecture.md)
(which see for all design rationale; this file is sequencing, tasks, and
gates). Companion design docs:
[`../sculpt-layers-design.md`](../sculpt-layers-design.md),
[`../dyntopo-vdm-region-hybrid.md`](../dyntopo-vdm-region-hybrid.md),
[`../tangent-displacement-issues.md`](../tangent-displacement-issues.md).

## Status (2026-07-03)

**Workstream F merged to master** (branch `displacement-subsurf-f`, torn
down). **Both engine tracks are complete and unified on this branch**: the
S track (S1–S5, engine work done on `subsurf`) was pulled in by rebasing
`displacement` onto the pushed `subsurf` branch, so the V commits sit on top
of S1–S5. **WORKSTREAM V IS COMPLETE (V1–V5)** and the **S app-wiring pass
is DONE** (below); next is workstream X (S's production draw integration
with V's tier rides X3):

- **X1 done (VDM on multires).** Engine: `Multires::materialize` synthesizes
  per-grid **chart UVs** on level meshes (`assignGridUVs`: ⌈√G⌉-per-row atlas
  cells, inset gutter; a pure function of topology, so charts are identical
  across levels AND backends — finest-level texels sample correctly from any
  level's parameterization); level meshes carry a runtime `Mesh::topoLocked`
  marker; `collectPromotionCandidates`/`promoteRegion` early-out on locked
  bases (subsurf clamp = **true ceiling**, no promotion band, matching
  sculpt-layers-design §8); per-splat `texelsClamped` crosses the seam as
  `Vdm_lastSplatClamped()` (the add-a-level prompt signal; napi + 4-place TS
  threaded). App: `_attachMultiresLevel` re-frames + re-tags the carrier when
  a VdmStore is attached, so the fragment tier renders across level switches;
  `destroy()` frees a live stack before the mesh/tree frees (S-pass double-
  free fix). Gates green: `test_multires` gains gridUV invariants (in-cell,
  level-consistent) + the locked-splat/no-promotion-under-force gate;
  `sculptcore_multires` integration test 26/26 both backends
  (`__multiresVdmTest`: splat through synthesized charts, no vertex motion,
  prompt signal exact cross-backend, store bit-stable across level switches).
  Found + filed: F3 cross-backend frame parity breaks at the ulp level on
  curved bases (see the X1 design-note follow-up); the atlas parity gate is
  quantized (1e-3) until frames are bit-stable. Deferred to X3/X4 app pass:
  interactive store lifecycle, per-dab carrier routing, the actual prompt UI.

- **S app-wiring pass done.** Engine additions: `Multires::downRefit(level)`
  (explicit down-refit — Jacobi-CG least squares on the stencil normal
  equations fits level−1 to the level surface, warm-started from the chain;
  the level's disp is re-expressed against the new base so its surface is
  preserved; coarser levels bit-untouched), app-tunable slot-tree params
  (`treeLeafLimit/DepthLimit/GpuTriTarget`), the `subdiv` C-API + bindings
  (`Multires_new/free/setActiveLevel/activeMesh/activeTree/writeback/
  downRefit` + the store-blob undo pair `Multires_serializeStore/
  restoreStore`; `maxLevel`/`activeLevel` bound methods), the `multires_refit`
  debug verb, and the gate extension in `test_multires` (residual halves,
  fine surface preserved to 1e-5, level 1 bitwise untouched). App half: napi
  wraps + 4-place TS threading; the LiteMesh attach model (mesh/spatial as
  non-owning slot views, cage parked, `_replaceMesh` flattens a live stack;
  dyntopo + auto-defrag force-gated off on level meshes; stroke-end and
  meshlog-undo/redo writeback hooks); undoable ToolOps
  `litemesh.multires_{enable,set_level,down_refit,delete}` (level switch
  undoes by switching back — level changes ride the toolstack, so no
  per-step level bookkeeping; refit/delete undo by store-blob restore);
  a Multires properties panel + level slider (drag-merged) behind the new
  `sculptcore.multires` feature flag (default off);
  `documentation/multires.md`. Gate green: `sculptcore_multires` integration
  test — 15/15 both backends; enable → lossless level round-trip → real DRAW
  stroke + writeback → undo/redo resync → down-refit (fine preserved, coarse
  moved) → delete restores the cage, with **bit-identical cross-backend
  checksums including the CG down-refit result**. Known debts: no `.wproj`
  persistence of the stack (flatten-on-save; X-track serialization), S5 GPU
  amplification not yet in the production draw (X3).

- **V5 done.** Engine half: displace C-API layer mutators
  (`Mesh_layerSetWeight/SetEnabled/SetFrozen/Remove`, compositor-maintained
  co), bound per-layer reads on `Mesh`, and the carrier overlay — the
  feature overlay draws `EDGE_LAYER_REGION` in pink. App half: napi wraps +
  4-place TS threading; the LAYER_DRAW sculpt tool (SculptTools 22 →
  LAYERDRAW) with active-sculpt-layer redirection through the paint-tool
  category path (`AttrUseFlags.SCULPT_LAYER`); a layer-stack panel on the
  LiteMesh properties tab (list + weight slider + enabled/frozen + add/
  remove, all undoable ToolOps; weight drags merge to one undo entry;
  remove restores by serialize-blob); feature flag
  `sculptcore.sculpt_layers` (default off) gating panel/tool/ops;
  `documentation/sculptLayers.md`. Gate green: `sculptcore_layers`
  integration test extended with a stroke through the REAL tool mapping +
  mutator round-trips + undo — 22/22 both backends, cross-backend
  checksums identical. Known polish debts: no dedicated icon (aliases
  SCULPT_DRAW); weight/enable refresh does a full spatial rebuild (heavy
  at multi-M verts).

- **V4 done.** `source/vdm/vdm_promote.{h,cc}`: eligibility predicate
  (fold bound — face max|D| vs `α_promote·ρ_min` from the shared 1-ring
  fold-radius estimate — plus overhang via ±texel central differences of the
  displaced surface at the face centroid vs the base normal; `α_promote`
  defaults above the splat clamp α, which is the promotion-side hysteresis —
  demotion is X4). `promoteRegion`: pattern-subdivide with callbacks
  threaded (meshlog + spatial), new-corner UVs recovered from 3D position
  barycentrics against pre-subdivide snapshot triangles (the box-modeling
  subdivide leaves default corner UVs on new verts), children classified by
  UV footprint (neighbour fans keep their inherited VDM carrier — carrier
  dropped NOCOPY so topo chunks capture/restore it), verts seeded to
  `base + frame·D(uv)` with the exact splat-time frame (snapshot barycentric
  interp; boundary verts land on the surface the VDM neighbour still
  renders — the C0 pin), footprint texels cleared (incl. dilation margin),
  carrier-boundary edges marked `EDGE_LAYER_REGION` (new persistent
  boundary flag, `BC_LAYER_REGION` bit outside BC_TYPE_MASK, threaded into
  dyntopo FeatureViews + graphStats; edge flags ride their own external
  chunk — `VdmEdgeFlagLogChunk` — since changed-edge packed-bool columns
  don't restore through topo rows). Debug verb `vdm_promote` (alpha/theta/
  cuts/force). Gate green: `test_vdm_promote` — forced-fold stroke promotes
  (seeded maxZ ≈ stroke magnitude, texels cleared, valid topology), ONE
  undo press reverts topology + seeds + carriers + texels + flags together,
  redo replays, and a dyntopo stroke across the promoted seam preserves the
  region boundary (flags propagate to split children).

- **V3 done.** Engine half: `source/vdm/vdm_gpu.{h,cc}` — GPU
  residency packing with the byte layout owned by C++ (gpuBrushes D1 rule):
  stable per-tile atlas slots (recycled via free list), a `grid²` page table
  over UV [0,1]² (tile coords → slot, -1 = zero), full-atlas + per-slot
  rgba32float pixel packing, and a dirty-slot drain (`takeGpuDirty`) whose
  topo flag tells the app when to re-upload the page table — the "no
  regen_gpu_node on VDM-only dabs" upload path. Bound surface
  (`VdmStore::gpuLayoutOut/...` via `Bind<VdmStore>`) reaches both backends
  through reflection; extern-C `VdmStore_new/free` + `Mesh_vdmSplatDab`
  exported for WASM (N-API wraps = app-side threading). Gate green:
  `test_vdm_gpu`. App half: backend threading (napi wraps + the 4-place TS
  change; `sculptcore_vdm.test.ts` proves the packed atlas bit-identical
  wasm vs native), UV-seam dilation skirts in the splatter (gutter fill via
  clamped-barycentric dilation; cross-chart matching stays Ptex/X2), and
  the fragment render path: WgslShaderGenerator VDM mode (@group(3) atlas +
  r32sint page table, manual bilinear — rgba32float is unfilterable, and an
  unsampled binding is reflection-stripped), analytic shading normal
  (±half-texel central differences of the store chained through screen
  derivatives of uv/local position — dpdx of the displaced position itself
  is helper-invocation noise at chart edges), LiteMesh attachVdmStore +
  per-frame dirty-tile writeTexture, encodeMeshBasePass re-push on
  attach/detach. Gate green: `sculptcore_vdm_render.test.ts` — headless PNG
  luminance A/B (vdm≠flat 0.31, ref≠flat 0.37, vdm≈ref at 37% residual +
  NCC 0.87 — the fragment tier shades without moving silhouettes; true
  silhouettes are X3's tessellated tier) + native↔wasm image parity 0.008.

- **V2 done.** `source/vdm/vdm_splat.{h,cc}`: per-dab UV rasterization of the
  brush footprint (tree filterNodes → `.detail.carrier == VDM` gate →
  fan-triangulated corner-UV rasterize; per-dab visited-texel set), falloff
  evaluated in world space from the *displaced* point `base + frame·texel`
  (so accumulation saturates naturally), tangent inversion through the F3
  frame (bary-interpolated, re-orthonormalized), total-magnitude clamp to
  `α·ρ_min` (per-vert fold radius from the 1-ring shape operator, min over
  the triangle; hysteresis/promotion state is V4). Touched faces re-export
  their `.detail.bound` pads (bounds-only spatial dirty). Undo: MeshLog gains
  a generic `LogChunkTypes::External` + `appendChunk` seam; `VdmLogChunk`
  (vdm_undo.h) rides the dab's step, so one undo press reverts geometry AND
  texels (self-inverse delta = same blob undoes and redoes). Debug verbs:
  `vdm_init` (store + carrier tagging + optional planar UV + frames),
  `vdm_stroke`, `save_vdm`/`assert_vdm` (the texel-snapshot analogue of
  save_pos/assert_pos). Gate green: `test_vdm_stroke` — texels land while
  `assert_pos` proves zero vertex motion, root AABB pad grows by exactly
  max|texel|, undo/redo texel round-trips, 50-dab accumulation bounded.
  Deferred: UV-seam one-texel skirts → V3 (with the GPU tile upload);
  wasm↔native parity + app wiring → V5 (the store has no app-side surface
  yet, same as V1's TS threading).

- **V1 done.** `source/vdm/vdm_store.{h,cc}`: sparse tiled float3 store,
  atlas backend behind the `sample(face, u, v)` parameterization seam (face
  unused until Ptex/X2); tiles allocated on first write, unallocated space
  samples zero; per-tile max|D| bounds + conservative UV-rect queries;
  `exportFaceBounds` (corner-UV bbox → tile bounds) feeds F2's
  `setFaceDisplacementBounds`; self-inverse tile-delta undo bracket
  (`beginDelta`/`endDelta`/`applyDelta`, the LogChunkElems::swap pattern —
  V2 brackets it inside the dab's MeshLog step); lz4 BinFile container
  serialization (mirrors `serial::writeMesh`). Gate green: `test_vdm_store`
  (write/sample/bilinear, bound pyramid vs brute force, undo/redo
  round-trips, bit-exact serialize round-trip, per-face export).

Workstream F recap:

- **F1 done.** `AttrUse::SCULPT_LAYER` + `SculptLayerSettings` sidecar
  (`mesh/sculpt_layers.h`, table on `Mesh::sculptLayers`, serialized as mesh
  format v3 — the settings POD lives in `mesh/`, not `displace/`, because
  `mesh_serialize.cc` persists it and mesh can't depend on displace). The
  compositor (`source/displace/compositor.{h,cc}`) treats evaluated `v.co` as
  authoritative with an *implicit* base (`base ≡ co − Σ wᵢ·dᵢ`): strokes fold
  layer-delta edits into co through the region-scoped `LayerEditScope` bracket
  (wired into `CommandExecutor::exec`), settings changes adjust co
  incrementally, and undo cannot desync (co + layers restore atomically; DELTA
  linearity keeps dyntopo interp exactly consistent). New `layerdraw` kernel
  (`save vertex co, no, slayer`), debug verbs `layer_add`/`layer_set`, and
  `stroke layer=` retargeting. Gates green: `test_sculpt_layers`
  (composition/weight/enable/frozen/serialize/split-interp),
  `test_layer_stroke_undo` (2-layer stack undo/redo, weight round-trip);
  wasm↔native parity runs app-side (`sculptcore_layers` integration test).
- **F2 done.** `.detail.bound` (float) + `.detail.carrier`
  (`DetailCarrier{GEOM,VDM}`) FACE builtins on `SpatialTreeMesh`;
  `regen_node_bounds` pads leaf AABBs by the max owned-face bound (gated on
  `hasDetailBounds`); `setFaceDisplacementBounds` /
  `markFacesDisplacementDirty` + public `regenDirtyBounds()`; C-API
  `setTreeFaceDisplacementBounds` (+ wasm symbol). Gate green:
  `test_spatial_displacement_bounds`.
- **F3 done.** Frame provider folded into `source/displace/frames.{h,cc}` (the
  module-map option): smoothed vertex normal + 4-RoSy cross-field tangent
  (`.frames.v.normal` / `.frames.v.tangent`, persistent NOINTERP), the cheap
  heat-diffusion variant, deterministically re-seeded from geometry alone.
  Gate green: `test_frame_provider` — sphere/cube orthonormality,
  Poincaré–Hopf index sum == 4χ == 8, bit-identical recompute.

Next: merge F to master, then create the `displacement` (V) and `subsurf` (S)
worktrees.

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

  Design note (X1 decomposition, decided at implementation time from the
  architecture report §4.1/§6/§8 + sculpt-layers-design §4.2/§8):
  - **Atlas backend first** (X2 swaps in Ptex under the `sample(face,u,v)`
    seam). The parameterization is synthesized, not authored: each cage-corner
    **grid is a chart**, packed into a ⌈√G⌉-per-row atlas grid with an inset
    gutter. Chart layout is a pure function of (gridCount, grid id, lattice
    coord) — deterministic across backends AND **identical at every level**
    (same grid → same chart, param t consistent), so texels authored at the
    finest level sample correctly from any level's UVs. `Multires::materialize`
    writes the per-corner `uv` (FLOAT2, AttrUse::UV) from the grid tables.
  - **Clamp policy**: materialized level meshes carry a runtime topology-lock
    marker; `collectPromotionCandidates` returns nothing on a locked mesh —
    on subsurf the splat clamp `α·ρ_min` is a **true ceiling** (no promotion
    band). Per-splat `texelsClamped` crosses the seam as the **add-a-level
    prompt signal**; the prompt is a non-modal hint offering the existing
    `litemesh.multires_*` level ops (a deliberate op, never automatic).
  - **Level policy**: the VDM is *editable* (splattable) only at the finest
    level; the fragment tier *renders* it at any level (grid charts are
    level-consistent). Amplified true-displacement display is X3.
  - **Deferred (tracked, not X1)**: interactive store lifecycle + stroke-path
    carrier routing (GEOM deform / VDM splat per dab) — production VDM strokes
    need these on polygon bases too; they ride the X3/X4 app pass. X1's bar is
    the V2/V3-style gate: engine + scripted app drivers, both backends.
  - **Follow-up found by the X1 gate — F3 cross-backend frame parity on
    curved bases — RESOLVED**: the raw atlas bytes differed by ulps on a CC
    level mesh (bit-exact on the flat-chart V3 fixture) because the curvature
    tangent seed used libm transcendentals (acos/atan2/cos/sin round
    differently between emscripten and native). `estimatePrincipalDir`
    (displace/frames.cc) is now transcendental-free: the dihedral weight is
    the normal chord `|n1−n2|` (= 2·sin(θ/2), monotone) and the 2×2
    eigen-direction comes from half-angle identities — only IEEE-exact
    +,−,×,÷,√ remain, so frames (and therefore VDM texels) are bit-identical
    across backends. All frame gates unchanged (orthonormality,
    Poincaré–Hopf = 8, recompute diffs = 0); the X1 atlas parity gate is back
    to **exact** raw-checksum equality. `crossFieldIndexSum` still uses atan2
    but is a gate metric, not data. The brush-side twin
    (brush/feature_field.cc) keeps its transcendental form — it is
    interactive-only, never a parity anchor.
- **X2 — Ptex backend** for VdmStore (per-face grids + adjacency + skirts),
  sharing S2's grid conventions; fragment path binds per-face table.

  Design note (X2 decomposition, decided from the source inventory):
  - **Why now**: the atlas's single global `resolution` under-resolves charts
    as the cage grows (chart span ∝ 1/√G) — per-grid resolution is the
    scalable carrier; that is where the face↔patch identity "pays".
  - **Backend = a mode inside `VdmStore`, not a class hierarchy.** The delta
    undo channel, GPU slot atlas + dirty drain, per-tile bounds, C-API,
    bindings, and `VdmLogChunk` are all key-agnostic over `uint64` tile keys
    — introducing `VdmBackend::{ATLAS, PTEX}` and branching sample/write/
    pack/serialize keeps every existing seam (including `Mesh_vdmSplatDab`)
    intact. Ptex tile keys are `(gridId << 32) | tileIndexWithinGrid`; each
    grid owns an `R_g × R_g` texel lattice (power of two, default from
    params; per-grid override is the adaptivity hook).
  - **Patch identity = S2's cage-corner grids**, but `vdm` stays subdiv-free:
    the adjacency (4 `GridLink`s per grid, the transpose seam convention) is
    *provided* to the store (`setPtexAdjacency`) by the owner — Multires
    hands over its `GridsStore` links; a polygon base could hand any
    quad-chart adjacency. Cross-grid **skirts** (X2's headline: seamless
    bilinear) copy border texels through those links at splat end — the
    `seamMates`/transpose walk, re-expressed over texels.
  - **Splat path**: level meshes get exact per-corner `(grid, localU, localV)`
    from `assignGridUVs`'s arithmetic (emitted as attrs alongside the packed
    chart uv); the Ptex branch rasterizes each face in its grid's own texel
    lattice — no packing loss, no gutters in data space.
  - **Fragment path stays UV-routed — no flat varyings needed**: the X1
    grid-chart uv recovers the grid id exactly via `floor(uv·cpr)` (the inset
    gutter keeps interior fragments in-cell), then a per-grid offset table
    (an `i32` texture — WGSL reflection has no storage-buffer path) gives
    slot base + `R_g`, and local uv = (uv − cell − inset)/span addresses the
    grid's tiles. Local-uv float precision is ample (≥3e-5 of a chart).
  - **Serialization**: `kVdmFormatVersion` 1 → 2 (backend tag; Ptex payload =
    per-grid {gridId, R_g, tiles}); the version guard already exists.
  - Stages: (1) store mode + per-grid tiles + sample/write + delta + ctest →
    (2) splatter Ptex branch + skirts-via-adjacency + ctest →
    (3) GPU table + WGSL + app upload + render A/B →
    (4) parity + serialization + docs.
  - **Stage 1 DONE**: `VdmBackend::{ATLAS,PTEX}` on `VdmStoreParams`;
    per-grid `R_g×R_g` lattices (`setPtexGridCount`/`setGridRes`/
    `setPtexAdjacency`, tiles keyed `ptexTileKey(grid, tileIdx)`), grid-local
    `texelP/writeTexelP/addTexelP` + clamped `sample(grid, u, v)` +
    `gridBound`; the delta bracket + GPU-slot machinery reused unchanged
    (shared `ensureTileAt`, backend-branched key decode in `applyDelta`);
    format v2 (backend tag + grid/adjacency tables + per-tile grid id, v1
    reads as atlas). Gate: `test_vdm_store` Ptex block (isolation, bilinear,
    override-res grid, delta round-trip, bitwise v2 round-trip); all V-track
    vdm gates green under v2.
  - **Stage 2 DONE**: per-grid lattices grew the one-texel **guard ring**
    (storage (R+2)²; payload coords stay [0,R), −1/R address the guard;
    `sample` taps land on it, so clamped bilinear is seamless with zero
    render-time adjacency lookups — the §6 "copied border skirts").
    `syncGridSkirts` fills guards from neighbour border payload through the
    provided links (t preserved, roles swapped — exactly grids.cc's
    `sideCoord`/`neighbor` convention; nearest-texel across resolution
    changes; diagonal guards average their edge neighbours).
    `assignGridUVs` emits the exact Ptex parameterization as corner attrs
    (`.ptex.c.grid` INT + `.ptex.c.uv` FLOAT2) alongside the packed chart uv;
    the splatter's PTEX branch rasterizes each face in its grid's own lattice
    (per-grid res, (grid,x,y) visited keys, guard-ring write window) and
    refreshes skirts of touched grids + their link targets at splat end
    (rides the open delta → undo-safe); `exportFaceBounds` reduces per-grid.
    Gates: `test_vdm_store` skirt block (guards bitwise == neighbour payload,
    seam-continuous bilinear) + `test_multires` `gatePtexSplat` (wide dab on
    the CC cube: 768 seam samples over all 96 links, worst discontinuity
    1.5e-8). Next: stage 3 — GPU per-grid offset table (i32 texture) + WGSL
    grid recovery via `floor(uv·cpr)` + app upload; then stage 4 parity/docs.
  - **Stage 3 engine half DONE**: `gpuPtexTable` / bound `gpuPtexTableOut` —
    the flat i32 per-grid offset table the fragment path binds instead of the
    [0,1]² page table (`out[0]`=G; per-grid `{slotTableOffset, R_g, tps}`
    headers; then tps² slot ints per grid, storage coords incl. the guard
    ring, −1 absent). `gpuLayoutOut` grew to 10 ints (`[8]`=backend,
    `[9]`=gridCount; older readers consume the first 8). Gate: `test_vdm_gpu`
    PTEX block (exact header offsets/res/tps, slot occupancy).
  - **Stage 3 DONE — X2 COMPLETE.** WGSL `VDM_PTEX` sampler
    (shader_nodes_wgsl.ts: same `vdmSample` seam, shared preamble; grid id =
    `floor(uv·cpr)`, local param un-inset, +1 guard-ring storage offset,
    per-tap slot lookups so taps straddle tiles and land on the copied
    skirts); LiteMesh `_syncVdmGpu` ptex branch (flat table as a 1024-wide
    i32 texture on the page-table binding; `vdmGridSize` carries cpr and
    `vdmResolution` the effective texels-per-packed-uv R/span for the
    derivative epsilon; backend detected once at attach → `vdmIsPtex`);
    renderengine folds `VDM_PTEX` into the material hash; bound seam
    `Multires::vdmAdjacencyOut` + `VdmStore::configurePtex` (the app carries
    the S2 links across — vdm and subdiv stay decoupled). Gate green
    (`sculptcore_multires` 29/29): screenshot A/B — the ptex sampler
    displaces shading meanAbs(ptex−flat)=0.295 (atlas path: 0.31),
    native↔wasm image parity 0.0092 with exact texel/tile-count equality
    (the stage-4 parity bar); full ctest + vdm/layers/parity suites
    unchanged. X2 debts → X-track: per-grid res adaptivity unused by callers
    yet; `.wproj` persistence still flatten-on-save.
- **X3 — Tessellated render tier**: V3's shader + S5's amplification =
  true-displacement opt-in; per-region selection from compositor state.

  Design note (X3 decomposition, from the S5/app-integration survey):
  - **The gap**: S5's `WgpuStencilAmplify` runs on the NATIVE wgpu device;
    the app draws on the TS WebGPU device. X3 re-dispatches the same SpMV
    there (the gpuBrushes model — C++ owns layouts, TS uploads + dispatches),
    porting `kSpmvWgsl` verbatim as a hand-written TS constant (the
    `litemesh_wgsl.ts` shipping precedent). The **bit-exactness contract**
    carries over unchanged: ascending-row CSR order + `fma` per component on
    both sides; the CPU chain (`StencilTable::eval`) is the oracle.
  - **Export seam (new)**: bound `Multires` out-param methods emit the
    per-level CSR (`stencilMetaOut/OffsetsOut/IndicesOut/WeightsOut`; meta =
    {coarseCount, fineCount, nnz}) plus `levelTriIndicesOut` (two triangles
    per grid cell straight from `gridVerts` — no materialized mesh). Source
    positions reuse `dumpVertCo` on the active (edit) level mesh.
  - **Stages**: (1) TS-device SpMV dispatcher (`scripts/webgpu/
    stencil_compute.ts`, cloned from brush_compute's parseBindings/ensureBuf/
    readback core) gated on **bit-parity vs the CPU chain** (materialize the
    render level, compare Float32-exact) + cross-backend checksums →
    (2) the tessellated draw: on-device result buffer (Storage|Vertex) +
    static index buffer, drawn in `drawQGPU` via the compiled material
    pipeline (direct `setVertexBuffer`/`drawIndexed` — the executor is
    non-indexed); fragment-derived flat normal (`cross(dpdx,dpdy)`) as the
    stage-2 shading stopgap; screenshot gate →
    (3) the S5-deferred second pass: smoothed-frame build at amplified verts
    (transcendental-free per the F3 parity rule) + VDM apply (V3's sampler
    seam) = true displaced silhouettes; screenshot A/B vs the fragment tier →
    (4) per-region selection (`.detail.carrier` + compositor predicate:
    fragment tier vs tessellated per face) + caching (skip re-amplify when
    the coarse level is clean) + the interactive app pass items deferred from
    X1/X2 (store lifecycle, per-dab carrier routing, add-a-level prompt UI).
  - **Bounds**: target ≤ L6 on the TS device initially — L7 CSR (~120 MB/
    level) sits against the 128 MiB storage-binding ceiling; single-level
    chunking is a follow-up (risk #5). Whole-object tessellated toggle first;
    per-region mixing lands in stage 4.
  - **Stage 1 DONE.** Export seam: bound `Multires::stencilMetaOut/
    OffsetsOut/IndicesOut/WeightsOut` + `levelTriIndicesOut`; TS dispatcher
    `scripts/webgpu/stencil_compute.ts` (verbatim `kSpmvWgsl` port, chained
    per-level passes, 2D-linearized dispatch, on-device result option).
    **Finding**: Dawn's D3D12 path lowers WGSL `fma` UNFUSED — the TS-device
    SpMV differs from the CPU chain by 1-ulp-class noise (maxAbsErr 4.8e-7 on
    the gate fixture; native wgpu/Vulkan remains bit-exact per S5). Gate
    design therefore splits: the EXPORT SEAM is gated bit-exact via a JS
    fma-exact CSR evaluation (f64 mul+add + one fround == f32 fma;
    `jsVsCpu == 0` both backends — the marshal/order/src contract), the GPU
    result gets a display-tier absolute tolerance (amplified verts never
    enter the mesh), and determinism is gated by EXACT cross-backend GPU
    checksums. `sculptcore_multires` 34/34. The JS-eval triangulation
    (jsVsCpu / jsVsGpu) is a reusable pattern for gating marshal seams
    independently of GPU rounding.
  - **Stage 2 DONE — the tessellated draw.** `TESS_TIER` material variant
    (WgslShaderGenerator: position-only VsIn, default-filled varyings — white
    for color-category attrs, mirroring sculptcore's missing-layer fills —
    and a fragment flat normal from `-cross(dpdx, dpdy)` of the world
    position; the sign matters — framebuffer y points down); the renderengine
    generates it per material hash and hands it to the mesh
    (`setTessDrawWgsl`). LiteMesh `tessellatedDisplay` (view state):
    `_ensureTessBuild` marshals the CSR chain (activeLevel, maxLevel],
    amplifies async on the renderer device (`keepResult` → the on-device
    Storage|Vertex buffer), uploads the static render-level index buffer, and
    `_drawTessellated` substitutes a direct `setVertexBuffer`/`drawIndexed`
    under the TESS_TIER pipeline for the batch dispatch (falls back to the
    batch until the async state lands; never throws on the render seam).
    Gate green (`sculptcore_multires` 37/37): screenshot A/B — tess vs the
    CPU-materialized fine level meanAbs 0.016 (flat-shading residual only)
    vs 0.22 level separation; native↔wasm 0.0055. Stage-2 limitations
    (ride stage 3/4): the NormalPass/AO still sees the coarse batch (the MRT
    guard falls back there), no SSS-MRT tess variant, re-amplify keyed on
    meshRevision (no finer-grained caching), smooth normals + VDM apply come
    with the stage-3 frame pass.
  - **Stage 3 DONE — smooth frames + the VDM apply (the S5-deferred second
    pass).** Frames amplify through the SAME stencil SpMV (three channels:
    positions + `.frames.v.normal`/`.tangent` from the edit level via new
    `Mesh::dumpFrameNormals/Tangents` bound methods — fixed names because
    strings can't cross the generic binding); a two-pass `tessFinalize`
    kernel then (A) orthonormalizes the frame and displaces each vert by the
    Ptex VDM texel at its grid-lattice param (`levelVertGridCoordsOut`;
    table+atlas as compute storage buffers) and (B) computes GEOMETRIC
    normals over the DISPLACED positions by lattice central differences
    (`levelGridVertsOut`), written only by each vert's canonical owner site —
    deterministic at seam replicas. The TESS_TIER variant takes
    position+normal streams (flat-normal preamble dropped). Gate green
    (`sculptcore_multires` 38/38, 3-level fixture so the chain spans two
    stencil levels): tess-vs-fine 0.055 vs 0.23 level separation (residual =
    4-neighbour lattice normals vs full 1-ring v.no), **tessvdm displaces
    the silhouette** (0.37 vs plain tess), native↔wasm 0.004-0.006.
    Hard-won lessons: drivers must WAIT for the async tess state before
    screenshotting (readiness poll — the draw falls back to the batch until
    it lands, so screenshots race), and a stale wasm binary let the fragment
    tier impersonate the tessellated one for a while (rebuild ALL backends
    after adding bound methods). Residual limitations → stage 4: frame
    channels amplified from the edit level (not the finest-authored frames),
    one-sided normals at grid borders, NormalPass/AO coarse fallback, no
    SSS-MRT variant.
  - **Stage 4a DONE — the interactive VDM app pass** (deferred from X1/X2).
    Engine: `Mesh_vdmSplatDabLogged` (the splat delta-bracket from the debug
    verb as a C-API — beginDelta → splat → endDelta → `VdmLogChunk` appended
    to the OPEN MeshLog step) + `VdmStore_serialize/deserialize` blob pair
    (v2 container; params + Ptex tables ride the blob), all napi-wrapped +
    4-place TS-threaded; `VdmStore::write` now emits tiles in **canonical
    key order** (map iteration reshuffles on delta remove+reinsert, which
    made undo/redo blobs checksum-differ; blobs double as determinism
    anchors + future save files). App: feature flag `sculptcore.vdm_sculpt`
    (default off) gating a LiteMesh panel (Enable/Delete VDM) + the undoable
    `litemesh.vdm_{enable,delete}` ops — enable builds a Ptex store from the
    multires stack's S2 adjacency (atlas over an existing unwrap otherwise);
    **lifecycle undo RELEASES the store instance instead of freeing it**
    (stroke history holds non-owning VdmLogChunk pointers, so the same
    instance must return on undo/redo; freed-store replay would crash).
    Dab routing: with a store attached, Draw dabs in BOTH the interactive op
    and `runSculptcoreStroke` splat texels (no vertex moves, GPU-brush path
    naturally skipped since DRAW has no kernel) with the engine-default
    fold clamp α=0.5; `Vdm_lastSplatClamped` surfaces the once-per-stroke
    add-a-level note. Gate: `sculptcore_multires` **49/49** — new
    interactive describe proves texels splat + vertices hold bit-still,
    stroke undo/redo round-trips the store blob exactly, delete+toolstack
    undo restores it intact, and tile counts + blob FNV checksums are
    identical wasm↔native. Known 4a leftovers → 4b: a splat does not bump
    meshRevision, so a tessellated display won't re-finalize until the next
    geometry edit (fold into the caching work); per-region carrier
    selection; NormalPass/AO substitution; SSS-MRT variant. (Pre-existing,
    unrelated: `sculptcore_brushes` symmetrize missBefore gate fails on
    this branch with and without 4a — triage separately.)
  - **Stage 4b DONE — split caching + the preview toggle; X3 CLOSED.**
    `VdmStore` gains a bound `contentRev()` (monotonic texel-content
    revision: bumps on writes, delta applies — undo/redo — and tile
    removals). The tess build is now split-cached: geometry edits re-run
    the whole chain (`meshRevision`-keyed, as before), a texel-only change
    re-runs JUST the finalize over the kept amplified position/frame
    buffers (`_refinalizeTess` — no SpMV re-dispatch, no index rebuild), so
    interactive VDM strokes update the displaced preview live. The panel
    gains **Displaced Preview** (`object.data.tessellatedDisplay`, view
    state) — the object-level render-path predicate. Gate: the tessvdm
    driver splats a second dab AFTER the build and waits for the storeRev
    catch-up (`refinalized`); `sculptcore_multires` 50/50, both bumps
    visibly displace the silhouette (tessvdm-vs-tess 0.51). Re-scoped to
    documented debts (multires.md): per-FACE carrier mixing is dormant
    (promotion is gated off on topo-locked level meshes → carrier tags are
    uniformly VDM until X4 demotion); NormalPass/AO substitution folds into
    the pre-existing LiteMesh-wide NormalPass skip (M6 "no SSAO" note);
    SSS-MRT turns out latently broken for ALL LiteMesh draws (the batch
    executor is seeded single-target and `setColorFormats` cannot grow it)
    — repairing SSS+LiteMesh (batch + tess) is its own work item, not X3's.
- **X4 — Cross-carrier bakes**: VDM→vertex-layer extraction, geometry→VDM
  demotion (explicit op), external VDM export (frame-synchronized).

  Design note (X4 decomposition): the bakes are EXPLICIT ops (the hybrid
  doc's demotion stance: never live per-dab), sharing the splatter's exact
  frame construction (`t ⊥ n`, `b = n × t`, the same `sample(face, u, v)`
  seam — the bake ≡ render rule). Stages: (1) **VDM→geometry apply** —
  per-vert texel sample displaces the vert, store cleared; on multires the
  result folds through `multiresWriteback`; (2) **geometry→VDM capture**
  (the inverse: rasterize per-face vert deltas vs a smooth base into
  texels, restore verts onto the base); (3) **store persistence/export**
  (the `.wproj` debt + the frame-synchronized external blob). Undo model:
  op-level blob snapshots (mesh blob on plain meshes / multires-store blob
  on level meshes + the VDM blob), with the store refilled IN PLACE
  (`VdmStore_restoreBlob`) — stroke-history VdmLogChunks hold non-owning
  pointers into the instance, so it must never be freed + replaced.
  - **Stage 1 DONE — VDM→geometry apply.** Engine: `vdm_bake.{h,cc}`
    `applyToVerts` (corner-param walk — Ptex attrs or the UV layer — first
    corner per vert; frame displace; optional `VdmStore::clearTiles`, a new
    GPU-aware non-delta clear) + C-API `Mesh_vdmApplyToVerts` (frames
    refreshed inside) and `VdmStore_restoreBlob` (in-place refill), napi +
    4-place threaded. App: `litemesh.vdm_apply` op + panel button — bakes,
    folds into the grids store on multires, rebuilds spatial; undo restores
    the multires-store blob (or mesh blob) + refills the SAME store
    instance. Gate: `sculptcore_multires` 52/52 — bake moves the position
    checksum, store empties, ONE toolstack undo restores positions
    bit-exact + all tiles + the exact blob checksum, and the baked
    positions are identical wasm↔native.
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
