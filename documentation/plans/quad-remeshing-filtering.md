# Quad-Remesh Filtering & Robustness — Tiered Implementation Plan

Prefiltering / filtering / robustness improvements for the quad remesher
(`source/remesh/`), targeting **organic / Meshy-generated**, **scanned /
photogrammetry**, and **messy character** meshes, with **adaptive density**.
CAD / hard-surface is *not* a primary target — which is why feature-graph
cleanup is the last *filter* tier.

Background and trade-off analysis:
[research/quad-remeshing-filtering.md](../research/quad-remeshing-filtering.md).
Shipped pipeline being extended: [quad-remeshing.md](../quad-remeshing.md).

**Process:** each tier is implemented, tested, then **paused for review** before
the next begins. Tiers are additive. Knob defaults are decided per knob on merit
at the tier's review gate — correctness-critical passes (e.g. Tier 3b gradation)
default **on**; destructive cleanup (Tier 1 triage) defaults **off** until
validated. Each tier's param table records the chosen default.

## Roadmap & rationale

The derivative-order hierarchy (positions 0th, normals 1st, curvature 2nd) makes
the cross field the noisiest stage, so the *filter* budget goes on the derived
field, not geometry. But for **messy character meshes**, two non-filter concerns
dominate quality and so come first: you can't tell if a filter helped without
**metrics**, and bad input **topology** poisons every later stage.

| Tier | Theme | Kind | Why here |
|---|---|---|---|
| **0** | Quality metrics + ugly-char corpus + stats plumbing | measurement | Can't evaluate any later tier without it; unblocks the capstone's retry decisions. |
| **1** | Input triage / repair (scoped) | topology | Garbage topology poisons curvature→field→param. Biggest lever for *messy* inputs. |
| **2** | Curvature tensor filtering + integration radius | field filter | Highest-leverage low-risk denoise for organic+scanned; no geometry change. |
| **3** | Auto curvature-density + gradation limiting | sizing | Generates a size field (small at face/folds, large on torso) + bounds its per-edge growth ratio. |
| **4** | Smoothness/alignment master knob | field filter | Dial smoothness up to suppress noise-born singularities. |
| **5** | Singularity pair cancellation | field cleanup | Removes residual +k/−k clutter Tier 2 didn't eliminate. Gated on whether it's needed. |
| **6** | Boundary / thin-part / component policy | topology policy | Eyelids, mouth holes, cuffs, accessories, double-sided sheets — a big character lever. |
| **7** | Feature-graph hysteresis + pruning | field filter | **Last filter tier** (CAD off-target). Cleans noisy hard constraints. |
| **8** | Retry/robustness policy + presets | orchestration | Capstone: turns knobs into an automatic policy; needs Tier 0 metrics + all knobs. |
| **9** | Field-aligned input pre-remesh | geometry (input) | The only tier that remeshes the *input triangulation* — cleans its flow before the field solve. **Exec order: early** (after Tier 1, before Tier 2); numbered last to avoid renumbering in-flight tiers and so it can use Tier 2's smoothed curvature + Tier 0's metrics. |

## Per-param surface area (repeats whenever a `RemeshParams` field is added)

1. `source/remesh/remesh_params.h` — field + default + ≤3-line doc comment.
2. `source/remesh/bindings.cc` — `BIND_STRUCT_MEMBER(st, <field>)`.
3. The owning **stage** struct (e.g. `CrossFieldParams` in `field/cross_field.h`,
   `QuantizeParams` in `quantize/quantize_ilp.h`) + its consumer `.cc`.
4. `source/remesh/remesh.cc` — thread `params.<field>` into the stage struct.
5. `source/remesh/cli/remesh_cli.cc` — `--flag` parse + pass-through + usage line.
6. `source/debug/remesh_app.cc` — `runRemesh` arg builder, `get_params` print,
   `set_param` handler.
7. `source/debug/remesh_ui.cc` — one ImGui widget.
8. `scripts/lite-mesh/litemesh.ts` — `opts.<camelCase>` mapping (host TS path).
9. **`scripts/lite-mesh/litemesh_ops.ts`** — `QuadRemeshLiteMeshOp` declares its
   **own** input properties (`tooldef().inputs`) and its `exec` **always passes
   every field**, overriding the bound struct's defaults (class doc at
   `litemesh_ops.ts:585`). Add a property here *and* in the exec mapping. Its
   defaults already **diverge** from C++ (e.g. `useSharpFeatures`/`reproject`);
   for each new knob explicitly decide whether the **C++ default** or an
   **app-UI default** is authoritative, and document it inline.

**Defaults:** decided per knob at its tier's review gate, not by blanket rule.
Destructive cleanup (Tier 1 triage) stays **off** until gate 1 validates it on
the corpus; correctness-critical filters (e.g. Tier 3b gradation in the pre-pass)
default **on**.

---

## Tier 0 — Quality metrics, corpus, and stats plumbing

**Goal:** an honest, repeatable quality readout so every later tier's review gate
has real numbers, and a fixed corpus of ugly character/scan assets to track over
time. Pure measurement — no algorithm change, lowest risk, highest unblock.

### 0a. A run-report API (do this first — Tier 8 depends on it)
`QuadRemesh` returns only `Mesh*` (`remesh.cc:172`) and the CLI validates only
*after* success (`remesh_cli.cc:306`), so there is nowhere to put failed-attempt
metrics, cross-field stats, fold counts, or a failure reason. Add a
**`RemeshRunReport` out-param** (or a `QuadRemeshEx` entry) carrying: per-stage
status, `CrossFieldStats` (`num_singularities`/`index_sum` — computed but never
surfaced), pre-extraction parametrization fold count, the validation block, the
duration, and a **failure-reason** string. The CLI/manifest and Tier 8's retry
loop both read this. Without it the retry policy flies blind.

> **Cross-boundary surface — decide explicitly.** The C entry `Mesh_quadRemesh`
> returns only `Mesh*` (`remesh.h:39`); the report is C++-only via the out-param.
> If Tier 8's retry lives **entirely inside C++** (recommended — it re-solves in
> the same process), the internal out-param is enough. If the **host/UI** needs
> the winning attempt's metrics / failure reason, add a separate report
> query/export across the C API (e.g. a `Mesh_quadRemeshEx` returning a bound
> report struct) — do **not** smuggle it through `Mesh*`. Default plan: keep retry
> in C++, expose only a compact summary to the host.

### 0b. Plumb stats + extend the quality report
STATS/manifest currently emit only `irregular_interior_verts` (`remesh_cli.cc:339`,
`:187`). `mesh_validate.h`'s `RemeshReport` **already computes** more than is
emitted — notably `valence_hist[17]` (`mesh_validate.h:47`, filled at `:371`) and
the full manifold/euler/winding/degenerate/inverted/all-quad/isoline set. So much
of this is **surface what exists**, not build from scratch:
- **% regular interior verts needs an interior count, not just `valence_hist`.**
  `valence_hist[17]` is filled for **every** vertex including boundary ones
  (`mesh_validate.h:344`), while `irregular_interior_verts` counts only non-boundary
  `valence != 4` (`:371`). So `%regular = 1 − irregular/total` is **wrong on open
  character meshes**. Add `interior_vert_count` (and/or an `interior_valence_hist`)
  and derive `%regular = 1 − irregular_interior_verts / interior_vert_count`;
- **surface** the existing structural/isoline fields to STATS;
- **add**: **max adjacent-quad area ratio** + **max edge-length ratio**;
- **add**: skinny-quad / **min-interior-angle** proxy (worst + histogram);
- **add**: **min scaled Jacobian per quad** (worst + mean) and **max inner angle**
  — the headline metrics of the layout-embedding literature
  ([research/layout-embedding-optimization.md](../research/layout-embedding-optimization.md)),
  so corpus numbers are comparable to published results;
- **add**: **component count** + **hole count** (input vs output) — **define these
  precisely**: *component count* = connected components over the face-adjacency
  graph; *hole count* = number of **boundary loops** (NOT genus-derived handles,
  NOT output residual cap loops — those are separate). Document the definition next
  to the metric so cross-run comparisons are meaningful;
- **add**: per-component singularity count;
- **add**: pre-extraction **parametrization fold count** (distinct from output
  `inverted_faces`);
- **add**: interior-edge **geometric fold counts** — fold90 (adjacent unit face
  normals dot < 0, pathological crease) and fold180 (dot < −0.95, true
  fold-back) — plus degenerate-face count, sampled on the input, the Tier 9
  pre-pass output, and the final output;
- runtime + failure reason come from 0a's report.

### 0c. Corpus + tracker
Assemble a small fixed set (~6–10) of deliberately ugly assets (Meshy character,
raw scan, thin-sheet clothing, holed face, multi-component accessory). Add a
debug-app/CLI batch verb that runs all of them and writes a metrics table
(CSV/JSON) so tiers can be compared run-over-run. Keep assets out of git if large;
reference by path.

### 0d. Early slice (pulled forward by Tier 9's early execution)
Tier 9 executes before the rest of the ladder, so the minimum measurement it
needs is promoted ahead of the corpus: (1) the **geometric fold metrics** above
as permanent debug-app/CLI stats, and (2) the dyntopo **convergence trace**
(`dyntopo_trace.h`: per-round op counts, thin-triangle quality, split/collapse
candidate counts, worst band overshoot/undershoot, and the oscillation/churn
report) surfaced the same way. Both are already prototyped as test scaffolding
in `tests/test_remesh_preremesh.cc`; promote them when 9d lands. The
corpus/tracker (0c) can land later without blocking gate 9.

