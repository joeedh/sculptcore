# Unified Sculpt-Layers Displacement System — Design Report

A design investigation for a single layered-displacement system that spans both
base-mesh kinds (**polygon** and **subdivision surface**) and both displacement
carriers (**vertex-coordinate layers** and **UV vector-displacement maps**), with
the two carriers composable in a well-defined order.

Companion reading (both already in this tree and load-bearing for this design):
[`tangent-displacement-issues.md`](tangent-displacement-issues.md) (the offset-
surface fold bound §6, smooth tangent frames §4.5, multires edit propagation §5)
and [`dyntopo-vdm-region-hybrid.md`](dyntopo-vdm-region-hybrid.md) (region
partition, the `.detail.carrier` tag, promotion/demotion, the static-base
tangent-frame dividend). This report assumes those and does not re-derive them.

> Scope note: this is a *design report with open questions*, not yet an
> implementation plan. The existing TypeScript `mesh_displacement.ts` layer
> system is **explicitly out of scope** — this is a fresh native (`sculptcore`)
> design built on the C++ attribute / dyntopo / boundary primitives.

> **Status (2026-08-20):** phasing step 1 has shipped —
> `AttrUse::SCULPT_LAYER` with the settings sidecar and the compositor
> (`source/displace/`), plus the layer c-api (`layerAdd` …
> `layerTableRestore`) wired to the Blender addon. On the multires side a
> layer is a per-grid store channel, edited per dab through the
> `LayerScratch` binding when a live edit target is armed — see
> `engine/CLAUDE.md` § *Attributes on the grid domain*. Later steps
> (VDM regions, promotion, the tangent-frame options) remain design-stage.

---

## 1. Goals and the unifying abstraction

We want one **layer stack** concept that means the same thing regardless of base
kind or carrier:

> The displayed surface is the base surface plus the ordered composition of all
> enabled sculpt layers. Each layer contributes a per-surface-point displacement;
> the layers differ only in *how that displacement is stored and sampled* and *in
> what reference frame it is applied*.

The two base kinds and two carriers populate a 2×2 matrix, but they reduce to a
small number of common operations (evaluate, interpolate-onto-new-geometry,
clamp, refit-down). That common interface is what makes the system "unified."

| | **Vertex-coordinate layer** | **UV vector-displacement map (VDM)** |
|---|---|---|
| **Polygon base** | per-vertex `float3` attribute (dyntopo-compatible) | UV/Ptex texture over a fixed sub-region |
| **Subsurf base** | per-subdiv-vertex displacement at a level = **multiresolution stack** | UV/Ptex over the (locked) cage |

Key framing decisions that fall out and are defended below:

- A vertex sculpt layer is a **new attribute *category*** (an `AttrUse` tag plus
  a per-layer settings side-table), not merely "a `float3` attribute."
- **VDM layers always come last** in composition; vertex layers compose first.
  This is the user's instinct and it is the correct one (§5).
- **Propagation between layers** is *automatic* for frame-relative (tangent)
  layers via composition, and *explicit* for the downward refit/reproject
  direction; the multires subsurf "it just rides along" behavior is the
  automatic case (§6).
- **VDM magnitude must be clamped regardless of base kind** because a VDM has
  neither of the two escape hatches a vertex layer has (add geometry, or refit
  the base) inside its own fixed-parameterization region (§8).

---

## 2. The sculpt-layer attribute category (polygon + the common storage)

Today's attribute metadata (`source/mesh/attribute_enums.h`) has two orthogonal
classifiers on every `AttrRef`:

- `AttrFlag` (operational): `TOPO | TEMP | NOCOPY | NOINTERP | TOPO_KEEP_FROZEN`
  — controls how topology / copy / interpolation treat the column.
- `AttrUse` (semantic): `NONE | UNIT | COLOR | UV | POLYGROUP` — what the data
  *means* to UI/render/semantics.

A vertex sculpt layer is a `FLOAT3` column on the `VERTEX` domain that the
displacement system treats specially. The clean way to mark it is a **new
`AttrUse` value**, e.g. `AttrUse::SCULPT_LAYER`, *plus* a per-layer settings
record (a sidecar, keyed by attribute name/index), because a layer carries more
state than one bit:

