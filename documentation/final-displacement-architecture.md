# Final Displacement Architecture — Layers, VDM, and Multires Subsurf

The consolidated architecture for the layered-displacement system: sculpt
layers stored in vertex positions, vector-displacement-map (VDM) layers stored
in UV-mapped or Ptex textures, and Catmull-Clark multiresolution editing. This
report records the decisions made on top of the three earlier design docs and
is the reference the implementation plan
([`plans/displacementAndSubSurf.md`](plans/displacementAndSubSurf.md)) builds
from.

Companion reading (assumed, not re-derived here):

- [`sculpt-layers-design.md`](sculpt-layers-design.md) — the layer-stack
  model: `AttrUse::SCULPT_LAYER` + settings sidecar, DELTA vs TANGENT storage,
  vertex-layers-then-VDM composition, the `α·ρ_min` clamp, the in-house
  discrete Catmull-Clark decision.
- [`dyntopo-vdm-region-hybrid.md`](dyntopo-vdm-region-hybrid.md) — the
  `.detail.carrier` region partition, the eligibility predicate,
  promotion/demotion, why texels must not live in the spatial tree.
- [`tangent-displacement-issues.md`](tangent-displacement-issues.md) — the
  offset-surface fold bound (§6), smooth tangent frames / synchronization
  (§4.5), multires edit propagation (§5).

Also load-bearing: [`spatial.md`](spatial.md) (GPU nodes, requested
attributes, update lifecycle) and, in the app repo,
`documentation/pbvhTexPaint.md` (the UV-space-raster / world-space-eval
texture-paint pattern this design generalizes) and
`documentation/shader-attributes.md` (the requested-attribute contract the
fragment render path rides on).

---

## 1. Decision summary

| Question | Decision |
|---|---|
| Texels in the spatial tree? | **No.** The tree gains per-face displacement *bounds*, tile-edit dirty hooks, and carrier-tag routing — never texel storage (§2). |
| VDM render pipeline | **Fragment-shader application by default** (normal-map-style, no vertex amplification); compute-tessellated true displacement as an **opt-in tier** (§3). |
| Multires representation | **Canonical grids store (implicit topology, disk-backable) + a materialized `mesh::Mesh` for the active edit level.** The single-finest-mesh + skip-iterator model is rejected (§4). |
| Subdivision beyond edited levels | **GPU stencil-table evaluation** (`fine = S · coarse` SpMV in compute), bit-consistent with the CPU discrete-CC surface. B-spline/Gregory patches and Phong-tessellation-style cheap subdivision are rejected (§5). |
| VDM carrier format | Both **UV atlas** and **Ptex** behind one `VdmStore` parameterization seam; atlas first (polygon bases), Ptex when multires lands (§6). |

## 2. The spatial tree carries bounds, not texels

Texture painting (`pbvhTexPaint.md`) proved the decomposition: the tree
answers *"which geometry is under the brush"*, the texture store answers
*"which texels does that geometry map to"*. A VDM sculpt dab is the same
pattern with two changes — the payload is a `float3` displacement instead of a
color, and the world position used for brush falloff is `base + VDM(texel)`,
reconstructed on the fly while rasterizing the dab footprint into UV space
(interpolate the base position across the face, add the texel's current
value). No step in that loop needs a persistent texel→tree binding, and the
hybrid doc's cost table shows why building one is structurally bad (no stable
key under churn, GPU-slice shatter, double storage).

What the VDM *does* impose on the tree is aggregate:

1. **Displacement-bound padding.** A conservative per-face `max|D|` scalar — a
   coarse mip of the VDM magnitude pyramid, FACE-domain, in the spirit of
   `.spatial.f.node` — folded into leaf AABBs during `regen_node_bounds`. This
   is the REYES displacement-bound idea; it keeps `castRay`, frustum cull, and
   screen-circle/rect queries correct against the displaced surface.
2. **A dirty hook from tile edits to leaves.** A dab that splats tiles flags
   the touched faces' leaves `Spatial_RegenBounds`. In the fragment render
   path that is *all* — the visual update is a dirty-tile texture upload, no
   `regen_gpu_node`, no slice rewrites.