### Verification
A batch run over the corpus emits the metrics table; spot-check a couple of
values by hand against a known asset.

### Review gate 0
Inspect the metrics table on the corpus as the **baseline**. Confirm the new
numbers are sane and reproducible (fixed seed → identical table). Everything
after is measured against this.

---

## Tier 1 — Input triage / repair (scoped)

**Goal:** make the working copy sane before any field math. `buildTriCopy`
(`remesh.cc:35`) currently copies only the outer boundary and assumes clean,
hole-free, manifold input — messy characters violate all of that.

**Scope tightly** (full mesh repair is a research field; do the high-value,
low-risk subset):
- **Weld** near-coincident / duplicate vertices. **Not** `edge_collapse.h` — that
  only collapses *connected* edges, but messy meshes have verts that are
  spatially coincident yet **unconnected** (split seams, stacked shells). Use a
  **spatial hash → union-find** over a tolerance from bbox scale, then **rebuild**
  topology on the remapped verts with explicit rules for **duplicate faces**
  (drop) and **faces that degenerate after remap** (drop). This is its own small
  routine, not a reuse of the collapse op.
- **Drop degenerate / zero-area faces** and zero-length edges.
- **Drop tiny disconnected components** below a vertex/area threshold (and count
  them — feeds Tier 6's per-component decisions).
- **Detect + report** non-manifold edges/verts via `mesh_validate.h`; do **not**
  attempt full non-manifold repair in v1 — record the reason in the report/manifest
  and either continue on the largest manifold patch or fail cleanly with a message.
- **Defer**: self-intersection / coincident-shell detection (expensive,
  research-grade). Note it as out-of-scope in the manifest, don't fake it.

### 1b. Copy/remap input constraint attrs onto the working mesh (unblocks Tier 3)
`buildTriCopy` (`remesh.cc:35`) copies **only positions + topology**, so **no
host-painted constraint reaches the solver** and Tier 3's "host supplied a density
map" branch is undecidable. Add a pass copying the **input** constraint layers by vertex map.

> **Face-domain `stroke_dir` needs the attr-preserving triangulation path.**
> `buildTriCopy` triangulates with `triangulateMesh` (`triangulate.h:121`), which
> rebuilds n-gons via Euler ops and **drops face attrs**. The attr-preserving
> helper is `triangulateFaceFanCb` (`triangulate.h:21`, snapshots/restores face +
> corner attrs). So `stroke_dir` copied **before** `triangulateMesh` is lost on
> the split faces, and copied **after** has no source-face mapping. Fix: switch
> `buildTriCopy` to the **`triangulateFaceFanCb`** path (per-face, preserves
> `stroke_dir` onto each fan triangle) — or record a source-face→split-face map
> and transfer afterward. Vertex/edge layers (`density`, `pole_pinned`) are
> unaffected (verts are preserved by the vmap); only the **face** layer needs this.

> **Copy the input layers, NOT the computed ones.** The `remesh.cc:24` TODO says
> "`pole_index`", but `.remesh.v.pole_index` is **computed output** (written by
> the field solve at `cross_field.cc:242`, consumed by `singularity_adjust` /
> `cut_graph` / `quad_extract`). Copying an input `pole_index` would **collide
> with / be overwritten by** the computed field. The actual **user-pin input**
> layer is `.remesh.v.pole_pinned` (bool, `singularity_adjust.cc:200`). So 1b
> copies **`.remesh.v.density` + `.remesh.v.pole_pinned` + `.remesh.f.stroke_dir`**
> only.
>
> **`pole_pinned` v1 semantics = "protect a computed pole," not "author one."**
> `singularity_adjust.cc:230` freezes the *prior* (solver-computed) index at a
> pinned vertex — so a user can only **keep whatever singularity the field put
> there**, not request a specific +1/−1. State this limitation explicitly. If
> authored pole *targets* are wanted later, add a dedicated input layer
> `.remesh.v.pole_target_index` (short) — separate from both the bool
> `pole_pinned` and the computed `pole_index`. Out of scope for v1.

Until this lands, **auto-density (Tier 3) is the only supported density path** —
state that explicitly rather than implying painted maps work.

**Where:** a new helper (e.g. `remesh/triage.{h,cc}`) + the attr-copy folded into
`buildTriCopy`, invoked right after the copy, before the pre-remesh/curvature.
`mesh_validate.h` for detection.

### New params
| field | default | meaning |
|-------|---------|---------|
| `triage` | `false` | run input cleanup (weld/degenerate/tiny-component drop). Default **off** until review gate 1 validates it; flip on after |
| `triage_weld_rel` | `1e-5f` | weld tolerance as a fraction of bbox diagonal |
| `triage_min_component_frac` | `0.0f` | drop components below this fraction of total verts (0 = keep all) |

(`triage` defaults **off** — destructive cleanup stays opt-in until gate 1
validates it on the corpus. The attr-copy of 1b is unconditional — it only *adds*
layers, never destroys.)

### Verification
- **gtest** `tests/test_remesh_triage.cc`: build meshes with injected duplicate
  verts, a zero-area face, a 3-vert dust component; assert each is removed and a
  clean cube is untouched. Assert the post-triage mesh passes `mesh_validate`'s
  degenerate/duplicate checks.
- **Corpus:** Tier 0 metrics before/after triage — component count should drop to
  the intended bodies; degenerate/non-manifold counts should fall.

### Review gate 1
Inspect: triage gtest, corpus component/degenerate deltas, and confirm a clean
asset is byte-identical through triage (no-op on good input). Decide the default
for `triage` (on vs off).

---

## Tier 2 — Curvature tensor-field smoothing + integration radius

**Goal:** a denoised principal-curvature field feeding `gatherConstraints`,
without altering geometry. **Where:** `field/curvature.cc`; today the per-vertex
shape operator `T` (`curvature.cc:94`) is eigendecomposed immediately
(`curvature.cc:137`) from the 1-ring only.

### 2a. Tensor diffusion (do first)
- Accumulate `T[v]` as now but **store the full tensor field** (6 unique
  components or `Eigen::Matrix3d` per vertex, sized by `m.v.capacity()`) instead
  of eigendecomposing in the same loop.
- Run `iters` Jacobi sweeps: `T[v] ← (1−λ)·T[v] + λ·(Σ_n w_vn·T[n]) / (Σ_n w_vn)`,
  neighbors `n` over the one-ring disk walk.
  - **Tensors are symmetric but NOT PSD** — `T` accumulates `beta·(ê⊗ê)` with
    `beta` a *signed* dihedral (`curvature.cc:112`), indefinite on saddles. Fine:
    averaging is linear and preserves symmetry; the downstream solver is
    `SelfAdjointEigenSolver` (needs symmetry, not definiteness). Do **not** assume
    PSD anywhere.
  - **Specify `w_vn`** (not "varea-style"; `varea` is per-vertex *mass*, not an
    edge weight): start with **uniform** `w_vn = 1`; if it over-smooths near
    irregular valences, switch to a mass weight `w_vn = varea[n]`. Pick one,
    document it. Avoid cotangent weights (can go negative, re-inject noise).
- *Then* eigendecompose the smoothed `T[v]`, keeping the tangent-plane projection,
  the kmin/kmax direction **swap** (`curvature.cc:177`), and re-orthogonalization
  (`curvature.cc:197`) verbatim.

### 2b. Integration radius (optional; build only if 2a is insufficient)
Generalize the 1-ring accumulation to an `radius`-ring / geodesic ball
(`radius × mean_edge_len`): BFS the k-ring, accumulate the edge-based tensors over
the wider neighborhood before 2a's diffusion. `radius = 1` reproduces today.
Measure 2a on real scans first.

### New params
| field | default | meaning |
|-------|---------|---------|
| `curvature_smooth_iters` | `0` | Jacobi sweeps over the tensor field (0 = today) |
| `curvature_smooth_lambda` | `0.5f` | per-sweep blend `0..1` |
| `curvature_radius` | `1` | integration neighborhood in rings (2b only) |

Thread into `CrossFieldParams`; `computeCurvature(Mesh&, …)` gains a params
struct/args.

> **Overlay cache invalidation (required).** `prepareOverlays`
> (`remesh_debug_app.cc:658`) recomputes curvature only when the temp layer is
> first created (`kmin_dir.ensure()` true). Store a **params signature** (hash of
> the smoothing knobs) on `RemeshApp`; recompute when it changes OR the layer was
> just created — so the overlay shows the *smoothed* field after a knob change.

### Verification
- **gtest** `tests/test_remesh_curvature_filter.cc`: noised cylinder — assert mean
  `kmax_dir` angular error vs the analytic circumferential direction **drops**
  with smoothing, and the clean cylinder is barely changed (no over-smoothing).
  Sphere: smoothing preserves umbilic isotropy (`|kmin−kmax|` small).
- **Debug-app visual:** organic asset, curvature overlay at iters `0` vs `4`
  (de-noise visible), then `run_remesh` both ways and compare via Tier 0 metrics
  (singularity count, % regular valence, skinny-quad score).

### Review gate 2
Inspect: overlay screenshots, regression numbers, corpus before/after. Decide if
2b (radius) is needed.

### Results — iters sweep on the pair/fold bench set (lambda 0.5)

Sequential same-session sweep over `curvature_smooth_iters` ∈ {0,2,4,8} on the
three fold benches (`tests/scripts/remesh_pairs_{sphere,dabsphere}.txt` +
AnimeGirl2 via `remesh_cli --triage 1 --target-quads 30000`). Diagnostics from
`[remesh_quantize:pairs]` / manifest `run.quantize`.

**Scan-like input (anime girl, 148,953 classes) — monotone win on the pair axis:**

| iters | singularities | spurious pairs | folds near pairs | final param folds | extract irr / regular | holes | quantize total |
|---|---|---|---|---|---|---|---|
| 0 | 1480 | 1151 | 27% | 1226 | 809 / 0.9677 | 276 | 642 s |
| 2 | 1270 | 1049 | 19% | 1152 | 664 / 0.9729 | 252 | 474 s |
| 4 | 1179 | 991 | 21% | 1103 | 626 / 0.9746 | 223 | 590 s |
| 8 | 1067 | 942 | 18% | 1060 | 570 / 0.9773 | 193 | 484 s |

No saturation by 8; quantize also gets *cheaper* (fewer singularities → fewer
rounding/tier-1b iterations). Raw `seamless_folds` is noisy run-to-run
(29.7k–34.7k, non-monotone) — judge by final param folds + extract metrics.

**Feature-driven input (dab-sphere, 108,300 faces) — harmful:** singularities
pinned at the topological minimum 8 / 0 spurious pairs at every setting, but
seamless folds 21,792 → ~48,500 (any iters ≥ 2), final param folds 49 → 260–755,
quantize total +35–80%. The dab curvature is *signal*; smoothing washes the
sizing/alignment cues into the flat regions.

**Analytic sphere (73,344 faces) — no-op:** 2 singularities / 0 pairs / ~75
seamless folds either way; its 27k+ param folds and `feasible=0` are pure
quantize-side (tier-1b grind ≈ 95% of its 12–15 min) and out of Tier-2 reach.

**Conclusion:** Tier 2 attacks the *spurious-pair* axis (noisy scan-like
curvature) and does nothing for — or actively harms — the *fold* axis on clean
or feature-driven inputs. Keep default `0`; enable (≈4–8) via the scan/messy
presets, not unconditionally.

---

## Tier 3 — Automatic curvature-density + gradation limiting

**Goal:** generate an adaptive size field automatically (small quads at
face/fingers/folds/silhouettes, large on torso/limbs) and bound its gradient so
size transitions don't shear quads. Subsumes the original "gradation only" tier —
auto-generation is what makes adaptive density useful without a painted map.

### 3a. Curvature-aware density generation
- **Use a dimensionless scale law**, not raw `k` — `density ∝ k` is unit-bearing
  and behaves differently across asset scales / target lengths. Drive it off a
  **dimensionless** product, e.g. `s = k · target_edge_length` (curvature
  relative to the quad size we're aiming for) — or `k · bbox_diag` for an
  asset-relative form. Map `s` through a clamp `[density_min, density_max]` so
  high-curvature → smaller quads, with a **minimum-feature-size floor** so scan
  noise / tiny folds don't drive density to the ceiling. `k` is the
  **Tier-2-smoothed** curvature (raw `k` would re-inject the noise we filtered).
- Write to `.remesh.v.density`. **`auto_density` must imply `use_density`** — the
  consumer is gated by `params.use_density` when `QuadRemesh` threads into
  `QuantizeParams` (`remesh.cc:212`), so `auto_density=true, use_density=false`
  would silently generate a field that is then ignored. Thread it as
  `qp.use_density = params.use_density || params.auto_density` (and document that
  auto-density implies density consumption). The "host painted a map" branch is
  only meaningful **after Tier 1b** lands the attr-copy; until then auto-density
  is the sole density source (state this in the docs).

### 3b. Gradation limiting (bounded per-edge growth ratio)
**Algorithm — hop/ratio-based, NOT geometric distance (implemented,
`field/density.cc`).** Convert density→goal length `h = L/sqrt(density)`, then
cap `h` to grow by at most `(1 + gradation)` across **any single edge**:
worklist min-propagation expanding outward from fine regions with geometrically
relaxed goals (`h[n] ← min(h[n], h[v]·(1+gradation))`) to a fixed point.
Distance-based (Alauzet `h[n] + beta·dist(v,n)`) is the wrong currency here —
the allowed step scales with the *current* edge's world length, so on a coarse
input one long edge legally spans a huge size step and a per-edge cliff
survives, exactly where the BK band then overlaps pathologically. Only ever
refines (raises density); `density_gradation_iters` caps total work (pops per
vertex), not convergence.

**Where (two call sites):**
- **Main pipeline** — prepass before `buildSeamlessSystem` (before
  `computeQuantization` in `remesh.cc`), writing limited values back so both
  consumers see the filtered field: the M4 seamless read
  (`seamless_internal.cc:166`, folds `1/sqrt(density)` into the FEM
  target-gradient — **this sets local size**) and the M5 quantize re-read
  (`quantize_ilp.cc:513`, secondary).
- **Tier 9 pre-pass** — before every BK dab (`preremesh.cc`), between
  `generateAutoDensity` and the size-scale write, so the split/collapse band
  never steps sharply across an edge. **Non-optional there** — defaults on
  (`PreRemeshParams::gradation = 0.5`); `0` disables for A/B only.

**Gate-3 decision (resolved):** the main pipeline's `density_gradation` now
defaults `0.5` (on, matching the pre-pass) — gradation is a correctness filter,
not a style knob. Inert unless a density field is in play (`auto_density` or
painted + `use_density`), so default runs are unaffected.

### New params
| field | default | meaning |
|-------|---------|---------|
| `auto_density` | `false` | generate `.remesh.v.density` from curvature |
| `density_min` / `density_max` | `0.25f` / `4.0f` | clamp on generated density (size range) |
| `density_gradation` | `0.5f` | max per-edge-hop goal-length growth ratio − 1; `0` = off. Typical `0.3–1.0` (the pre-pass has its own knob, same default) |
| `density_gradation_iters` | `10` | work cap (pops per vertex); termination is natural |

### Verification
- **gtest:** steep density step (1.0 vs 8.0 across a plane/cylinder) — assert
  worst adjacent-quad area/edge ratio (Tier 0 metric) **drops** with gradation on,
  quad count stays sane. Auto-density on a sphere-with-a-bump: assert smaller
  quads cluster on the bump.
- **Debug-app visual:** wireframe at gradation `0` vs `0.5` over a step; jump
  should smear over several rings.

### Review gate 3
Inspect: adjacent-quad ratios, auto-density quad-size map screenshot, corpus
metrics. Confirm coarse/fine regions preserved, only transitions widen.

---

## Tier 4 — Smoothness/alignment master knob

**Goal:** trade "globally smoothest, fewest singularities" against "tracks
curvature, inherits noise". **Where:** `cross_field.cc:101`, `wsmooth` hardcoded
to `1.0`. The alignment side is already exposed via `curvature_weight`
(`constraints.cc:219`).

### Changes
- Add `field_smoothness` → `CrossFieldParams`; use for `wsmooth`
  (`cross_field.cc:122`). Default `1.0f` reproduces current output.
- **`curvature_weight` is the other half of the knob and is not yet a
  `RemeshParams` field.** It exists only in `CrossFieldParams` (`cross_field.h:32`,
  used at `constraints.cc:220`) and `QuadRemesh` never sets it (`remesh.cc:196`
  threads only `use_curvature`/`use_sharp_features`/`sharp_angle`/`seed`). To
  "surface both together" it must become a real `RemeshParams` field with the full
  binding/CLI/UI/TS surface — **or** the plan declares it stays internal/debug-only.
  Since the tradeoff *is* the point of this tier, add it.
- Document: effective regularization ≈ `field_smoothness / curvature_weight`;
  surface both together in the UI.

### New params
| field | default | meaning |
|-------|---------|---------|
| `field_smoothness` | `1.0f` | per-edge smoothness weight; higher = smoother field, fewer singularities |
| `curvature_weight` | `1.0f` | soft curvature-alignment scale (promote from `CrossFieldParams`; thread through `remesh.cc` + bindings) |

### Verification
- **gtest (hard assert):** Gauss–Bonnet `index_sum == 4χ` at **every** setting.
- **gtest (diagnostic, not asserted):** record `num_singularities` across a
  smoothness sweep on a noised sphere; expected trend non-increasing but **not** a
  reliable invariant for a constrained global field — a non-monotone point is an
  inspect flag, not a failure. Fixed input+seed regression baseline is fair.
- **Debug-app visual:** screenshots at smoothness `0.5 / 1 / 4`.

### Review gate 4
Inspect: invariant check, singularity-vs-smoothness curve, screenshots. Confirm
high smoothness doesn't collapse the field (empty extract).

### Results (implemented; awaiting gate review)
- `test_remesh_field.cc:testSmoothnessSweep` — Gauss–Bonnet `index_sum == 4χ`
  holds at all 10 settings on a noised sphere (hard assert). Diagnostic curve is
  cleanly monotone: singularities 86→78→56→46→38→28 for smoothness
  0.25→8 (weight 1), and 20→38→56→86 for weight 0→4 (smoothness 1) —
  confirming effective regularization ≈ `field_smoothness / curvature_weight`.
- `tests/scripts/remesh_tier4_smoothness.txt` (debug_app): clean torus extracts
  the identical valid 1920-quad mesh at smoothness 0.5 and 8 (no collapse, no
  empty extract). On a dab-noised sphere the extremes both degrade the
  *extract* even though the field stays valid: smoothness 0.5 → 12 boundary
  edges / 5 inverted (field inherits noise), 1.0 → 6 / 4 (best), 4.0 → 84 / 45
  with visible radial slivers (field stops tracking local curvature).
  Screenshots: `tests/remesher-results/tier4/`.
- **Joint grid with Tier 2** (`field_smoothness` {1,2} ×
  `curvature_smooth_iters` {0,4}, lambda 0.5, same bench set as Tier 2's
  Results). Anime girl (148,953 classes, triage ON):

  | fs | iters | singularities | spurious pairs | final param folds | extract irr / regular | quantize total |
  |---|---|---|---|---|---|---|
  | 1 | 0 | 1480 | 1151 | 1226 | 809 / 0.9677 | 642 s |
  | 1 | 4 | 1179 | 991 | 1103 | 626 / 0.9746 | 590 s |
  | 2 | 0 | 1256 | 1026 | 1161 | 658 / 0.9737 | 497 s |
  | 2 | 4 | 1084 | 975 | 1087 | 641 / 0.9738 | 537 s |

  The knobs compose (1480 → 1084 singularities) but with diminishing returns on
  the pair axis — even max dosing (iters 8, Tier 2 Results) leaves ~82% of the
  spurious pairs. fs=2 alone is a straight perf win on the dab-sphere (quantize
  137 s vs 235 s, final folds 49 → 53, quality flat) — the smoother field gives
  ARAP/rounding easier systems. Outlier worth knowing: on the analytic sphere,
  fs=2 + iters=4 broke the degenerate 2-pole field configuration (16
  distributed singularities, `index_sum` preserved) and final param folds
  collapsed 29,865 → 194 with quantize 9× faster — singularity *placement*,
  not count, drove that mesh's fold pathology.

---

## Tier 5 — Singularity pair cancellation

**Goal:** remove residual spurious singularity clutter that curvature smoothing
(Tier 2) and the smoothness knob (Tier 4) left behind.

**Gated — and first decide whether it's needed at all.** The shipped code
*deliberately defers* this with a documented rationale (`singularity_adjust.h:13`,
`XXX:`): after M3's fixed-period Poisson re-solve the field already sits at the
integer-optimum **for its current singularities**, and the **no-spiral guarantee
comes from M5's quantization regardless**. So the marginal value is only
*aesthetic* singularity reduction, not correctness. Build this **only if** Tier
2/4 corpus metrics still show clutter that hurts the valence/skinny-quad numbers —
otherwise skip it. The fundamental fix is reducing the source (noise, Tier 2).
Tier 0d's convergence/band-pressure trace helps make that call: it separates
field singularity clutter from BK split/collapse churn, which look alike in the
final valence numbers.

### Approach (this is genuinely hard — not a `pole_index` edit)
M3 holds **period jumps fixed**; cancelling a pair is a **cohomology edit**:
- identify nearby opposite-index pairs (+k/−k adjacent in the dual graph);
- apply a **coordinated multi-edge period-jump change** along a path between them
  (the discrete move that annihilates the pair), then **re-run the phase solve**
  (M3) for the new period configuration;
- **roll back** if the validation metrics (parametrization folds, quantization
  feasibility, skinny-quad score) worsen — net-improvement-only;
- honor user pins; maintain `index_sum == 4χ` throughout.
Extend `field/singularity_adjust.{h,cc}` (this is the deferred work its header
flags, not a bolt-on).

### New params
| field | default | meaning |
|-------|---------|---------|
| `singularity_cancel` | `true` | enable opposite-pair (±1) cancellation (default-on since gate 5; both the C++ default and the app-op `BoolProperty` are `true`) |
| `singularity_cancel_max_sep` | `1.5` | pair-separation gate, in units of the target quad edge length (geodesic, not hops — "sub-resolution" = what the output lattice can't represent as distinct irregular verts) |

### Verification
- **gtest:** synthetic field with a planted adjacent +1/−1 pair on a flat region;
  assert the pair is cancelled and `index_sum` unchanged. A genuine pole (sphere's
  required singularities) must **not** be cancelled.
- **Corpus:** singularity count + fold count drop without raising skinny-quad
  score.

### Review gate 5
Inspect: planted-pair test, corpus deltas, confirm required poles survive.

### Gate decision (items 1–4 bench sweeps, 2026-06-11)

**Build it — for the scan/messy input class.** The source-reduction knobs
saturate well above zero: at max dosing (Tier 2 iters 8 / Tier 4 fs 2) the
anime-girl bench still carries 942–975 spurious pairs (only −18% from the 1151
baseline), ~19% of seamless folds co-locate with them, and extract regularity
plateaus at ~0.977. That residual clutter is exactly what this tier exists for.
Two qualifiers from the same sweeps:

- **It is not a fold fix for clean inputs.** The fold-heavy benches with zero
  spurious pairs (192-sphere, dab-sphere) are untouchable from the field side;
  their cost is quantize-side — the 192-sphere spends ~95% of its 11–15 min in
  tier-1b (~17k batched 4-probe triangular solves). That axis belongs to the
  tier-1b probe budget / solver work, not to this tier.
- **Configuration edits have fold leverage too.** The sphere fs=2/iters=4
  outlier (param folds 29,865 → 194 once the degenerate 2-pole configuration
  broke into 16 distributed singularities) shows singularity *placement* can
  dominate fold count — supporting the rollback-guarded cancellation/relocation
  design over a pure-aesthetics framing, and suggesting the validation metric
  set (folds, feasibility) will sometimes *reward* relocation on near-umbilic
  regions.

### Quantize-cost correlation (Tier-2/4 sweep manifests, 2026-06-11)

Cross-referencing the six agirl sweep manifests (fs ∈ {1,2} × cs ∈ {0,2,4,8};
pairs 1151 → 942 across the dose range) against their quantize profiles:

| config | pairs | sing | rounds | round_s | t1b_s | total_s |
|--------|------:|-----:|-------:|--------:|------:|--------:|
| fs1 cs0 | 1151 | 1480 | 1634 | 203.6 | 395.7 | 641.8 |
| fs1 cs2 | 1049 | 1270 | 1314 | 158.0 | 274.7 | 473.8 |
| fs1 cs4 |  991 | 1179 | 1261 | 180.3 | 368.4 | 589.7 |
| fs1 cs8 |  942 | 1067 | 1213 | 176.6 | 263.7 | 484.0 |
| fs2 cs0 | 1026 | 1256 | 1339 | 183.0 | 274.2 | 497.2 |
| fs2 cs4 |  975 | 1084 | 1168 | 152.0 | 345.9 | 537.4 |

- **Rounding rounds track singularity count ≈ linearly** (sing −28% → rounds
  −26% across the cs sweep) — the one real perf coupling: fewer poles means
  fewer integer classes to round.
- **Tier-1b is fold-regime-driven, not pair-count-driven.** The 192-sphere has
  zero spurious pairs yet spends ~926 s in tier-1b; on agirl, t1b swings
  264–396 s with no monotone relation to pairs (probes follow folds).
- **Wall-clock noise swamps config deltas**: an identical-config rerun
  (triage_on, byte-identical counters to fs1 cs0) landed at 772.8 s vs 641.8 s
  (+20%) — only counter deltas are attributable, never cross-session totals.

**Verdict:** noise pairs are primarily a *quality* problem (clutter, ~19%
co-located seamless folds, valence); their perf contribution is modest and
flows through the rounding-round count. The Tier-5 bench gate should weigh
quality metrics first, `rounds`/`round_s` second, and total wall-clock not at
all (same-session A/B only).

### Decimate-for-solve A/B (research/singularity-merging.md rec 2, 2026-06-11)

Same bench (agirl 148k tris, mean input edge ≈0.0037, 30k quads → L_quad =
0.00583, triage on), adding `--solve` decimation before the field solve:

| config | solve mesh | pairs | param_folds | rounds | total_s |
|--------|-----------|------:|------------:|-------:|--------:|
| full-res (fs1 cs0) | 148k tris | 1151 | 1226 | 1634 | 641.8 |
| `--solve 0.00583` (1.0×L_quad) | ~60k tris | 869 | 572 | 1056 | 345 |
| `--solve 0.00408` (0.7×L_quad) | ~120k tris | — | — | — | **DNF, killed at 3 h** |

- **At quad scale the win is real and broad**: pairs −25%, param folds −53%,
  rounds −35% — counter deltas, hence attributable. Solving the field on a
  coarser, BK-relaxed triangulation reduces the *source* noise, not just cost.
- **0.7×L_quad is lose-lose.** 0.00408 vs input mean 0.0037 is barely 1.1×
  coarsening — a near-full-res mesh whose triangle quality decimation made
  *worse*. The run spent ~2.9 h of CPU inside quantize (vs 642 s full-res,
  ≥17×, far beyond the ±20% noise band) and never reached extraction. The cost
  curve is **not monotone in solve resolution**: triangulation quality
  dominates triangle count in the fold/tier-1b regime.
- **Practical rule:** decimate-for-solve pays only at/near the quad scale.
  This reinforces the Tier-9 decision (`--solve` removed): the 1.0×L win comes
  from solving on fewer *and cleaner* triangles, which the field-aligned
  pre-remesh delivers strictly better than quality-blind decimation.
- The 3 h black box is also why quantize now streams `[quantize_progress]`
  running counters (the killed leg yielded only a wall-clock).

### Implementation (landed 2026-06-11)

`cancelSingularityPairs` (`field/singularity_adjust.{h,cc}`) — bounded rounds
of find → flip → re-solve, exactly the cohomology edit sketched above:

- **Pairing:** one bounded Dijkstra per +1 pole over interior, manifold,
  orientation-consistent regions (pinned verts excluded), pruned at
  `max_sep · target_edge_length` geodesic distance; greedy vertex-disjoint
  shortest-path selection so each edge flips at most once per round.
- **The flip:** a ±1 implied-period delta chained along the path with
  `periodSignAt` (the sign with which a period flip on edge e moves vertex v's
  pole walk), so every intermediate vertex's index transfer nets zero and the
  endpoints absorb ∓1. Deltas are added to the implied periods inside the
  fixed-period Poisson re-solve (`solvePhaseField`'s `edge_delta` hook).
- **Acceptance:** a round is kept only if the solver succeeded, `index_sum` is
  conserved, and the pole count strictly drops; otherwise θ/period/pole are
  restored from snapshots and the pass stops (net-improvement-only, as
  specified). Deterministic by construction (tie-broken extract-min, sorted
  candidates, no RNG).
- **Wiring:** `RemeshParams.singularity_cancel{,_max_sep}` → hook in
  `remesh.cc` after the singularity stage (gated on the derived `L_quad`
  scale); `cancel_*` counters in `RemeshRunReport` + CLI manifest; bindings;
  debug-app `remesh_adjust_singularities cancel=1` verb extension. All nine
  per-param surfaces are wired (incl. `remesh_app.cc`/`remesh_ui.cc` and the
  host `litemesh{,_ops}.ts`; the C++ default `false` is authoritative).
- **gtests** (`test_remesh_singularity`, all green): planted analytic ±1 pair
  on a flat grid — gate below the separation is inert, gate above annihilates
  it in 1 round / 0 reverts and the curl drops to the flat ground state
  (~7e-6); a sphere's 8 required +1 poles are untouched (no opposite-sign
  targets); pinning one endpoint blocks the pair; cancellation is
  byte-deterministic.

### Bench A/B + gate 5 verdict (2026-06-11)

The first `--singularity-cancel 1` agirl leg exploded (quantize sides 71,784
→ 211,392, killed mid-grind) and exposed a latent **multi-component cut-graph
bug**: `buildCutGraph` grew a *single-rooted* dual spanning tree, leaving
every component but the root's fully cut. The cancel pass legitimately moved
the first pole into a small accessory component, rooting the lone tree there
and leaving the main body (~139.6k faces) fully cut — the 139,608-side delta
is exactly F_main − 1. Even the off leg had silently carried its 47 accessory
components fully cut. Fixed in `b7a4bf2` (dual spanning **forest**, pole
components root at their first pole's face; regression test in
`test_remesh_param`), and **both legs re-ran on the fixed binary**
(same-session, legs concurrent):

| metric | off (buggy, ref) | off (fixed) | on (fixed) | on vs off |
|--------|---:|---:|---:|---:|
| cut_sides | 71,784 | 69,962 | 69,962 | = |
| singularities | 1,480 | 1,480 | **732** | −51% |
| spurious_pairs | 1,151 | 1,151 | **490** | −57% |
| rounding rounds | 1,634 | 1,580 | **752** | −52% |
| tier1b_probes | 19,072 | 14,244 | 8,080 | −43% |
| seamless folds near pairs | 8,468 | 8,399 | **1,377** | −84% |
| seamless_folds | 31,283 | 31,214 | 34,666 | +11% |
| param_folds (validation) | 1,226 | 1,102 | **739** | −33% |
| irregular interior verts | 809 | 656 | **538** | −18% |
| regular_interior_frac | 0.9677 | 0.9735 | **0.9779** | +0.44 pt |
| inverted_faces | 28 | 11 | 14 | +3 |
| output quads | 26,415 | 26,307 | 25,542 | −3% |
| quantize total_s | 991 | 588 | 416 | −29% |

- **Cancel pass: 373 of 384 attempted pairs cancelled, 0 reverted rounds**
  (sing 1,480 → 732 ✓ = 1,480 − 2·373 − 2 net-zero strays).
- `cut_sides`/`classes` are byte-equal off-vs-on as expected — the cotree
  size is topology-determined (tree = F − #components); rooting only picks
  *which* edges, which is what the buggy single root got wrong.
- **Gate 5: PASS.** Cancelled pairs translate ~1:1 into the predicted wins:
  rounds −52% tracks sing −51% (confirming the linear coupling measured
  above), realized param folds −33%, near-pair seamless folds −84%, irregular
  verts −18%. Raw `seamless_folds` ticks up +11% but the realized fold count
  after tier-1b/stiffening drops by a third — the pass removes precisely the
  *stubborn* (pair-co-located) folds. `inverted_faces` 11 → 14 is noise-scale.
- The cut-graph fix alone (off-buggy → off-fixed) was worth −41% quantize
  wall, param_folds −10%, inverted 28 → 11, and output components 48 → 16:
  the fully-cut accessories had been quantizing into garbage shards. Of 61
  input components only 16 (off) / 12 (on) survive to extraction — tiny
  components below quad scale now honestly produce zero quads (a Tier-6
  component-policy item, not a quantize bug).
- Per the gate verdict, `singularity_cancel` is now **default on** (the C++
  `RemeshParams` default plus the one diverging surface, the app op's
  `singularityCancel` `BoolProperty` in `litemesh_ops.ts`).

---

## Tier 6 — Boundary / thin-part / component policy

**Goal:** define how characters' awkward topology is handled — the current levers
are just `cap_odd_holes` and `buildTriCopy`'s single-outer-boundary assumption
(`remesh.cc:48`). **Scope carefully; some of this is research-grade — document
limits, don't over-promise.**

> **Two different "hole" problems — keep them separate.** `cap_odd_holes` and the
> cap path at `quad_extract.cc:530` are **output residual cleanup**: they close
> spurious boundary loops left in the *extracted quad mesh*. **Input hole policy**
> (below) is a **pre-solve** concern on the *triangle input*, a separate
> triangulation/patching problem. Do not conflate them or describe input filling
> as "generalizing `cap_odd_holes`".

### Decisions to implement
- **Per-component independent remesh** (rides on Tier 1's component split):
  optionally remesh disconnected accessories separately so one component's field
  doesn't perturb another.
- **Input hole policy (pre-solve):** classify input boundary loops by rim length;
  **fill tiny holes** (nostrils, small punctures) by triangulating them *before*
  the field solve so they don't seed spurious boundary constraints, while
  **preserving large boundaries** (mouth, sleeve cuffs, eyelids) as open. This is
  new pre-solve patching, distinct from the output cap path.
- **Better output hole capping** (the extraction cap, `quad_extract.cc:530`):
  today every cap is a center fan — acceptable for small even rims (center
  valence n/2) but a single high-valence pole on large ones — and an *odd* rim
  gets either one triangle (opt-in `cap_odd_holes`) or stays open. Parity
  (4F = 2E_int + E_bnd) says a disk with an odd boundary can never be all-quad,
  so a real odd-hole cap must touch the surrounding mesh: split one rim edge
  (rim becomes even; the adjacent quad becomes a pentagon) and propagate the
  quad-strip split a few faces, terminating in a valence 3-5 pair, then cap the
  even rim normally. Large even rims (n ≳ 12) should bridge / recursively split
  into smaller rims (or grid-fill when the rim decomposes into 4 sides) instead
  of fanning. Once the odd cap is all-quad, revisit `cap_odd_holes`'s default —
  the all-quad-contract reason for off disappears. Keep this distinct from the
  input hole policy above; the two must not double-close a legitimately-open rim.
  - **Measured (6.0 diagnostics, fox @ 15k quads, cap_odd=1):** 198 output rims →
    103 capped (52 odd = the 52 output tris), 95 open: **48 size** (all n < 4 —
    tiny 2–3-edge slits; the >2048 side is impossible at 1598 boundary edges),
    **34 pinched** (rim revisits a vertex), **13 border-heuristic** (input has 4
    real holes → ~9 spurious), 0 odd-skipped, 0 untraced. So the dominant fixes
    are: cap/weld tiny rims (n=2 zip, n=3 triangle or split-to-4 quad), split
    pinched rims at their pinch vertices into simple sub-loops, and per-loop
    input-rim correspondence to replace the global `inputClosed` heuristic.
  - **DONE (6.1):** all three landed in `quad_extract.cc` — n=3 triangle cap
    (no digons exist; n<3 stays in the size bucket), pinch-split worklist
    (`holes_pinched_split`; the pinch vert becomes an edge-manifold bowtie), and
    a per-loop real-border classifier: mean rim-vert distance to the input
    boundary polyline ≤ 1.0 × mean rim edge (`kBorderTol`; real borders measure
    ~0.7 cells — the extracted rim is the outermost full lattice line — vs ≥ 1.6
    for spurious rims; fox's 4 "holes" are single-edge sub-grid cracks, so none
    are real). **Fox @ 15k, cap_odd=1: boundary loops 94 → 0** (watertight;
    1598 → 0 boundary edges), 253 rims capped (158 odd → tris, was 52), 0 open.
    Plane-with-hole unchanged (2 rims classified border, kept open); gtest
    `testPlaneHoleBorders` pins it. Known limitation → 6.3: an input hole
    within ~1 cell of an output rim would classify as border; sub-grid input
    cracks (fox) are now capped in the *output* — the input-side fill
    (`input_hole_fill_max_frac`) remains the principled fix.
  - **DONE (6.2):** the all-quad odd cap landed as **odd-rim pairing**, not the
    single rim-edge split sketched above — that termination is impossible: the
    same parity argument (4F = 2E_int + B) applied to the split's patch shows a
    lone odd rim always costs ≥ 1 triangle, while Σ rim lengths even per
    component means odd rims come in *pairs*. So `quad_extract.cc` traces the
    dual quad strip from each odd rim edge (in one edge, out the opposite);
    when it exits at another unpaired odd non-border rim, every strip quad is
    split in two lengthwise (a "ladder" split off the entry/exit edge
    midpoints) — both rims gain one vert, turn even, and cap all-quad
    (`odd_rims_paired` in `ExtractStats` + manifest). Unpairable odd rims fall
    back to a fan with one cap triangle (provably minimal). Even-rim emission
    hardened alongside: chord-split candidates rejected when a sub-piece folds
    (Newell vs parent) or is a sliver (isoperimetric `4πA/P² < 0.2` — its
    centroid fan degenerates to a bowtie), and the 4-rim single-quad close is
    gated on agreeing with the adjacent faces' Newell sum (`flatQuadOk`); fixes
    sphere/reproject `inv 1 → 0`, simple.obj `inv 5 → 2`. **Fox @ 15k,
    cap_odd=1: output tris 158 → 48** (110/158 odd rims paired; the 48 are
    unpaired-rim fallbacks), still watertight (0 holes), inverted 70 → 93 vs
    cap_odd=0 across all 253 caps (fan pleats on ragged rims; smoothed by
    reprojection). gtest `testOddCapExtract` pins the capped-cylinder pair:
    watertight, all-quad, euler 2, `odd_rims_paired == 2`.
  - **DONE (6.3):** `input_hole_fill_max_frac` landed as `fillInputHoles`
    (`triage.cc`), run on the work copy right after triage and before any field
    math: trace input boundary loops opposite the face winding (per-vert
    boundary-edge incidence; != 2 = pinch → preserve), then triangulate every
    simple loop whose rim length < frac × total boundary length — single
    triangle at n=3, centroid fan (attrs row-copied from a rim vert) above.
    Counts land in `TriageReport` (`input_holes_{filled,kept}` +
    `input_hole_fill_faces`), the manifest triage block, and the STATS line;
    CLI `--hole-fill <f>`. No double-close by construction: filled loops
    vanish from the input boundary before the solve, and preserved loops reach
    the extract cap path as border-classified rims (never capped). gtest
    `testHoleFill` pins the plan's plane fixture (tiny 4-rim filled by a
    4-tri fan, large 12-rim + 32-border preserved, manifold + consistent
    winding, idempotent re-run); CLI e2e on the same fixture: `holes 3 → 2`,
    euler −1 → 0, all-quad, inverted 0. **Fox finding:** its 4 input "holes"
    are *not* loops at all — each is an isolated single boundary edge
    (dead-end slit off the 13 non-manifold edges, both endpoints with exactly
    one incident boundary edge), so no input-side fill can close them;
    `input_holes_kept=4` and the 6.1 *output* border cap remains the fox fix.
- **Boundary preservation:** keep large open rims aligned (already pinned as
  boundaries in `feature_tag`); verify they survive reprojection.
- **Boundary sliding in the pre-pass:** Tier 9c *pins* boundary verts outright in
  collapse + smooth, so a dense input rim stays dense and rim triangle quality
  can't improve. Upgrade to **constrain-to-polyline**: collapses along a boundary
  loop and tangential smoothing *along* it (1D BK on the rim) — the
  layout-embedding treatment, where boundary nodes slide along the boundary
  ([research/layout-embedding-optimization.md](../research/layout-embedding-optimization.md)).
- **Thin double-sided sheets:** the hardest case (cross-field/param degenerate on
  near-zero-thickness shells). For v1, **detect and document as known-poor**;
  optionally route to a fallback or flag in the report rather than silently
  emitting garbage.

### New params
| field | default | meaning |
|-------|---------|---------|
| `input_hole_fill_max_frac` | `0.0f` | **pre-solve**: triangulate input holes whose rim length < this fraction of total boundary; preserve larger (0 = today) |
| `per_component` | `false` | remesh disconnected components independently |

### Verification
- **gtest:** a plane with one tiny hole + one large hole — assert tiny capped,
  large preserved. Two-component mesh — assert independent remesh when enabled.
  Odd-rim cap — assert the patch is all-quad and watertight (3-5 pair
  termination, no triangle), and an even large rim caps without a high-valence
  pole.
- **Corpus:** holed-face and multi-accessory assets — hole-count and
  boundary-preservation-error metrics behave as intended.

### Review gate 6
Inspect: hole-policy test, the all-quad odd-rim cap (+ `cap_odd_holes` default
decision), component handling, corpus boundary metrics, and the thin-sheet
detection/report.

---

## Tier 7 (LAST FILTER TIER) — Feature-graph hysteresis + spur pruning

**Goal:** clean noisy *hard* feature constraints so they don't force spurious
singularities. Last filter tier — payoff concentrates on CAD/mechanical, not a
primary target; organic/scanned lean on boundaries, detected separately and
unaffected. **Where:** `field/feature_tag.cc`; today a single per-edge dihedral
threshold (`feature_tag.cc:47`), edges tagged independently. Boundary tagging
unchanged.

### 7a. Hysteresis (relative band — avoids "unset" ambiguity)
A reflected float can't carry "unset" intent, so a `high`/`low` pair both
defaulting to `sharp_angle` is ambiguous. Use a relative band on the single
authoritative knob:
- `sharp_angle` stays **the** strong threshold (unchanged).
- `feature_hysteresis` (rad, default `0`, **clamped to `[0, sharp_angle]`**): weak
  threshold = `max(0, sharp_angle − feature_hysteresis)`. Without the clamp a large
  hysteresis drives the weak threshold negative and tags nearly every edge
  weak-sharp.
- Strong-sharp if dihedral ≥ `sharp_angle`; a weak edge is kept only if adjacent
  (shared vertex) to a strong edge. Tag strong set, flood weak neighbors.
- `feature_hysteresis = 0` ⇒ exactly today's tagging; `--sharp-angle` keeps its
  current behavior.

### 7b. Spur pruning
Build the sharp-edge graph; drop chains/components shorter than
`feature_min_chain` that don't terminate at a true corner/junction (degree ≠ 2).
`feature_min_chain = 0` = off. (Gap bridging: skip in v1 unless 7a/7b review shows
leakage.)

### New params
| field | default | meaning |
|-------|---------|---------|
| `feature_hysteresis` | `0.0f` | rad below `sharp_angle` for the weak threshold (0 = today) |
| `feature_min_chain` | `0` | drop sharp chains shorter than this many edges (0 = off) |

### Verification
- **gtest** `tests/test_remesh_feature_filter.cc`: box with a real sharp edge +
  injected single-edge noise sharps; assert real edge retained, noise dropped,
  fewer singularities than unfiltered; clean cube unchanged.
- **Corpus:** singularity count + skinny-quad score on a hard-ish asset.

### Review gate 7
Inspect: retained-vs-dropped correctness, singularity delta, cube no-op.

---

## Tier 8 (CAPSTONE) — Retry/robustness policy + presets

**Goal:** turn the knobs into an automatic robustness policy and a small set of
named presets. **Last**, because it orchestrates every prior knob and depends on
Tier 0 metrics to make decisions. Each retry is a full expensive solve — cap
attempts hard.

### 8a. Retry policy
A bounded loop around `QuadRemesh` driven by Tier 0 metrics:
- too many parametrization folds → raise `field_smoothness`, retry;
- singularity count too high → raise `curvature_smooth_iters` (and/or enable
  `singularity_cancel`), retry;
- adjacent size ratio too steep → raise `density_gradation`, retry;
- field/folds still noisy after the above → enable/strengthen the Tier 9
  pre-remesh (`pre_remesh`, more `pre_remesh_iters`), retry from the **original**
  input;
- too many residual odd holes / tri caps → coarser `target_edge_length`, retry.
Hard cap on attempts. **Record the full attempt trail, not just the winner** —
the `RemeshRunReport` (Tier 0a) carries a per-attempt array: the params used, the
failure reason, validation stats, fold count, singularity count, and **whether the
attempt started from the original input or a mutated intermediate**. Without the
trail, auto-retry can improve a result while making its failures unreproducible.
The manifest emits the trail + which fallback won; the host gets the compact
summary. Default attempts = 1 (today's behavior) unless `auto_retry` is on.

### 8b. Presets
Named bundles setting the whole knob vector: **Organic Clean**, **Organic Noisy**,
**Messy Generated Character**, **Scan**, **Hard Surface**. A preset sets curvature
smoothing, pre-remesh target, field smoothness, auto-density + gradation, hole/cap
policy, and retry policy. Expose as a single dropdown in the UI / a `--preset`
CLI flag that pre-fills the params (still individually overridable).

### New params
| field | default | meaning |
|-------|---------|---------|
| `auto_retry` | `false` | enable the metric-driven retry loop |
| `max_attempts` | `3` | hard cap when `auto_retry` is on |
| (preset) | — | CLI `--preset <name>` / UI dropdown; pre-fills the knob vector |

### Verification
- **gtest:** a deliberately fold-prone input — assert `auto_retry` reduces folds
  vs a single attempt and reports the winning fallback; assert `max_attempts` is
  respected.
- **Corpus:** run every preset over the corpus; the per-input "best preset"
  metrics table is the deliverable.

### Review gate 8 (final)
Inspect: retry behavior + manifest fallback log, preset metrics table over the
corpus. Then the cross-tier closeout.

---

## Tier 9 — Field-aligned input pre-remesh

**Goal:** clean the *input triangulation itself* before any of the real field
math — produce isotropic, feature-following triangle flow so the cross field (the
noisiest stage) starts from low-noise geometry. This is the **only** tier that
remeshes the input; every other tier keeps the triangulation fixed and filters a
*derived* quantity. It attacks the fold-fraction at its source: the
"stuck-in-quantize" cliff on dense organic assets (e.g.
`an-elegant-fox-character`) is ~30% folded faces driven by a noisy field driven
by noisy input — clean the input and the field, folds, and quantize cost all fall
together.

> **Execution order vs. ladder order.** In the *pipeline* this runs **early** — on
> `work` after Tier 1 triage, before Tier 2 curvature / `computeCrossField`. It is
> *numbered* last only to avoid renumbering the in-flight Tiers 0–8 and so it can
> lean on Tier 2's smoothed curvature for a better rough field and Tier 0's metrics
> to prove it. Implement after the tiers it depends on; place it early at runtime.

> **Decided 2026-06: the separate `--solve` decimation stage is removed — the
> pre-remesher is the only input-geometry stage.** `decimateForSolve`
> (`remesh.cc:224`) was already the BK quartet plus an isotropic relaxation tail,
> and the pre-pass is a strict superset (field-aligned smooth, feature pinning,
> adaptive sizing, fold-safe collapse), so deleting it loses nothing. This is the
> paper-endorsed shape — the layout-embedding evaluation isotropically
> retriangulates every input and never decimates; near-equilateral triangles are
> what every downstream cotan solve wants
> ([research/layout-embedding-optimization.md](../research/layout-embedding-optimization.md)).
> Removal follow-ons: `solve_edge_length` and its CLI/UI/TS surface go away —
> `pre_remesh_target` becomes the one explicit deep-coarsening route, exempt from
> the auto-target **20% edge budget** (auto-resolved targets are clamped to
> `L ≤ mean_in/√0.8` so the pre-pass never coarsens away more than 20% of the
> input's edges, with the density floor capped at the same budget). The budget
> clamp's `solve_edge_length <= 0` exemption clause dies with the param, and the
> dense-input coarsen bootstrap (`remesh.cc:442`) *calls* `decimateForSolve`, so
> it must be replaced (budgeted auto paths provably never fire it —
> `0.5·L_pre ≤ 0.56·mean < mean`; explicit-target paths coarsen directly via the
> BK band).

### 9a. Field-aligned (anisotropic) tangential smooth — the one new primitive
The two existing smooths are isotropic (pull each vertex to its one-ring
centroid): dyntopo `do_smooth` (`dyntopo.h:398–410`) and the decimate relaxation
(`remesh.cc:160–218`). Both *wash out* feature flow. The new operator steers the
tangential move with the rough cross field:
- Lift the per-face field (`.remesh.f.theta` → the `(u,v)` cross directions) to a
  per-vertex tangent frame (average the incident faces' nearest representative,
  handling the 4-RoSy period ambiguity).
- Replace the isotropic Laplacian with a **field-steered** update: decompose the
  one-ring tangential delta into the `(u,v)` cross-frame and reshape so vertices
  relax toward straightened `u`/`v` isolines (directional / edge-aligned
  weighting, à la Jakob et al. instant-meshes). Keep the **tangent-plane
  projection + edge-scale clamp** (`remesh.cc:206–213`) — but the clamp alone is
  **not fold-safe**: a move up to the full shortest incident edge can invert an
  incident triangle (dyntopo's own `smoothTangent` clamps to **half** the
  shortest edge for exactly this reason). **Implemented:** `tangentialSmooth`
  now clamps to half the shortest incident edge unconditionally, plus an
  **opt-in** `fold_guard` param that cancels only *good→bad* fan transitions
  (a tri agreeing with the fan normal flipping against the fan-after, or an
  unfolded adjacent fan-tri pair creasing past 90°) — already-folded fans stay
  free to relax flat. Off by default because binary move-cancellation breaks
  the exact iso/field equivalence contracts the unit tests pin; the 9b driver
  opts in at both call sites.
- A scalar `pre_remesh_align ∈ [0,1]` blends isotropic↔field-aligned (`0`
  reproduces today's relaxation; `1` fully field-aligned) for A/B + tuning.
- **Where:** a new `remesh/preremesh.{h,cc}` (it needs both dyntopo and the field).

### 9b. The convergence driver
Outer loop on `work`, to `pre_remesh_iters` or until max vertex move < ε:
1. **Bootstrap** (`pre_remesh_bootstrap_iters`): a few *isotropic* sweeps first — a
   field-aligned smooth driven by a still-noisy field over-regularizes toward its
   own noise; denoise the geometry a little before trusting the field.
2. **Rough field** (cheap, throwaway): `computeCrossField` with a cheap params set
   (curvature constraints; no quantize). The field solve is the Eigen
   `SimplicialLDLT` — *not* the expensive stage — so recomputing every
   `pre_remesh_field_cadence` outer iters (field is stable once geometry settles)
   is affordable.
3. **Botsch-Kobbelt to a size field:** split long / collapse short / flip — reuse
   the shared BK-loop body (see the blockquote above), with the split/collapse band
   driven by a **per-vertex size field** (below) rather than one global length.
4. **Field-aligned smooth (9a).**

**Adaptive (non-uniform) sizing — and `grade` is *not* the mechanism.** dyntopo's
existing `grade` (`dyntopo.h:63–67`) relaxes the band *radially* by
`(1 + grade·dist/radius)` from the dab center — meaningless over a whole-mesh dab
(it would only coarsen away from the bbox center), so it does **not** give
curvature-adaptive triangles. Instead extend the BK candidate loop
(`dyntopo.h:613–628`) to read a **per-vertex size field**, setting the local target
to `L(v) = pre_remesh_target / sqrt(density(v))` — refine where the field turns
fastest (high curvature), coarsen flat regions. Feed it through **the full Tier 3
sizing chain each outer iter**: `generateAutoDensity → limitDensityGradation →
size-scale write` (gradation sits between generation and consumption — see 3b;
it is non-optional here), recomputed on `work` each rebuild (auto-density is
curvature-driven, so it regenerates on the new triangulation) — the *same* size
field that drives the final quad sizing, so pre-pass and output stay coherent. This
isn't cosmetic: a uniform pre-remesh too coarse where the cross field bends
*aliases* the field and can manufacture the very folds this tier exists to kill.
`pre_remesh_density = false` falls back to the single global `pre_remesh_target`
(A/B only; the default is **on**).

**BK band overlap (structural churn source — fixed).** With the classic
`4/3·L` / `4/5·L` band, a split's child edges (`l_max/2 = 0.667·L`) land *below*
the collapse threshold (`0.8·L`): splits directly feed collapse candidates. The
`max_stall_rounds` early-out bounds the damage but doesn't remove the cycle.
**Implemented** as the band-widening option: `bkRemeshToTarget` sets
`l_min = (2/3)·L` (= `l_max/2`; the strict `< l_min` compare keeps exact split
children out of the collapse band), and `applyBrushDab` itself clamps every
caller's effective `l_min` to `l_max/2`, so the sculpt path can't be handed an
overlapping band either (all current callers already pass ratios ≤ 0.5 — the
clamp is a no-op for them).
Measured on the fox at target 0.1: per-iter rounds 77/45/22/31/21 vs the
baseline pinned at the 100-round cap every iter; op counts decay 12k→~400 vs a
steady ~2400/iter churn; vert count settles instead of oscillating. (At target
0.05 iters 0–1 still hit the cap — residual churn, not the band cycle.)

### 9c. Feature pinning
`decimateForSolve` sets `preserve_features=false` and the tri copy carries no
boundary overlays — fine for blind coarsening, **wrong** for feature-following
flow (vertices slide off creases, collapses cross features). The pre-pass must
pin boundary + dihedral-sharp edges in **both** collapse and smooth.
`gatherConstraints` already reads those tags for the field; reuse the same
classification (compute sharp on `work`, or consume Tier 1b's copied constraint
layers / Tier 7's feature tags if present). v1: boundary loops + a dihedral-sharp
threshold computed on `work`.

### 9d. Pipeline integration
New pre-pass on `work` in `remesh.cc`, after triage, before `computeCrossField`,
gated on `pre_remesh`. Two integration points to get right:
- **Reproject must fire.** Output fidelity relies on the final reproject onto the
  full-res original (`remesh.cc:359–362`), today gated on the `decimated` flag.
  Generalize the gate to "`work` geometry diverged from input" so a pre-remesh
  *without* `--solve` decimation still reprojects.
- **Reproject must be fold-safe.** `reprojectToSurface` (`extract/reproject.cc`)
  is a pure closest-point snap; on thin features it snaps vertices to the
  **opposite sheet**, manufacturing fold-backs. **Implemented** as a
  normal-compatible re-query (`ReprojectParams::sheet_min_dot`, default `-0.5` =
  reject only clearly-opposite sheets; `<= -1` disables): when the plain closest
  point's face normal opposes the vertex normal, re-query through the
  `closestPointWalk` sheet filter and accept the filtered hit only within 3× the
  plain distance. Two structural facts forced the shape: quad extraction does
  **not** inherit input winding (a one-time orientation vote over ≤256 sampled
  unfiltered hits calibrates the sign), and vertex normals *lie* in folded
  umbrellas (the filter engages only where 1-ring normal coherence > 0.7, else
  the plain snap keeps ironing extraction folds flat). Fox at target 0.05:
  post-reproject fold180 ends at 41 vs the input's 39 — ≈ zero net fold-back
  added (baseline 261). Intersects Tier 6's thin-sheet case.
- **Collapse must be fold-safe (found via the Tier 0d diagnostic, fixed at the
  source).** `edge_collapse.h`'s `prevent_inversion` only rejected a collapse
  that flipped a *single* star face; it let a collapse fold two faces across
  their shared post-collapse edge. The guard now also groups surviving star
  faces (plus their outside-star radial neighbors) by post-collapse edge key and
  rejects any collapse where a not-folded pair (`n_i·n_j ≥ 0`) becomes folded
  (`< 0`). Shared with sculpt dyntopo (same `prevent_inversion=true` path);
  dyntopo regression gates stay green.
- **`--solve` is removed, not composed with.** Deep coarsening *is* the pre-pass
  with an explicit `pre_remesh_target` (see the removal blockquote above); the
  `decimated` flag and its reproject gating die with the stage — the reproject
  gate reduces to "the pre-pass ran".

### New params (mirrors `PreRemeshParams`, `remesh/preremesh.h`)
The pipeline gate `pre_remesh=false` keeps the pipeline unchanged; the knobs
below take effect only when it's on (or in the debug app's pre-pass mode).

| field | default | meaning |
|-------|---------|---------|
| `pre_remesh` | `false` | run the field-aligned input pre-remesh on `work` before the field solve |
| `pre_remesh_iters` | `0` | outer convergence iterations; `0` = auto from the measured input (clamped to [3,6]) |
| `pre_remesh_target` | `0.0f` | **base** pre-pass edge length (scaled per-vertex by `1/sqrt(density)` when `pre_remesh_density`); `0` = auto (`target_edge_length` or the count-mode formula, clamped to the 20% edge budget). The one explicit deep-coarsening route once `solve_edge_length` is removed |
| `pre_remesh_density` | `true` | drive the split/collapse band from the per-vertex curvature size field (Tier 3 sizing chain, recomputed on `work`); `false` = uniform (A/B only) |
| `pre_remesh_gradation` | `0.5f` | per-edge-hop size growth cap on the pre-pass field (3b); `0` disables (A/B only) |
| `pre_remesh_gradation_iters` | `10` | gradation work cap (pops per vertex) |
| `pre_remesh_align` | `1.0f` | isotropic(`0`)↔field-aligned(`1`) smooth blend |
| `pre_remesh_field_cadence` | `2` | recompute the rough field every N outer iters |
| `pre_remesh_bootstrap_iters` | `-1` | isotropic denoise sweeps before field-aligned begins; `-1` = auto from input noise (1/2/4) |
| `pre_remesh_smooth_iters` | `5` | inner field-aligned smooth sweeps per outer iter |
| `pre_remesh_smooth_lambda` | `0.5f` | per-sweep relaxation factor |
| `pre_remesh_converge_eps` | `0.05f` | early-out: stop when max smooth move < eps·target (`0` = run all iters); `PreRemeshParams::converge_eps` mirrors this default |
| `pre_remesh_preserve_features` | `true` | 9c: pin boundary + dihedral-sharp creases |
| `pre_remesh_sharp_angle` | `0.785f` | 9c dihedral threshold (rad) |

(`density_min`/`density_max` reuse Tier 3's fields. Full per-param surface area
for each: the 9-step list at the top of this doc.)

Reproject fold-safety adds one knob on `ReprojectParams` (not a pre-pass param —
it guards every reproject): `sheet_min_dot`, default `-0.5` (reject only
clearly-opposite sheets; `<= -1` disables the filter entirely).

### 9e. Debug-app pre-pass mode + rough-field visualization
- **Run-just-the-pre-pass:** a `pre_remesh` command (the `remesh_app.cc` dispatch,
  alongside `set_param` / `run_remesh`) and a UI button that runs **only** 9b on
  the current asset and shows the resulting *triangle* mesh — inspect the cleaned
  flow without the full quad pipeline. Add an "isotropic vs field-aligned" toggle
  (drives `pre_remesh_align`) for side-by-side.
- **Rough-field overlay (mostly free):** the app's cross-field overlay already
  reads `.remesh.f.theta` (`emitCrossField`, `remesh_debug_app.cc:399`; gated by
  `app.showCrossField`, computed via `ensureCrossField`, `:340`). The pre-pass
  writes its rough field into the **same** `.remesh.f.theta` on `work`, so the
  existing overlay visualizes it directly — the work is (a) leaving the
  intermediate field in place after the pre-pass mode runs, and (b) optionally
  re-emitting the overlay **per outer iteration** to animate convergence (a
  `pre_remesh_show_field` toggle + a step/▶ control). The curvature and streamline
  overlays come along for free since they read the same TEMP layers.
- New `RemeshApp` flags + UI widgets (with hover tooltips, per the app
  convention) for the pre-pass knobs and the show-rough-field toggle. The
  pre-pass UI block (`remesh_ui.cc`) exposes density min/max but **no gradation
  widget yet** — add `pre_remesh_gradation` (+ iters).

### 9f. (Optional, A/B last) Area-weighted tangential smooth
Weight the one-ring centroid by incident-triangle areas so the relaxation pushes
toward uniform-*area* triangles, not just uniform edge lengths. Build only if
the min-angle / edge-variance metrics stall after 9a–9e; A/B against the
standard smooth on the corpus.

### 9g. Source-anchor correspondence transport (pairs with the `--solve` removal)
Reproject is a *global* closest-point snap, patched by the `sheet_min_dot`
normal-compatibility heuristic (9d) because closest-point can land on the
opposite sheet of a thin part. The layout-embedding paper restores fidelity by
**transport** instead: nodes live intrinsically as `(face, barycentric)` on the
input and cross resolutions by transporting the map and re-tracing (§7.3 —
"optimization on coarse meshes can be prolongated to high-resolution inputs").
Adopt that: the pre-pass maintains a per-vertex **source anchor** (input face id
+ barycentric), updated through split (interpolate the edge's two anchors),
collapse (survivor keeps its anchor), and tangential smooth (local re-walk via
`closestPointWalk` from the previous anchor). Reproject then starts as a **local
walk from a valid anchor** — structurally unable to sheet-jump — and
`sheet_min_dot` demotes to a safety net. With the decimation stage gone the
pre-remesher is the sole owner of input correspondence, and this pre-builds the
intrinsic representation the layout milestone needs from day one
([research/layout-embedding-optimization.md](../research/layout-embedding-optimization.md)).

### Verification
- **Primary test config = non-uniform on.** Run the quality gates with
  `pre_remesh_density=true` (the realistic intended use); the uniform path is a
  secondary A/B point, and the no-op guarantee covers the default-off case.
- **gtest** `tests/test_remesh_preremesh.cc`: (1) a noised but smooth organic patch
  (bumpy plane / low-res sphere), density on — assert the pre-pass **raises**
  triangle quality (min-angle proxy up, edge-length variance *within a size class*
  down) and, on a feature box, that pinned sharp edges survive (vertices don't
  migrate off the crease). (2) **No-op guarantee:** `pre_remesh=false` ⇒ `work`
  byte-identical through the stage. (3) Field-noise drop: assert `crossFieldCurl`
  (already in `singularity_adjust.cc:55`) on the post-pre-pass field is **lower**
  than on the raw input's field. (4) **Adaptive grading:** with
  `pre_remesh_density=true` on a curvature-varying fixture (sphere-with-a-bump /
  plane-with-a-crease), assert local edge length is **shorter in the high-curvature
  region than on the flat region** (the size field actually grades the
  triangulation), while flat regions stay near `pre_remesh_target`. (5) **Fold
  safety:** fold90/fold180 counts (Tier 0d) after pre-pass + reproject must not
  exceed the input's.
- **Fox A/B (the motivating case):** `an-elegant-fox-character` with `pre_remesh`
  on (`pre_remesh_density=true`) vs off — Tier 0 metrics must show
  `parametrization_folds` and quantize wall time both fall sharply (the cliff was
  ~30% folds, N≈62,682; target: folds well under the seam-relax gate so quantize
  stops thrashing **without** `--solve`). Record the **uniform-density** run too,
  to show adaptive sizing helps beyond plain field-alignment.
- **Debug-app visual:** pre-pass mode on an organic asset, rough-field overlay at
  iter `0` vs final — flow visibly straightens along features.

### Review gate 9
Inspect: the quality/curl/adaptive-grading gtest, the no-op guarantee, the fox
folds+time A/B **uniform vs adaptive density** (does the pre-pass remove the
quantize cliff at full resolution, and does adaptive sizing beat uniform?), and
pre-pass mode screenshots (isotropic vs field-aligned, rough-field overlay).
Include an intermediate `pre_remesh_align ≈ 0.5` corpus point: the
layout-embedding paper keeps field alignment a small bias over the distortion
driver (`ω_align ≈ 0.1`), and full alignment against a noisy rough field can
regularize toward the field's own noise — verify `1.0` actually beats `0.5`
before keeping it as the default. Decide whether
`pre_remesh` should join the "Messy Generated Character" / "Scan" presets (Tier
8b) and whether the Tier-8 retry loop enables it automatically.

---

## Cross-tier closeout (once, at the end)

- Update the **Parameters** table in [quad-remeshing.md](../quad-remeshing.md) and
  the CLI `--help` block with every new flag.
- `node make.mjs build native && node make.mjs test native` green; `npx tsgo
  --noEmit` clean if `litemesh*.ts` changed.
- WASM smoke (`node make.mjs build wasm`) — `RemeshParams` crosses the seam, so
  every new field must be in `bindings.cc`.
- Strip remaining `CLAUDENOTE:` comments; keep only permanent ≤3-line notes.