```
struct SculptLayerSettings {
  enum Mode  { DELTA, TANGENT } mode;     // how stored values map to world disp
  enum Space { WORLD, OBJECT }  space;    // for DELTA mode
  int   parent;        // index of the layer this one is relative to (-1 = base)
  float weight;        // opacity / blend weight
  bool  enabled;
  bool  frozen;        // excluded from active editing
  float clampFrac;     // α in §8 (VDM and optionally tangent vertex layers)
};
```

Why a category and not "any `float3`": a generic `float3` attribute (e.g. a
custom color-as-vector, a cached normal) must **not** be folded into the
displayed position. The category tag is the gate that the surface-evaluation pass
and the brush iterate over; everything else (custom `float3`s, `.spatial.*`,
normals) is invisible to it.

What we get for free from the existing infrastructure:

- **Interpolation onto new geometry.** `interpAttrs` /`interpAttrRows`
  (`source/mesh/utils/attr_interp.h`) already lerp every non-`TOPO`/non-`TEMP`
  floating-point column. A `float3` vertex layer is therefore interpolated
  correctly on edge split/collapse with **zero new code** — *for the DELTA
  (world) storage case*. (The TANGENT case has a caveat; see §7.)
- **Undo.** Vertex layers are plain per-element attributes, so they ride the
  existing meshlog attribute-swap path (`LogChunkElems`, declared via the brush's
  sbrush `save` statement) like any sculpted position. No new undo channel
  needed for the vertex carrier.
- **GPU / draw.** The evaluated world position is what already flows to the
  spatial tree / draw buffers; the layer stack feeds the position pass, it does
  not need a new requested-attribute contract unless we want to *render* a layer
  in isolation.

---

## 3. Frames: "tangent basis" vs "simple delta"

The user specifies vertex layers use *either* a tangent-space basis *or* a simple
delta. These are genuinely different and the difference drives the whole
ordering/propagation analysis, so state it precisely:

- **DELTA (world/object).** Stored value `d` is added directly: contribution
  `= d` (world) or `R_obj·d` (object). No frame, no fold bound, commutative,
  exact under interpolation. **Does not ride** when a layer below it moves —
  the vector is absolute.
- **TANGENT.** Stored value `v` is expressed in a per-vertex frame
  `[t, b, n]` derived from the surface *below* this layer; contribution
  `= [t,b,n]·v`. **Rides** base/lower-layer motion (the frame rotates/translates
  with it), which is exactly the property multires depends on — but it is subject
  to the offset-fold bound (§8) and is only approximate under naive
  interpolation (§7).

The frame for the TANGENT case:

- `n` = the *smoothed* base/lower normal (not the faceted per-face normal — a
  frame discontinuity manufactures an artificial zero-radius bend, see
  `tangent-displacement-issues.md` §6.3).
- `t` (azimuth about `n`) = from the UV gradient *or* from a cross field. The
  cross field (`tangent-displacement-issues.md` §4.5) is UV-independent and
  smoother but is only affordable on a **static** base — which is precisely the
  subsurf cage and the VDM regions, not a churning dyntopo region.
- For **subsurf**, the frame comes from the **discrete smoothed normal + a cross
  field** on the static cage (per the §4.2 roll-our-own decision). Analytic
  limit-surface derivatives `∂p/∂u, ∂p/∂v` would be smoother still, but that is
  the OpenSubdiv-shaped path we are deferring to an optional bake stage.

**Storage of the frame** is a decision point (§11): recompute deterministically
on the fly (no memory, must be bit-synchronized everywhere it is read) vs. cache
it in a `TEMP`/derived attribute (memory, guaranteed synchronization). For an
all-in-engine system (no external bake to a foreign renderer) recompute-on-fly
is attractive; a cached frame becomes important if/when we bake to an external
VDM.

---

## 4. The two base kinds

### 4.1 Polygon base

- Vertex layers = per-vertex `float3` attributes, one per layer, tagged
  `SCULPT_LAYER`.
- **Dyntopo runs freely.** New vertices inherit layer values via `interpAttrs`;
  the boundary/feature system protects parameterization (§7).
- VDM layers ride the existing `CORNER`-domain UV attribute (`AttrUse::UV`), with
  the texture store UV-keyed and *outside* the spatial tree (per the hybrid doc).
  On a polygon mesh a VDM is naturally tied to the region-partition
  (`.detail.carrier`): VDM lives only where the surface is height-field-stable,
  geometry/vertex layers carry the rest.