3. **Carrier routing.** The dab loop reads `.detail.carrier` per face to
   dispatch vertex-executor vs. UV-splatter.

**Picking.** With the clamp guaranteeing the VDM is small high-frequency
residual, raycasting the base surface inside padded AABBs yields a hit whose
error is bounded by the clamp — acceptable for brush placement. An optional
refinement (short relief-mapping-style march sampling the VDM near the base
hit) can be added behind the same `castRay` API later; it is a CPU
`VdmStore` sample, still no tree residency.

## 3. Two VDM render paths, selected by the predicate we already have

The paths serve different magnitude regimes, and the fold-bound clamp is the
natural selector — the failure cases of normal-map-style shading (large
displacement, overhangs) are exactly the content the clamp/promotion machinery
evicts from the VDM carrier. The two systems reinforce each other; no separate
threshold is needed.

### 3.1 Fragment path (default, live sculpting)

Geometry stays at base/multires resolution. The fragment shader samples the
VDM and applies it as shading only: normal perturbation from the VDM's
derivatives (computed in-shader; a baked companion normal mip is a fallback if
profiling demands it), optionally parallax offset at mid magnitudes.

- WebGPU has no tessellation stage, so this is also the path of least
  resistance.
- The requested-attribute contract carries everything needed with no new
  machinery: UV (CORNER domain) already flows; the smooth tangent frame is one
  more requested per-vertex attribute (cross-field azimuth + smoothed normal),
  interpolated and re-orthonormalized per pixel. Frame synchronization
  (bake ≡ render) holds by construction because brush and shader read the same
  stored attribute (§7).
- Per-dab cost is a dirty-tile texture upload — dramatically better brush
  feel than any geometry-amplifying path.
- Accepted artifacts: depth/shadows/AO see the undisplaced surface. At clamped
  magnitudes this is the same bargain normal maps make everywhere.

### 3.2 Tessellated path (opt-in tier)

Compute-shader vertex generation toward texel density into **transient**
buffers + indirect draw; sample the VDM per generated vertex, displace, draw
real triangles. Correct silhouettes/depth/shadows, and the only way to render
overhang-bearing VDM content (relevant mostly to VDM stamp brushes). Two hard
rules:

- The amplified vertices are render-side only — they **never** enter the mesh
  or the spatial tree (that would be texels-in-tree with extra steps). Content
  that must become editable geometry goes through promotion instead.
- On a multires base the amplification *is* the §5 stencil evaluation, so the
  two features share one compute path.

## 4. Multires: canonical grids store + materialized active-level mesh

### 4.1 The decision

The multires stack's canonical form is a **grids store**: per-base-face
regular grids with implicit topology, holding per-level frame-relative
`float3` displacement arrays (the discrete multires model from
`sculpt-layers-design.md` §4.2). Editing happens on a **materialized
`mesh::Mesh` of the active level** — an evaluation cache built from cached
Catmull-Clark stencils — so the entire existing stack (spatial tree, sbrush
executor, meshlog, requested attributes, draw) applies unchanged. Multires is
a *base provider* underneath the layer compositor, not a parallel geometry
system.

- **Level switch** = materialize the target level + build its tree. Recently
  used level meshes/trees are kept resident (LRU; users toggle two levels
  constantly) and evicted under memory pressure.
