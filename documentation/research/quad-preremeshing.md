# Field-Aligned Triangle Pre-Remeshing for Quad Output

## Question

Would the quad remesher benefit from a preprocessing step that resamples the
input into roughly regular, field-following triangles — built by computing a
rough cross field and then iterating dyntopo remeshing together with a modified
tangential smooth that aligns geometry with that field?

Short answer: **yes, as a preconditioner — but the load-bearing piece is the
edge-flip criterion, not the smoother**, and the marginal benefit is smaller for
our typical input than for arbitrary scanned meshes. This report explains why,
where the step fits relative to the existing
[`quad-remeshing.md`](quad-remeshing.md) MIQ pipeline, and what it would reuse
from `source/dyntopo/`.

---

## Where it helps

Quad-extraction quality is downstream of two things: the cross field and the
sampling of the surface the field lives on. A near-uniform, well-shaped triangle
mesh improves both:

- **Cleaner field estimation.** Curvature / principal-direction estimation is
  far less noisy on isotropic triangles, so the 4-RoSy field built on top is
  smoother and places its singularities more sensibly. Sliver triangles inject
  curvature noise that the field smoothing then has to fight.
- **More robust parameterization / position field.** Both routes — the
  global seamless-parameterization (MIQ) pipeline in `quad-remeshing.md` and the
  local Instant-Meshes-style position-field extraction — are markedly better
  conditioned on a uniform mesh than on one with wildly varying edge lengths.

As a *preconditioner*, field-aware isotropic remeshing is a genuine win.

---

## The correction: align **edges**, not triangles

"Roughly regular triangles that follow the cross field" conflates two goals that
pull in different directions:

- *Regular (near-equilateral) triangles* — what plain Botsch-Kobbelt with a
  **Delaunay** flip test produces.
- *Edges that follow the cross field* — what quad extraction actually wants,
  because the extracted quad edges trace the field's integral lines.

A Delaunay-flipped equilateral mesh has edges at ~60° increments that do **not**
respect a 4-direction field. The decisive lever is therefore the **flip
criterion**, not the smoother. Replace (or augment) the Delaunay test with a
*field-aligned* one: prefer the diagonal whose edge directions best match the
local cross frame. Field-aligned vertex smoothing plus field-aligned flips
together produce triangle meshes whose edges run along the field and whose
triangles pair naturally into quads. Smoothing alone does not get there — it
repositions vertices but leaves connectivity off-field.

### Recipe

1. Build a rough 4-RoSy field (curvature-seeded, a few smoothing iterations).
2. Dyntopo split/collapse for density (curvature-graded — the existing path).
3. **Field-aligned edge flips** — the new core piece.
4. Anisotropic tangential smooth that projects motion onto the field frame
   rather than the isotropic centroid.

This is effectively a from-scratch variant of **Instant Field-Aligned Meshes**
(Jakob et al., 2015). The iterative remesh-plus-field-smooth loop is the same
idea, and its local position-field extraction can even skip the global seamless
parameterization that the MIQ pipeline requires.

---

## Where it does *not* help

The hard part of quad remeshing — globally consistent singularity placement and
a seamless integer-grid parameterization — is dominated by the **field**, not by
the triangulation beneath it. Field-aligned tri-remeshing makes that step more
robust but does not solve it. The remesh loop will not repair a bad singularity
structure; that is a field-design problem and stays the responsibility of the
field stage in `quad-remeshing.md`.

---

## Project-specific weighting

Our input to a quad remesher is typically a **dyntopo sculpt mesh, which is
already near-uniform**. That is precisely the case where an additional
field-aligned tri-remesh pass has the *smallest* marginal benefit — the
isotropy is mostly present already. The payoff concentrates almost entirely in:

- the **field-aligned flip predicate** (step 3), and
- the **anisotropic tangential smooth** (step 4),

both of which reuse the most existing dyntopo machinery. The split/collapse
density loop we already have buys comparatively little extra here.

Given the Catmull-Clark direction in the sculpt-layers work
([`sculpt-layers-design.md`](../sculpt-layers-design.md)), the most leveraged
outcome is a clean field-aligned base mesh whose extraordinary points (the field
singularities) become the SubD control-cage irregular vertices — exactly what a
Catmull-Clark cage wants fed into it.

---

## Relationship to the two pipelines

| Stage | Global MIQ (`quad-remeshing.md`) | Instant-Meshes-style local |
|---|---|---|
| Field | 4-RoSy, curvature + feature seeded | same |
| Pre-remesh (this doc) | optional preconditioner | **part of the core loop** |
| Sampling | parameterization integer grid | position field (local) |
| Extraction | seamless param + quantization (no spirals) | local snapping/merge |
| Singularity guarantee | strong (quantization) | weaker (local) |

For the global pipeline the pre-remesh is a robustness aid bolted in front of
the field stage. For the local pipeline it *is* most of the algorithm. Either
way the new engine code is the same two pieces (field-aligned flip + anisotropic
smooth), so the step is worth prototyping independently of which extractor we
ultimately ship.

---

## Implementation sketch against `source/dyntopo/`

The existing round loop is the Botsch-Kobbelt quartet over maximal independent
sets (split / collapse / flip / smooth) in `source/dyntopo/dyntopo.h`. Two
modifications convert it from isotropic to field-aligned:

- **Field-aligned flip predicate.** Where `do_flips` currently uses a
  length / Delaunay criterion, add a mode that scores each candidate diagonal by
  the angular deviation of its two incident edge directions from the nearest of
  the four cross-field directions at the edge midpoint, and flips toward the
  lower-deviation configuration. Keep a Delaunay tie-break to avoid degenerate
  triangles. This is the only genuinely new geometric kernel.
- **Anisotropic tangential smooth.** The existing tangential smoother moves a
  vertex toward its neighborhood centroid in the tangent plane. Replace the
  isotropic step with one that decomposes the move into the local cross-field
  frame (u, v axes) and weights the two components to pull vertices toward field
  integral lines, then reprojects to the surface (closest-point on the source
  BVH). `do_smooth` already exists as a (default-off) lever.

The cross field itself is per-face or per-vertex state; store it as a builtin
attribute in the same style the MIQ plan uses
(`BuiltinAttr<..., ".remesh.f.theta">`). Field construction (curvature seeding +
a few smoothing iterations) is shared with the MIQ field stage, so it should
live in the eventual `source/remesh/` module rather than in `dyntopo/`.

Both modifications are local, independent-set-friendly, and thread the existing
`MeshCallbacks` so spatial-tree and meshlog currency are preserved exactly as
the current dyntopo path does.

---

## Recommendation

Prototype the **field-aligned flip + anisotropic smooth** pair as a dyntopo
mode, gated behind a rough curvature-seeded cross field, and measure quad-output
quality through the Instant-Meshes-style local extractor first (it needs no
seamless parameterization, so it is the cheapest way to see whether field
alignment is paying off). Treat the full density loop as optional given our
already-uniform input. If the local extractor's quads are good enough, the
heavier MIQ pipeline becomes a quality upgrade rather than a prerequisite.