### 4.2 Subdivision-surface base — topology locked

- The cage topology is **locked**; this system never adds/removes subdiv
  vertices. (Any operation that changes subsurf topology is a *separate new
  system*, per the brief.)
- The "vertex coordinate layer" here is the **multiresolution stack**: each
  subdivision level `L` stores per-`L`-vertex displacement relative to the
  *smoothed* level `L-1`, in a tangent frame at `L`. This is the classic
  displaced-subdivision pyramid (`tangent-displacement-issues.md` §5), in its
  **discrete** form (see below).
- Because topology is locked there is **no dyntopo interplay** on subsurf — the
  multires stack *is* the geometry carrier, and the only way to add carrying
  capacity is to add a level (a deliberate op), not to remesh.
- A VDM sits on top of the finest level for the highest-frequency residual.

**Implementation decision — roll our own subdivision (not OpenSubdiv).** The
native engine has no subsurf today, so this is greenfield C++ either way. We will
implement a **uniform Catmull-Clark refiner** over `mesh::Mesh` rather than vendor
OpenSubdiv. Rationale:

- Locked topology means the subdivision **stencils** (each fine vertex as a fixed
  linear combination of cage vertices) are computed **once and cached** — there is
  no incremental/adaptive churn that OSD's machinery exists to manage.
- We adopt the **discrete multires model** (Blender-style): a level's "smooth"
  surface is the *discretely* Catmull-Clark-subdivided level below, and the layer
  stores displacement relative to that. This needs **no analytic limit-surface
  evaluation** — which is the one thing (Gregory patches at extraordinary
  vertices) that OSD is uniquely good at and that is otherwise hard to roll.
- The tangent frame on subsurf comes from the **discrete smoothed normal + a
  cross field** on the static cage (affordable precisely because the cage does not
  churn — `tangent-displacement-issues.md` §4.5 / hybrid doc §6), not from
  analytic limit derivatives.
- A self-contained refiner fits the engine's ethos (litestl, no STL in hot paths,
  trivial WASM build, direct `mesh::Mesh` integration); OSD is a large STL-heavy
  dependency whose `Far` data model would have to be marshaled to/from ours.
- Creasing for v1 is simple sharp/boundary rules only (the boundary system already
  tracks `EDGE_SHARP`).

OpenSubdiv stays a **possible later, bake-path-only** option, reconsidered *only*
if we need analytic limit evaluation for **external VDM/Ptex export** to a foreign
renderer, or production-accurate **semi-sharp creases** — and even then confined to
the export path, never the live sculpt loop. This choice also leans the VDM
carrier toward a UV atlas over Ptex (Ptex pairs naturally with the analytic-limit
/ OSD world; see §11 Q3).

The unification: a subsurf level's per-vertex displacement and a polygon
vertex layer are the *same operation* (frame-relative per-vertex `float3`),
differing only in which vertex set they live on (cage-subdivided vs. base) and
where the frame comes from (limit derivatives vs. smoothed base + UV/cross
field).

---

## 5. Composition and ordering — feasibility of "any order"

This is the central feasibility question. The answer hinges on **whether a
layer's frame depends on the cumulative result of earlier layers.**

Let `S₀` be the base surface and `Sₖ = S₀ + Σ_{i≤k} contributionᵢ`.

- **DELTA layers** add a fixed vector independent of any frame → mutually
  **commutative**. Any order, exactly, with no cost.
- **TANGENT vertex layers and VDMs** apply their stored value through a frame.
  If that frame is read from `Sₖ₋₁` (the running surface), order is
  **fundamental**: the layer rides whatever is below it.

Two coherent global models, and the recommended hybrid:

1. **Fixed-frame** — every tangent frame is read from the base (or smoothed
   base) only. All contributions become fixed vectors → fully commutative, most
   stable, simplest. Cost: a big low-frequency vertex layer does not re-orient
   the high-frequency detail stacked on it (often *desirable* for stability, but
   not "sculpt-stack" intuitive).
2. **Stacked-frame** — every layer's frame is read from the running surface →
   order matters everywhere, costs a frame re-derivation per layer, and the fold
   bound applies to the *running* curvature (errors compound). Most intuitive,
   least stable, most expensive.

**Recommended hybrid (answers "any order" precisely):**

- **Vertex layers compose first**, among themselves in order. DELTA ones are
  commutative; TANGENT ones are a stack (each relative to the running surface, or
  to its declared `parent`). This produces an intermediate surface `S_vert`.