- **Stroke end** writes the level mesh's positions back into the store
  (re-expressed in the level's tangent frames) — the same
  refresh-at-`endStep` pattern meshlog already uses for created verts.
- **Finer-than-edit-level display** comes from the §5 GPU stencil
  amplification, not from CPU materialization.
- **Disk backing** falls out: the store is flat per-level arrays (mmap /
  page / LZ4-friendly, same shape as the autosave split-serialization work);
  the only explicit mesh in RAM is the level being edited.

### 4.2 The rejected alternative: one finest-level mesh + skip iterator

The brush executor is deliberately mesh-structure-agnostic, so a single mesh
at the finest level with a vertex iterator that skips verts outside the
current level is expressible. Rejected for three structural reasons:

1. **Neighborhoods, not iteration, are the hard part.** `for_neighbor` is a
   first-class sbrush construct (one-ring walk on CPU, CSR on GPU), and every
   smooth/fair/relax/boundary kernel depends on it. On a finest mesh, a
   level-L vert's one-ring is entirely finer-level verts a hair's breadth away
   on the composited surface; its true level-L neighbors are `2^(F−L)`
   edge-hops away. A smooth kernel through the skip iterator averages against
   the wrong ring and acts at the wrong frequency (≈ no-op). Fixing that
   requires a per-level adjacency overlay — a virtual level-L mesh with extra
   steps. (Note: in a *grids* structure the level-L neighbor is O(1) stride
   indexing — an argument for the grids store, not for the explicit finest
   mesh.)
2. **The memory floor defeats disk backing.** A materialized finest
   `mesh::Mesh` pays explicit edges/corners/faces/cycles/pool slots plus
   `.spatial.*` attrs at finest density, permanently — a 10k cage at 6 levels
   is ~40M faces of explicit topology — and it is the *hot* structure (tree,
   meshlog, draw all point into it), so it is the one thing that cannot be
   paged. The cold per-level arrays it could page are not where the memory
   went.
3. **Per-dab cost scales with display density, not edit density.** Display is
   always finest, so a coarse-level dab must recompose all finer in-region
   verts on the CPU and push a finest-density spatial/GPU update per dab. In
   the chosen model dab cost scales with the edit level and the fine preview
   is GPU work.

What the skip model legitimately buys — instant level switch, always-finest
display — is recovered by the level-mesh LRU cache and the GPU amplification
respectively. The residual risk is bulk spatial-tree build time at high vert
counts (see `dyntopo-m7-cascade.md`: ~68 s full rebuild at 5M vs 2–11 ms
incremental); a fast bulk-build path is a prerequisite tracked in the plan.

### 4.3 The grids-direct reserve path

The executor's structure-independence is held in reserve, not spent up front:
a grids-backed vertex-iterator factory (O(1) stride neighbors) could later run
position-only or even `for_neighbor` kernels directly on the store for
coarse-level edits with no materialization at all. It is not the primary plan
because spatial, meshlog, and the GPU attr-fill are all `mesh::Mesh`-bound
today — the executor's genericity does not cover the expensive part. It is the
escape hatch if level materialization ever proves too slow.

## 5. Subdivision beyond the edited levels: GPU stencil tables

For giving a VDM (or a fine-display preview) more geometric density than the
user has edited, three options were evaluated:

1. **B-spline patches on the GPU** — after one CC step regular quads are
   bicubic B-splines, but extraordinary vertices need Gregory-style patches
   and crack-free transition handling: the OpenSubdiv-shaped machinery the
   layers doc deliberately deferred. Worse, a render-side *analytic* surface
   diverges from the CPU's *discrete*-CC surface — a bake/render
   synchronization break. Rejected; reconsidered only if adaptive
   view-dependent density becomes a requirement.
2. **Stencil-table evaluation (chosen).** Locked topology makes "subdivide N
   more levels" a static sparse matrix: `fine_pos = S · coarse_pos`. Upload
   the table once, run an SpMV-style compute pass (cacheable across frames
   when the coarse level is unchanged), then a small second pass builds the
   smoothed frame at fine verts and applies the VDM. Bit-consistent with the
   CPU surface — same stencils, generated by the same refiner module — so no
   synchronization break, and it is exactly the regular bandwidth-bound work
   WebGPU compute is good at.
3. **Cheap local subdivision + smoothed tangent frame** (Phong tessellation /
   PN-triangles). Cheaper per vertex (no neighbor gathers) but defines a
   *different* smooth surface, desynchronizing VDM frames/magnitudes from what
   was authored. Per `tangent-displacement-issues.md` §4.5, synchronization is
   the load-bearing requirement — and the savings over a cached SpMV are
   small. Usable only where the VDM is not applied (e.g. a distant-LOD smooth
   preview).

## 6. VdmStore

The UV/Ptex-keyed tile store, owned by the sculpt-layer system, never by
`SpatialTree`:

- **Parameterization backends** behind one `sample(face, u, v)` seam: a
  corner-UV **atlas** (reuses the existing `AttrUse::UV` CORNER layer) and
  **Ptex** per-face grids + adjacency table. Realtime Ptex = tile array +
  per-face offset table in a storage buffer + copied one-texel border skirts
  so bilinear filtering is seamless. Atlas ships first (polygon bases); Ptex
  when multires lands, where the face↔patch identity pays.
- **Pyramids**: mip chain for sampling, plus a magnitude-bound pyramid whose
  coarsest levels feed the per-face `max|D|` bounds of §2.
- **Undo**: its own tile-delta channel, bracketed inside the same MeshLog step
  as the dab's vertex/topology edits so one undo press reverts a whole dab.
- **GPU residency**: dirty-tile uploads; the fragment path binds the tile
  array + face table directly.
- **Serialization**: image/tile container, sharing the compression path
  autosave established.

## 7. The frame provider

One module owns the smooth tangent frame: smoothed vertex normal + cross-field
azimuth angle, computed on static bases (VDM regions, the subsurf cage — the
static-base dividend of the hybrid doc §6) and stored as a per-vertex
attribute. It is the single synchronization anchor: the brush splatter
(inverting world-space edits into tangent texels), promotion bakes, the
fragment shader, and the stencil-amplification pass all read this one field.
Never derive a frame two different ways on two sides of a bake.

## 8. The architectural layers

Bottom to top:

1. **Base providers** — polygon `mesh::Mesh` (dyntopo-live) and the multires
   module (`source/subdiv/`: cached-stencil uniform CC refiner, grids store,
   level materialization, explicit down-refit op).
2. **VdmStore** (§6).
3. **Frame provider** (§7).
4. **Layer compositor** — the evaluation core from `sculpt-layers-design.md`:
   orders vertex layers then VDMs, applies frames, enforces the `α·ρ_min`
   clamp, owns `.detail.carrier` + promotion/demotion. Produces evaluated
   vertex positions (what the tree/meshlog see) and per-face displacement
   bounds (what the tree pads with). The clamp/predicate state lives here and
   only here; brush (promotion trigger), compositor (clamping), and renderer
   (path selection) all read this one source of truth.
5. **Spatial tree** — structurally unchanged; gains bound-padded AABBs,
   tile-dirty hooks, carrier-tag reads. Gains no texels.
6. **Brush/dab dispatch** — per-face routing: vertex carrier → existing sbrush
   executor; VDM carrier → texpaint-style UV-space splatter with world-space
   falloff. One MeshLog step brackets vertex edits, topology edits, and VDM
   tile deltas.
7. **Render layer** — path A (fragment, default) and path B (compute
   amplification, opt-in), selected per region from the compositor's predicate
   state; the multires fine-display preview is path B's stencil pass reused.

## 9. Open decisions

Carried forward from `sculpt-layers-design.md` §11 (composition-model
confirmation, DELTA-default confirmation, whole-mesh VDM vs. mandatory region
partition, blend semantics) plus, new to this report:

1. **In-shader VDM normal derivation vs. baked companion normal mips** —
   start in-shader (cannot desync, no second store); bake only if profiling
   demands.
2. **Bulk spatial-tree build** — fast path or per-level tree cache policy for
   finest-level editing (§4.2 risk).
3. **Grids-store granularity** — per-base-face grids (Ptex/Blender-style,
   pairs with quad cages) vs. per-quadrant grids after one CC step (handles
   n-gon cages uniformly). Leaning per-quadrant-after-one-split, matching the
   Ptex `__faceindex` convention.
4. **Level-mesh LRU budget** — how many materialized levels/trees to retain,
   and whether eviction is size-based or count-based.
