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
the next begins. Tiers are additive; every new knob defaults to current behavior,
so a half-finished ladder never changes existing output.

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
| **3** | Auto curvature-density + gradation limiting | sizing | Generates a size field (small at face/folds, large on torso) + bounds its gradient. |
| **4** | Smoothness/alignment master knob | field filter | Dial smoothness up to suppress noise-born singularities. |
| **5** | Singularity pair cancellation | field cleanup | Removes residual +k/−k clutter Tier 2 didn't eliminate. Gated on whether it's needed. |
| **6** | Boundary / thin-part / component policy | topology policy | Eyelids, mouth holes, cuffs, accessories, double-sided sheets — a big character lever. |
| **7** | Feature-graph hysteresis + pruning | field filter | **Last filter tier** (CAD off-target). Cleans noisy hard constraints. |
| **8** | Retry/robustness policy + presets | orchestration | Capstone: turns knobs into an automatic policy; needs Tier 0 metrics + all knobs. |

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

**Default rule (with one carve-out):** every new knob defaults so each merge is a
no-op until turned — **except Tier 1**, the intentionally behavior-changing
robustness tier, whose destructive cleanup defaults **off** until review gate 1
validates it on the corpus, then flips on.

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
- **add**: **component count** + **hole count** (input vs output) — **define these
  precisely**: *component count* = connected components over the face-adjacency
  graph; *hole count* = number of **boundary loops** (NOT genus-derived handles,
  NOT output residual cap loops — those are separate). Document the definition next
  to the metric so cross-run comparisons are meaningful;
- **add**: per-component singularity count;
- **add**: pre-extraction **parametrization fold count** (distinct from output
  `inverted_faces`);
- runtime + failure reason come from 0a's report.

### 0c. Corpus + tracker
Assemble a small fixed set (~6–10) of deliberately ugly assets (Meshy character,
raw scan, thin-sheet clothing, holed face, multi-component accessory). Add a
debug-app/CLI batch verb that runs all of them and writes a metrics table
(CSV/JSON) so tiers can be compared run-over-run. Keep assets out of git if large;
reference by path.

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
map" branch is undecidable. Add a pass copying the **input** constraint layers by
vertex map (and, after triangulation, transferring face-domain `stroke_dir` to the
split faces).

> **Copy the input layers, NOT the computed ones.** The `remesh.cc:24` TODO says
> "`pole_index`", but `.remesh.v.pole_index` is **computed output** (written by
> the field solve at `cross_field.cc:242`, consumed by `singularity_adjust` /
> `cut_graph` / `quad_extract`). Copying an input `pole_index` would **collide
> with / be overwritten by** the computed field. The actual **user-pin input**
> layer is `.remesh.v.pole_pinned` (bool, `singularity_adjust.cc:200`). So 1b
> copies **`.remesh.v.density` + `.remesh.v.pole_pinned` + `.remesh.f.stroke_dir`**
> only. (If users ever need to pin a *specific index value*, that is a new
> dedicated input layer — never the computed `pole_index`.)

Until this lands, **auto-density (Tier 3) is the only supported density path** —
state that explicitly rather than implying painted maps work.

**Where:** a new helper (e.g. `remesh/triage.{h,cc}`) + the attr-copy folded into
`buildTriCopy`, invoked right after the copy, before decimation/curvature.
`mesh_validate.h` for detection.

### New params
| field | default | meaning |
|-------|---------|---------|
| `triage` | `false` | run input cleanup (weld/degenerate/tiny-component drop). Default **off** until review gate 1 validates it; flip on after |
| `triage_weld_rel` | `1e-5f` | weld tolerance as a fraction of bbox diagonal |
| `triage_min_component_frac` | `0.0f` | drop components below this fraction of total verts (0 = keep all) |

(`triage` defaults **off** to keep merges no-op; gate 1 decides whether to flip it
on by default. The attr-copy of 1b is unconditional — it only *adds* layers, never
destroys.)

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

### 3b. Gradation limiting (Alauzet bounded gradation)
**Where — get this right:** density has two consumers and the *earlier* one sets
local size:
- **M4 seamless param** — `buildSeamlessSystem` reads `.remesh.v.density` and
  folds `1/sqrt(density)` into the FEM target-gradient (`seamless_internal.cc:166`).
  **This sets local size.**
- M5 quantize re-reads it at `quantize_ilp.cc:513` (secondary).

So the limiter runs as a **prepass before `buildSeamlessSystem`** (before
`computeQuantization` in `remesh.cc`), writing the limited values back so **both**
reads see the filtered field. Algorithm: convert density→size `h`, iterate
`h[v] ← min over neighbors n of (h[n] + beta·dist(v,n))` to a fixed point / `N`
sweeps, convert back. Confirm both call sites consume the filtered values.

### New params
| field | default | meaning |
|-------|---------|---------|
| `auto_density` | `false` | generate `.remesh.v.density` from curvature |
| `density_min` / `density_max` | `0.25f` / `4.0f` | clamp on generated density (size range) |
| `density_gradation` | `0.0f` | max size growth rate; `0` = off. Typical `0.3–1.0` |
| `density_gradation_iters` | `10` | limiter sweep cap |

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
- Document: effective regularization ≈ `field_smoothness / curvature_weight`;
  surface both together in the UI.

### New params
| field | default | meaning |
|-------|---------|---------|
| `field_smoothness` | `1.0f` | per-edge smoothness weight; higher = smoother field, fewer singularities |

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
| `singularity_cancel` | `false` | enable adjacent +k/−k pair cancellation/relocation |
| `singularity_cancel_radius` | `2` | max dual-graph hops for a "nearby" pair |

### Verification
- **gtest:** synthetic field with a planted adjacent +1/−1 pair on a flat region;
  assert the pair is cancelled and `index_sum` unchanged. A genuine pole (sphere's
  required singularities) must **not** be cancelled.
- **Corpus:** singularity count + fold count drop without raising skinny-quad
  score.

### Review gate 5
Inspect: planted-pair test, corpus deltas, confirm required poles survive.

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
- **Output residual cap (existing):** keep `cap_odd_holes` / the extraction cap
  as-is for spurious *output* loops; just make sure the two policies don't
  double-close a legitimately-open rim.
- **Boundary preservation:** keep large open rims aligned (already pinned as
  boundaries in `feature_tag`); verify they survive reprojection.
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
- **Corpus:** holed-face and multi-accessory assets — hole-count and
  boundary-preservation-error metrics behave as intended.

### Review gate 6
Inspect: hole-policy test, component handling, corpus boundary metrics, and the
thin-sheet detection/report.

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
- too many residual odd holes / tri caps → coarser `target_edge_length`, retry.
Hard cap on attempts; record **which fallback won** and final metrics in the
manifest. Default attempts = 1 (today's behavior) unless `auto_retry` is on.

### 8b. Presets
Named bundles setting the whole knob vector: **Organic Clean**, **Organic Noisy**,
**Messy Generated Character**, **Scan**, **Hard Surface**. A preset sets curvature
smoothing, solve-edge length, field smoothness, auto-density + gradation, hole/cap
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

## Cross-tier closeout (once, at the end)

- Update the **Parameters** table in [quad-remeshing.md](../quad-remeshing.md) and
  the CLI `--help` block with every new flag.
- `node make.mjs build native && node make.mjs test native` green; `npx tsgo
  --noEmit` clean if `litemesh*.ts` changed.
- WASM smoke (`node make.mjs build wasm`) — `RemeshParams` crosses the seam, so
  every new field must be in `bindings.cc`.
- Strip remaining `CLAUDENOTE:` comments; keep only permanent ≤3-line notes.