- **All VDM layers compose last, against a *single* smooth frame derived from
  the smoothed `S_vert`.** Their sampled vectors **sum** before the frame is
  applied, so VDM layers are **order-independent among themselves**, and only
  their *total* magnitude is clamped against `ρ_min` (§8). A VDM therefore never
  reframes another VDM (correct: high-frequency detail should not re-orient other
  high-frequency detail).

Why VDMs must come after vertex layers (confirming the brief):

- The fold-stability argument (`tangent-displacement-issues.md` §6) *wants* the
  low/mid frequency carried as geometry/vertices (which can refit `ρ_min`
  upward) and only the small high-frequency residual in the VDM. Vertex-first
  realizes that.
- A VDM placed *before* a tangent vertex layer would force that vertex layer's
  frame to be built on the VDM-bumped surface — circular (the VDM's own clamp
  depends on a frame that the later layer then changes) and unstable.

So: **"any order" is feasible and exact for DELTA layers; for the frame-bearing
carriers the well-defined, stable composition is vertex-layers-then-VDM, with
VDMs commutative among themselves.** I recommend enforcing that partition rather
than allowing fully arbitrary interleaving.

---

## 6. Propagation of displacement between layers

The brief asks specifically *in what cases* we must propagate between layers, and
notes it is automatic in a multires subsurf. There are four distinct cases:

1. **Edit a lower layer, higher layers should follow — AUTOMATIC for TANGENT.**
   Because a TANGENT (frame-relative) layer's contribution is recomputed through
   the running frame at evaluation time, moving a lower layer carries the higher
   detail along *for free* — nothing is propagated, the composition simply
   re-evaluates. **This is exactly the multires "go low, push the big forms, the
   fine bumps ride along" behavior.** It requires the higher layer to be TANGENT;
   a DELTA (world) layer will *not* follow and will point wrong after a lower
   edit. → *Cases needing no work: tangent/multires stacks.*

2. **Push a high-level edit DOWN into a lower layer / the base — EXPLICIT
   (refit / reproject).** The hard direction (`tangent-displacement-issues.md`
   §5). You cannot uniquely un-subdivide an arbitrary fine edit, so this is a
   least-squares reprojection: fit the lower layer to absorb the *smooth*
   (low-frequency) part of the higher layer's displacement, then re-express the
   higher layer relative to the new lower surface. Needed when:
   - the high-frequency magnitude approaches the fold bound and we want to move
     low frequency down to raise `ρ_min` (the **base-refit lever**, §8 — the main
     reason this op exists);
   - the user invokes an explicit "sculpt base / reshape" operation;
   - we want a DELTA layer to start following a lower edit (re-fit it once).
   This is an explicit, non-live (deferred) operation in all packages and should
   be here too.

3. **New geometry under dyntopo (polygon) — AUTOMATIC via attribute interp.**
   Split/collapse run `interpAttrs`, which lerps the `float3` vertex layers onto
   new vertices. VDM layers need *no* propagation — they are UV-keyed; new
   vertices simply get interpolated corner UVs and sample the same texture. (See
   §7 for the tangent-layer caveat.)

4. **Cross-representation propagation (bake / extract) — EXPLICIT.** Converting a
   VDM into a vertex layer (or extracting a VDM from vertex detail) is a bake, by
   definition explicit. This is also how the §8 "promotion" turns a VDM region
   into geometry.

Summary: propagation is *free* whenever detail is stored frame-relative and read
through composition (the multires/tangent case, and dyntopo interpolation); it is
an *explicit* tool only for the downward refit/reproject and for cross-carrier
bakes.

---

## 7. Dyntopo interaction on polygon meshes (the boundary system)

The brief's claim — *polygon meshes with vertex or VDM layers survive dyntopo via
the boundary system* — checks out against the code, with one caveat to design
around.

**What already works:**

