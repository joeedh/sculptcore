# Region-Partitioned Hybrid: Dyntopo Geometry + UV-Mapped Vector Displacement

A design note on carrying surface detail in **two disjoint carriers chosen per
region** — a UV-mapped vector displacement map (VDM) where the surface is
height-field-like, and live dyntopo geometry where it isn't — with a geometric
predicate deciding the boundary. This is the "by region" option from the
dyntopo↔VDM interop discussion; it is the only way to combine the two that does
**not** put texels in the spatial tree.

Companion reading: [`tangent-displacement-issues.md`](tangent-displacement-issues.md)
(§6 offset-surface fold bound, §4.5 tangent frames), [`dynamic-topology.md`](dynamic-topology.md),
[`spatial.md`](spatial.md).

## 1. Why partition at all

Dyntopo and a VDM are **competing** detail carriers: dyntopo stores detail *as
geometry* (add/remove vertices to track the sculpt), a VDM stores it *as
texture* over coarse geometry. They cannot share a region, because a precomputed
texel structure needs a **stable parameterization** and dynamic remeshing
**destroys** stable parameterization. Trying to bind them anyway — a
geometry-keyed (spatial-tree-resident) texel index under live remeshing — is
expensive for structural reasons (no stable key under churn; UV seams shatter
the contiguous-slice packing the GPU nodes use; the texels are redundant with
the very vertices dyntopo is adding). See the interop discussion for the full
cost argument.

The resolution is to never let the carriers overlap: **partition the surface so
each region is owned by exactly one carrier**, with the partition driven by a
geometric predicate. Disjoint regions mean detail is never stored twice and no
texel ever needs a home in the spatial tree.

## 2. The selector: a geometric eligibility predicate

The decision is not aesthetic — it is exactly the stability boundary of
tangent-space displacement from `tangent-displacement-issues.md` §6. A region is
**VDM-eligible** iff a tangent-space VDM can represent it *stably*; otherwise it
is **geometry-owned** (dyntopo).

Let the base (control / smoothed) surface in a candidate region have point
`p(u,v)`, unit normal `n`, signed principal curvatures `κ₁, κ₂`, and let the
detail surface be `q = p + D` (`D` the displacement; for a scalar map `D = d·n`).
Two independent failure modes disqualify a region:

1. **Normal-offset fold (cusp).** Even a pure height field folds once the offset
   exceeds the local curvature radius: self-intersection when
   `|d| > ρ_min = 1 / |κ|_max` in the concave-toward-offset direction. Require a
   safety margin: `|d| ≤ α · ρ_min` with `α ∈ (0,1)` (e.g. `α ≈ 0.5`).

2. **Overhang / tangential fold.** The detail surface is not a height field over
   the base — it turns back under itself. Detect as the detail-surface normal
   `n_q` deviating from `n` beyond a threshold (`angle(n_q, n) > θ_max`, with
   `θ_max < 90°`), equivalently the parameterization Jacobian `∂q/∂(u,v)`
   losing rank / flipping orientation.

Combined predicate over a region `R`:

```
eligible(R)  ⇔   max_{p∈R} |offset along fold dir| ≤ α·ρ_min(p)
            AND  max_{p∈R} angle(n_q, n)          ≤ θ_max
```

`ρ_min` is measured on the **base**, not the detail surface. This connects
directly to the base-refit lever (`tangent-displacement-issues.md` §6.1):
pushing a region's low-frequency form down into the base geometry *raises*
`ρ_min`, which *expands* the VDM-eligible area and defers promotion to geometry.
Refitting the base is therefore the knob that keeps as much detail as possible in
the cheap texture carrier.

## 3. The two carriers and how the partition is stored

- **VDM region — texture carrier.** A flat UV-space store (a tiled / virtual-
  texture pyramid, or Ptex-style per-face grids), **keyed by UV, never by the
  spatial tree**. The base mesh + UV is *fixed* here. Edits land in the texture
  by rasterizing the brush footprint into UV space via the current geometry's
  UVs — `O(brush region)`, fully independent of the spatial tree's topology
  churn. Per-corner UVs ride the existing `CORNER` attribute domain.

- **Geometry region — dyntopo carrier.** Unchanged from today: real geometry,
  owned by the `SpatialTree` exactly as it is now (`.spatial.f.node` ownership,
  `fill_leaf_attr` GPU buffers, incremental `add_face_at` + deferred rebalance).
  No UV stability is required here — detail lives in vertices.

- **The partition itself** is a `FACE`-domain `int` tag (mirroring
  `.spatial.f.node`): `.detail.carrier ∈ {VDM, GEOM}`. It is cheap to read in
  the dab loop and is the single source of truth for which carrier owns a face.

## 4. Carrier transitions (promotion / demotion)

The partition is dynamic; the selector moves the boundary as the sculpt evolves.

- **Promotion (VDM → GEOM)** — triggered when an edit pushes a VDM region past
  the §2 predicate (an incipient fold or overhang). Local and one-way:
  1. Subdivide the affected base faces into real geometry at a resolution
     matching the local texel density.
  2. Seed the new vertices by sampling `base + VDM` (position **and**
     interpolated attributes) so the geometry starts exactly on the displaced
     surface — no popping.
  3. Flip `.detail.carrier → GEOM`, hand the new faces to dyntopo / meshlog /
     spatial through the existing `MeshCallbacks` (`onFaceCreate`, …) so undo
     and the spatial tree stay current.
  Cost is `O(region)` and amortizes like the dyntopo per-dab split budget.