- `interpAttrs` / `interpAttrRows` (`source/mesh/utils/attr_interp.h`)
  generically lerp every non-`TOPO`/non-`TEMP` float column on split/collapse, so
  DELTA vertex layers ride remeshing exactly and for free. Corner UVs lerp on
  split (so a VDM's parameterization stays continuous), and edge feature flags
  propagate to child edges.
- The **boundary/feature system** (`source/mesh/boundary.h`,
  `FeatureViews` in `source/dyntopo/dyntopo.h`) marks `EDGE_SEAM`, `EDGE_SHARP`,
  `EDGE_PROJECTED`, derived `EDGE_POLYGROUP` / `EDGE_UVCHART`, and a per-vertex
  `VERT_CLASS` bitmask. With `preserve_features` on, dyntopo **never splits a
  flagged edge into garbage, never collapses across a feature vert, never flips
  or smooths a feature edge/vert** — which is exactly what keeps a VDM's UV
  parameterization (and a region boundary) intact under remeshing.

**What we add for sculpt layers:**

- Region-boundary edges between a VDM region and a geometry/vertex region
  (`.detail.carrier` transitions from the hybrid doc) must be promoted to dyntopo
  **feature edges**, and any UV-chart seam inside a VDM region likewise, so
  remeshing respects the parameterization. A new `BoundaryClass` bit
  (`BC_LAYER_REGION`) is the natural home.

**The caveat — TANGENT vertex layers under remesh.** Lerping the *stored
tangent-space value* on an edge split is not identical to lerping the *world
displacement*, because the endpoint frames differ. For small (high-frequency)
displacements with smooth frames the error is negligible, and feature
preservation keeps splits off discontinuities — but it is not exact. Three ways
to handle it, in increasing fidelity:

1. accept the lerp as an approximation (fine for small residual detail);
2. store the layer's *world displacement* as the canonical value (lerp it
   exactly, derive the tangent representation lazily) — this collapses TANGENT
   storage toward DELTA-with-a-frame and is the simplest exact option;
3. hook the new-vertex `MeshCallbacks::onVertCreate` to recompute the frame at
   the new vertex and re-project the (exactly-lerped) world displacement back
   into tangent space.

Recommendation: default polygon vertex layers to **DELTA/world storage** (exact
dyntopo, commutative) and reserve true TANGENT frame-relative storage for the
**subsurf multires** path (locked topology → stable frame → the caveat never
arises) and for VDMs. Offer TANGENT polygon layers via option (3) when a user
specifically wants "ride the base deformation" on a polygon mesh. This is a
decision point (§11).

---

## 8. Limiting displacement inside VDM layers (and why VDM is special)

The offset-surface fold bound (`tangent-displacement-issues.md` §6) is geometric
and unavoidable: a tangent/normal/vector displacement self-intersects once
`|displacement| > ρ_min = 1/|κ_max|` of the surface it rides on, and the frame
rotation under base curvature compounds it. A DELTA (world) vertex layer is *not*
a parameterized offset and has no fold bound — it can make any shape (including
geometrically self-intersecting ones that are still valid geometry). The bound
applies to **TANGENT and VDM** carriers.

VDM is the carrier that **must** be limited, because inside its fixed-
parameterization region it has *neither* escape hatch the others have:

- a polygon vertex region can **add geometry** (dyntopo) to carry more detail;
- a lower layer / base can be **refit** to raise `ρ_min` (§6 case 2 above);
- a VDM region's base + UV is *fixed by construction* — so its only options are
  to clamp, or to stop being a VDM region.

Therefore: **clamp total VDM magnitude to `α·ρ_min` (α≈0.5) of the smoothed
post-vertex surface, with hysteresis**, and on violation choose by base kind:

- **Polygon base:** prefer **promotion** (VDM → geometry) per the hybrid doc —
  subdivide the affected base faces, seed verts from `base + VDM`, flip
  `.detail.carrier → GEOM`, hand to dyntopo/meshlog/spatial via `MeshCallbacks`.
  Clamp is the safety net; promotion is the real fix because it preserves the
  detail as geometry instead of discarding it.
- **Subsurf base (locked):** promotion is *not available* (topology is locked
  and lives in the separate subsurf system). So the choices narrow to **hard
  clamp** or **push the detail into a finer multires level** (add a level — a
  deliberate op). On locked subsurf the VDM clamp is a true ceiling.

`ρ_min` is measured on the **base/lower** surface, which is why the base-refit
lever (§6 case 2) is the knob that *expands* how much a VDM can safely hold.

---

## 9. Cross-cutting concerns

- **Undo.** Vertex layers ride the existing meshlog attribute-swap path. VDM
  layers need their **own undo channel** (tile/texel deltas), distinct from the
  geometry log — flagged as an open item in the hybrid doc and confirmed here.
- **Serialization.** Vertex layers serialize as ordinary attributes plus the
  `SculptLayerSettings` sidecar; VDM needs an image/tile store format. (Note the
  broader caveat that LiteMesh serialization is currently stubbed.)
- **Evaluation cost / caching.** The composed world position is what the spatial
  tree and draw path consume. Composition should be incremental — only re-evaluate
  vertices/texels in the dab region per stroke, mirroring how dyntopo and the
  spatial tree already work `O(brush region)`.
- **Display of a single layer in isolation** (for UI) would need the
  layer-isolated evaluation; this is a render concern, not core to composition.

---

## 10. Suggested phasing

1. **Vertex layers on polygon meshes, DELTA storage.** New `AttrUse::SCULPT_LAYER`
   category + settings sidecar; composition pass; rides dyntopo for free; meshlog
   undo for free. Smallest correct slice, exercises the category + composition.
2. **VDM layers on a static polygon region** with the §8 clamp and the
   `.detail.carrier` region partition + feature-edge protection. Adds the
   UV-keyed store and VDM undo channel.
3. **Promotion (VDM → geometry)** on polygon meshes — closes the §8 loop.
4. **Subsurf multires stack** (locked topology): our own uniform Catmull-Clark
   refiner (cached stencils), discrete multires model, per-level frame-relative
   vertex layers with automatic upward ride and explicit downward refit; discrete
   smoothed + cross-field frames; VDM on the finest level.
5. **TANGENT polygon vertex layers** (the §7 option-3 reprojection hook) if
   demand exists.

This orders the work so each milestone is independently useful and the hardest,
least-decided pieces (subsurf multires, demotion) come last.

---

## 11. Open questions / decisions needed

**Resolved so far:** subsurf subdivision is **rolled in-house** — a uniform
Catmull-Clark refiner with cached stencils over `mesh::Mesh`, using the discrete
multires model and discrete-smoothed + cross-field frames; **not OpenSubdiv**
(deferred to an optional bake-path-only role). See §4.2.

1. **Composition frame model.** Confirm the recommended hybrid (vertex layers
   first, ordered/stacked among themselves; all VDMs last, sharing one frame from
   the smoothed post-vertex surface, commutative among themselves). Or do you
   want a stricter fixed-base-frame model (everything commutative, VDM does not
   ride vertex layers at all)?

2. **Polygon vertex-layer storage default.** DELTA/world (exact under dyntopo,
   commutative, does not ride lower edits) vs TANGENT (rides lower edits, needs
   the §7 reprojection hook to stay exact under remesh). Recommend DELTA default,
   TANGENT reserved for subsurf multires + opt-in on polygon. Agree?

3. **VDM carrier format.** Global UV atlas vs Ptex-style per-face grids. Ptex
   removes seam layout and pairs naturally with subsurf (one face ↔ one patch) but
   needs an adjacency table; an atlas reuses the existing `CORNER` UV attribute
   directly. The §4.2 roll-our-own / discrete-multires decision **leans toward an
   atlas** (Ptex's affinity is with the analytic-limit / OSD path we deferred).
   Confirm?

4. **Is a whole-mesh VDM allowed, or is VDM always region-partitioned?** i.e. can
   a user put a plain VDM on a static polygon base without the `.detail.carrier`
   hybrid machinery, or is the region partition mandatory whenever a VDM exists?

5. **Subsurf scope / timing.** Is the subsurf multires carrier in scope for v1,
   or is v1 polygon-only (vertex layers + VDM) with subsurf as a later milestone?
   (The "subsurf topology is a separate new system" note suggests later.)

6. **On clamp violation:** polygon → auto-promote to geometry vs hard clamp vs
   warn; subsurf → hard clamp vs require an explicit new multires level. Confirm
   the per-base-kind policy in §8.

7. **Tangent-frame source & storage.** Subsurf is settled (discrete smoothed +
   cross field, §4.2). Remaining: for **polygon** tangent layers/VDMs, UV-gradient
   tangent vs cross field; and recompute-on-fly vs cached `TEMP` frame attribute.
   Any need to keep a *bake-synchronized* stored frame (i.e. do we anticipate
   exporting VDMs to an external renderer)?

8. **Layer blend semantics.** Just enable + scalar weight, or full blend modes
   (add / multiply / masked) and per-layer vertex masks?