- **Demotion (GEOM → VDM)** — the reverse, when a geometry region relaxes back
  to height-field-like (e.g. smoothing removes an overhang). This is *harder*
  (it needs re-projection onto a stable base + UV, the ZBrush "reproject onto a
  clean base" problem) and should be an **explicit / deferred** operation, not a
  live per-dab one. Many pipelines never demote and simply accept that geometry,
  once promoted, stays geometry until an export re-bake.

**Hysteresis is mandatory** to stop the boundary thrashing between carriers:
promote at `α·ρ_min`, demote only well below (e.g. `0.5·α·ρ_min`), so a region
hovering at the threshold doesn't flip every dab.

## 5. The boundary: continuity and feature edges

The seam between a VDM region and a geometry region is the delicate part.

- **C⁰ continuity (no cracks).** The shared border edge-loop is real geometry
  owned by both sides. Its vertex positions are **authoritative**: the VDM side
  feathers its displacement to meet them, and the geometry side pins its
  boundary vertices to the displaced base position. Promote a one-ring "skirt"
  so the transition spans a band, never a single hard edge.

- **Feature-locked remeshing.** Region boundaries (and any UV seams inside VDM
  regions) must be promoted to **dyntopo feature edges** — treated like
  `EDGE_SEAM` / sharp edges via `FeatureViews` — so split/collapse/flip never
  cross them and scramble the parameterization. Without this, a remesh near a
  seam turns the UV (and the baked VDM) to garbage.

## 6. Tangent frames in VDM regions — the static-base dividend

`tangent-displacement-issues.md` §4.5 ruled out a global cross field under live
remeshing: keeping its connection-Laplacian eigensolve current over a churning
mesh would cost more than the remesh. **VDM regions are by construction the part
that is *not* churning** — the base + UV is fixed there. So a smooth,
curvature-aligned cross-field frame is not only affordable in VDM regions, it is
the natural choice, and it can be precomputed once per region and stored as a
per-vertex angle (synchronized for any later bake). Geometry regions carry no
VDM and so need no frame. The two awkward halves of §4.5 land exactly where each
is cheap: cross fields on the static VDM regions, local smoothed frames nowhere
(geometry regions don't sample a VDM at all).

## 7. Cost model — why this avoids the texel-in-tree blowup

| Cost source | Texel-in-spatial-tree | Region hybrid |
|---|---|---|
| Texel store vs. topology churn | re-binned every dab (`O(touched texels)`) | independent of spatial tree; never re-binned |
| GPU buffer regen | full `regen_gpu_node` carries texels too | VDM not in GPU nodes; geometry path unchanged |
| Detail double-storage | texels redundant with dyntopo verts | regions disjoint → never stored twice |
| Per-dab overhead | proportional to texel density | `O(brush region)` splat + occasional promotion |

The only new live cost is (a) the boundary band and (b) promotion events, both
`O(region)` and amortizable. The VDM store stays a flat UV-keyed structure with
the spatial tree untouched, which is the whole point.

## 8. Risks and open questions

- **Boundary thrash** — mitigated by the §4 hysteresis, but the thresholds
  (`α`, `θ_max`, demote margin) need tuning against real strokes.
- **Topological edits inside a VDM region** — displacement is fine, but any
  edit that changes *base* topology requires promotion first (the base UV must
  stay fixed while it is VDM-owned).
- **Promotion bake quality** — sampling `base + VDM` to seed geometry must
  handle texel filtering and UV seams cleanly, or promoted regions show seam
  artifacts.
- **Authoring model** — is the partition purely selector-driven, or also
  user-paintable (force a region to geometry for hand-sculpting, or to VDM to
  keep it light)? Likely both: auto-promote on fold, manual override allowed.
- **Undo / meshlog** — promotion changes topology and must go through meshlog
  like any dyntopo op; VDM edits need their own undo channel (tile deltas),
  distinct from the geometry log.
- **VDM residency** — a virtual-texture store needs tile streaming /
  eviction; sizing it against the dyntopo geometry budget is an open balance.

## 9. Concrete integration sketch (sculptcore terms)

1. `FACE`-domain `int` `.detail.carrier` attribute (`{VDM, GEOM}`), `TEMP` like
   the `.spatial.*` ownership attrs, plus the `CORNER` UV layer.
2. A `VdmStore` keyed by UV (tile pyramid), owned by the brush/sculpt layer —
   **not** by `SpatialTree`. Brush edits splat the dab footprint into UV space.
3. Per-dab selector over the brush region: evaluate §2 from the base shape
   operator (`κ₁,κ₂`) + the current displacement magnitude; collect faces that
   violate the predicate.
4. `promoteRegion(faces)`: subdivide → seed verts from `base + VDM` sample →
   set `.detail.carrier = GEOM` → emit `MeshCallbacks` so spatial + meshlog
   ingest the new geometry.
5. Flag region-boundary and UV-seam edges as dyntopo features so remeshing
   respects them.
6. Precompute a cross-field tangent frame per VDM region on its static base
   (affordable; see §6); store as a per-vertex angle for synchronized bakes.

## 10. Relationship to prior art

This is heterogeneous surface representation: a multires/Ptex-style displaced
base for the stable part, explicit adaptive geometry for the rest, with a
curvature/overhang criterion choosing between them — conceptually adjacent to
feature-adaptive subdivision, REYES displacement bounds, and the
sculptris-pro (dyntopo) vs. HD-geometry vs. displacement split in production
sculpt tools. The contribution here is making the *selector* the literal
offset-surface fold bound, so the partition is principled rather than authored,
and tying it to the base-refit lever so the cheap carrier covers as much surface
as the geometry allows.
