# PTex and Vector Displacement: Production Usage, Edit Propagation, and the Offset‑Surface Stability Problem

## 1. What Ptex is, and why displacement lives on it

Ptex (Disney, Burley & Lacewell 2008) stores a separate small texture **per face** of the control mesh, plus a compact **adjacency table** (for each face edge, the neighboring face and its rotation). There is no global UV layout, no atlas, no seams to lay out by hand, and no wasted atlas gutter. Filtering that crosses a face boundary walks the adjacency table into the neighbor's texel grid, so the per‑face decomposition is invisible after filtering. Each face carries its own mip pyramid.

That "no UVs, per‑face" property is exactly what makes Ptex the default carrier for **displacement** in production: a sculpt produces one fine‑detail value (or vector) per surface sample, and Ptex lets you store that sample‑for‑sample against the cage face it belongs to, with correct cross‑face filtering and no UV‑seam cracks in the displaced result. The cost it removes — manual UV unwrapping and seam management — is precisely the cost that dominates a hand‑authored displacement pipeline.

Two encodings matter here:

- **Scalar (height) displacement** — one channel `d`; the surface moves along its own shading normal: `p' = p + d·n`.
- **Vector displacement (VDM)** — three channels; the surface moves by a full 3‑vector. This is what lets a displacement create **overhangs and undercuts** (folds of skin, mushroom caps, ears), which scalar height displacement *cannot* represent because it can only push along `±n`.

## 2. Ptex displacement on simple (polygon) surfaces

On a plain polygon mesh the per‑face Ptex texel grid maps to the face's own `(s,t)` parameterization. The displacement vector, if tangent‑space, is expressed in a frame `[t, b, n]` built from that face's geometry. This works, but it has two structural weaknesses that drive everyone toward subdivision as the displacement base:

1. **The tangent frame is only as smooth as the base shading.** A faceted low‑poly base gives a frame that jumps at every edge; a tangent‑space VDM baked against it shows faceting/seam artifacts unless the renderer reconstructs a smooth interpolated frame.
2. **A polygon base is a poor low‑frequency carrier.** Displacement has to make up *all* the smooth curvature the coarse mesh lacks, which means large displacement magnitudes — and large magnitudes are exactly what triggers the stability failure in §6.

So in practice "simple surface" displacement is mostly used for low‑relief detail (skin pores, fabric weave) where displacement stays small, or as a baking target that will be rendered as a subdiv anyway.

## 3. Ptex displacement on subdivision surfaces — the natural marriage

This is where Ptex is genuinely at home, and it's the dominant production path (RenderMan, OpenSubdiv, Arnold, Mudbox/ZBrush export targets).

The key structural fact: for a **Catmull‑Clark** cage, after one subdivision step every face is a quad, and each base face owns a unit `[0,1]²` patch domain. **Ptex's per‑face square is identical to the subdivision patch's parameter domain.** One Ptex face ↔ one base face ↔ one limit‑surface patch. RenderMan wires this with a per‑face `__faceindex` (a face‑varying integer; non‑quad base faces are numbered per‑vertex after the first split), so the renderer knows exactly which Ptex face to evaluate at each shading point. (See RenderMan's Per‑Face Textures and Subdivision Surfaces docs.)

Two things fall out of this that solve the §2 weaknesses:

- **The tangent frame comes from the limit surface, not the cage.** Subdivision gives you analytic first derivatives `∂p/∂u, ∂p/∂v` of the *smooth limit surface* everywhere except extraordinary points (where they're handled by the patch scheme — Gregory patches in OpenSubdiv). That frame varies smoothly across face boundaries, so a tangent‑space VDM reconstructs without faceting. This is the "displaced subdivision surface" representation of Lee, Moreton & Hoppe (SIGGRAPH 2000): a smooth subdivision base plus a scalar/vector detail field sampled in the limit‑surface frame.
- **The base already carries the low frequencies.** The limit surface is smooth and curved, so the displacement only has to encode the residual *high‑frequency* detail. This keeps magnitudes small, which is the stability lever (§6).

The canonical VDM bake against a subdiv base is literally: subdivide to the high level, take the high‑level limit point and the low‑level limit point under the same `(u,v)`, and **store the difference as the per‑texel vector** in the Ptex face. RenderMan describes exactly this ("for each base face, a 3‑channel float displacement texture; the difference of the two limit points is stored as a vector displacement"). Because it's stored against the smooth limit parameterization, it re‑applies cleanly at any dicing rate.

## 4. Tangent‑space choices and why they exist

VDM exporters expose a coordinate‑space choice, and the choice is entirely about deformation stability:

- **Object / world space VDM** — the stored vector is absolute. Cheapest and most stable numerically (no frame reconstruction), but it is **only valid for the rest pose**. Once the mesh deforms, an object‑space vector points the wrong way. Used for static props / non‑deforming renders. (ZBrush "World" mode; Mudbox "Object Space".)
- **Tangent‑space VDM** — the stored vector is expressed in the per‑sample `[t,b,n]` frame, so it rotates with the surface and survives animation/deformation. Required for anything that deforms. Mudbox splits this further:
  - **Relative tangent** — frame from the face's own normal/tangent/binormal; intended for re‑stamping inside the sculpt app (brush/stencil VDMs).
  - **Absolute tangent** — a consistent, smoothly interpolated frame intended for a deforming render; this is the one that behaves under animation.

ZBrush's VDM **brushes/stamps** are the relative‑tangent case taken to its logical end: the VDM is authored against a *flat* reference plane and stamped onto the surface through the local frame, which is why a single VDM alpha can drop a whole ear or barnacle (overhang included) anywhere on the model.

## 4.5 Smooth tangent frames (synchronized bases and cross fields)

§3 got a smooth frame for free from the subdivision limit surface. On a **plain polygon mesh** with a UV‑mapped (non‑Ptex) VDM there is no limit surface to differentiate, so the tangent basis has to be built from the mesh + UVs, and this is where tangent‑space VDM is delicate. Two largely separate concerns: getting the frame *consistent* (synchronization), and getting it *smooth* (cross fields).

### Synchronization is the load‑bearing requirement

A tangent‑space map — normal *or* vector — is only correct under the **exact** frame it was inverted against. The standard per‑vertex basis is the classic one: per‑triangle tangent/bitangent from UV derivatives (Lengyel), area/angle‑averaged at shared vertices, normalized, Gram–Schmidt'd against the smoothed vertex normal, with a handedness sign; at render time it's barycentrically interpolated and re‑orthonormalized per pixel. The averaging *is* the smoothing — nobody pre‑blurs the frame as a texture. What makes it work is that baker and renderer use a **bit‑identical** basis (this is what MikkTSpace standardizes). Smooth the tangents one way at bake and another at render and you get cross‑hatch / seam artifacts.

VDM is **more** sensitive than a normal map, because a normal map encodes only a rotation (unit direction) while a VDM encodes direction **and magnitude**. Consequences pipelines handle explicitly:

- **Normalize the basis** (orthonormal `[t,b,n]`) and keep displacement magnitude in world/object units, so UV stretch rotates the displacement but doesn't shear/scale it. Let the basis carry UV‑space length and a stretched island scales the bump.
- **Object‑space VDM is preferred whenever the mesh doesn't deform** — no frame to mis‑sync. Tangent‑space VDM is reserved for deforming assets and is where the ZBrush↔renderer "displacement is flipped/sheared" pain historically lives.
- The real pre‑smoothing that *does* happen is on the **cage normals used for bake projection** (xNormal/Substance averaged‑cage / smooth‑normal ray casting) — that smooths the *projection direction*, not the stored field. Some engines also bake a smoothed tangent frame down to a **vertex attribute** once, offline, purely to guarantee bake/render agreement.

### A cross field for a smoother in‑plane tangent

The only noisy part of the frame is the **azimuth of `t`** (its rotation about `n`); `n` is already the smoothed vertex normal. A **cross field** replaces the UV‑derived azimuth with the smoothness‑optimal one, decoupled from the parameterization. The simple construction (Knöppel–Crane–Pinkall–Schröder, *Globally Optimal Direction Fields*, 2013):

- Represent the cross as a unit complex per vertex in a local tangent frame, **raised to the 4th power** to kill the 90° ambiguity: `ψ_i = e^{i·4θ_i}`, θ measured from an arbitrary in‑plane reference `r_i`.
- Across each edge the references differ by a parallel‑transport angle `ρ_ij` (discrete Levi‑Civita connection). Minimize the connection‑Dirichlet energy `E = Σ_ij w_ij |ψ_j − e^{i·4ρ_ij} ψ_i|²` with `|ψ_i| = 1`. Unconstrained‑then‑normalize, this is the **smallest eigenvector of a Hermitian connection Laplacian** — one sparse eigensolve. Recover `θ_i = arg(ψ_i)/4`, `t_i = cosθ_i·r_i + sinθ_i·(n_i×r_i)`, `b_i = n_i×t_i`. (Cheaper still: heat‑diffuse the 4th‑power field from a curvature seed.)
- Optional alignment term pulls `ψ_i` toward the principal‑curvature cross (shape‑operator eigen‑directions, weighted by `|κ₁−κ₂|`), so the frame follows curvature where pronounced and stays smooth where flat.

What it buys: a **UV‑independent**, interior‑`C∞` tangent that pairs especially well with **Ptex** — Ptex gives per‑face storage but per‑face `(s,t)` has no cross‑face orientation continuity, and the cross field supplies exactly that with no UVs. The three catches, worst‑biting first:

1. **Singularities are topologically unavoidable.** Total index `= χ` (Poincaré–Hopf); on a sphere, e.g. eight index‑`+1/4` cowlicks. You don't remove the discontinuity, you **concentrate it into isolated points** (and the cut‑paths joining them) instead of smearing it along UV seams. Around each, the cross winds by a multiple of 90°.
2. **A cross defines `t` only up to 90°.** Fine for an *unoriented* anisotropy direction, but a VDM stores **signed** `t`/`b` components, so you must comb the 4‑RoSy into a signed vector frame — reintroducing a branch choice and **transition functions / period jumps** across the cuts. Taken to its end this *is* seamless / field‑aligned parameterization (the quad‑remeshing problem). In practice you place singularities in low‑displacement or hidden regions and live with the transitions.
3. **Synchronization still rules.** Bake and render must use the identical field, so store the per‑vertex angle (or recompute deterministically) — same constraint as MikkTSpace, on a nicer field.

The honest limit: a cross field fixes **framing continuity** (kills UV‑induced azimuthal noise, gives a curvature‑aligned basis) but does **nothing for `ρ_min`**. The offset‑surface fold (§6) is geometric — about the base's curvature radius, not how the tangent plane is oriented. A smoother frame makes the displacement *direction* well‑behaved; it cannot make a large displacement near a crease stable. The two stay separate problems.

## 5. Propagating edits between subdivision levels

This is the multiresolution problem, and every sculpt package solves it with the same core idea: **a detail pyramid where each level stores a displacement relative to the smoothed version of the level below, expressed in a local frame.** (Zorin/Schröder/Sweldens multiresolution editing; Guskov et al. successive displacements; Lee et al. displaced subdivision.) Because each level's detail is a *local‑frame offset from the smoothed lower level*, edits decompose by frequency:

- **Sculpting at a high level** writes into that level's detail vectors. Lowering the level and looking at the broad shape, the high‑frequency bumps are encoded as detail relative to the (still smooth) lower level — so they ride along when you later move the lower level. This is the everyday ZBrush behavior: "go to a low level, push the big forms, return to the high level and all the fine bumpiness is still there." The fine detail is stored as a frame‑relative offset, so moving the coarse cage carries it.

- **Propagating a high‑level edit *down* to a lower level** is the harder direction and is done by **reprojection / refitting**, not by algebraic inversion (you can't uniquely un‑subdivide an arbitrary fine edit). The operations, by package:
  - **Blender Multiresolution** — *Sculpt Base Mesh* deforms the base while previewing higher‑level displacement; the **Reshape**/reproject path transfers the displaced shape's low frequencies back onto a chosen level. Baking takes the **delta between the Viewport (low) level and the Render (high) level** — i.e. only the difference is ever baked, never the absolute shape.
  - **ZBrush** — *Reconstruct Subdivision* rebuilds lower levels from the lowest current level (only succeeds on clean all‑quad Catmull‑Clark meshes; fails after booleans/triangles). *Project All* / **Reproject Lower Levels** re‑projects existing high‑frequency detail onto a base you've re‑edited or re‑topologized, recomputing the per‑level detail against the new lower geometry.
  - **Mudbox** — sculpt at a low subdivision level; higher‑level detail rides on top through the same level stack.

The invariant in all of them: **only the per‑level *difference* is stored/baked**, and the map you ship represents `(high‑level limit) − (low‑level limit)` under a shared parameterization. Matching the bake's low level to the render's subdivision level is the standard correctness rule ("match subdiv levels so only the difference is mapped").

## 6. The numerical‑instability problem — and how the base geometry is updated to avoid it

The line‑bending analogy is exactly right, and it has a precise classical statement.

### The math

Offsetting a surface along its normal, `p'(u,v) = p(u,v) + d·n(u,v)`, is a smooth, injective map **only while `d` stays inside the local radius of curvature.** The locus of principal‑curvature centers is the **focal surface** (the surface analogue of a curve's evolute). Push the offset past a focal sheet and neighboring normals cross — the offset surface **folds, cusps, and self‑intersects**. The standard condition (Patrikalakis–Maekawa–Cho; the NURBS‑offset literature) is:

> A signed offset of distance `d` self‑intersects locally wherever `|d| > ρ_min`, i.e. `|d| > 1/|κ_max|`, where `κ_max` is the largest‑magnitude principal curvature in the concave‑toward‑offset direction.

That's the surface version of "bend a straight line into a tight curve and its normals intersect": for a curve, the offset develops a cusp exactly at `d = ρ`. A scalar (height) displacement map *is* a normal offset, so it inherits this bound directly. A **tangent‑space vector displacement** is a generalized offset — `p' = p + [t,b,n]·V` — and it fails the same way for two compounding reasons: (a) the normal component of `V` is a literal offset and folds when it exceeds `ρ_min`; (b) the frame `[t,b,n]` itself rotates with base curvature, so when the base curves tightly under a large `V`, the Jacobian of `p ↦ p + T(u,v)·V(u,v)` loses rank and the displaced sheet self‑intersects even for purely tangential `V`. In both cases the trigger is **displacement magnitude approaching the base's local curvature radius.**

### How the base mesh is updated to stay out of that regime

The production answer is not "clamp the displacement" — it's **keep the displacement high‑frequency and move the low frequency into the base geometry so that `|V| ≪ ρ_min` everywhere.** Concretely:

1. **Refit/reproject the broad shape into the base (control cage), not the map.** When a sculpt introduces large smooth form, that form is pushed *down* into the subdivision cage (§5: Blender *Sculpt Base Mesh*/Reshape, ZBrush *Reproject Lower Levels*, sculpting at a low Mudbox level). The limit surface then tracks the sculpt's low frequencies, its curvature radius `ρ_min` grows to match the real shape, and the residual left in the Ptex/VDM map is small high‑frequency detail that comfortably satisfies `|d| < ρ_min`. **This is the literal answer to "how is the base mesh geometry updated":** you re‑fit the cage so the smooth carrier absorbs everything that would otherwise be a dangerously large offset, leaving the map to carry only what's safe.

2. **Use vector displacement instead of scalar where the surface curves or overhangs.** A fixed per‑texel vector tolerates more base bending than a pure normal offset (it isn't forced to ride the instantaneous, fast‑rotating normal), and it's the *only* option for genuine overhangs — a normal offset can't even represent them, let alone do so stably. The VDM is still defined in a tangent frame, so it doesn't escape the bound, but it raises the magnitude you can survive before folding.

3. **Reconstruct the tangent frame from the *smooth limit surface*, not the faceted cage.** Discontinuous per‑face frames manufacture artificial cusps at every face boundary (a frame discontinuity is a zero‑radius "bend"). Deriving `[t,b,n]` from subdivision limit derivatives gives a `C¹`‑ish, smoothly varying frame, so the only curvature the displacement has to respect is the real surface curvature — this is exactly Mudbox's *Absolute Tangent* (smoothly interpolated frame for deformation) versus *Relative Tangent*, and the displaced‑subdivision‑surface representation's reason for sampling detail in the limit frame.

4. **Bound and pad displacement for the renderer.** Ray‑traced displacement (RenderMan dicing, Arnold) requires a conservative displacement **bound** so the BVH/grid can be padded; the same bound is the natural place to **clamp** pathological texels. This is a safety net, not the primary fix — the primary fix is (1).

The throughline: the offset‑surface fold is governed by `|displacement| vs ρ_min`. You don't fight it by limiting how much an artist can sculpt; you fight it by **continuously re‑absorbing the low‑frequency part of the sculpt into the subdivision base** so that what remains as a tangent/vector displacement is always small relative to the base's curvature radius — and by making the frame that displacement rides on as smooth as the limit surface itself.

## 7. End‑to‑end, the way a studio actually runs it

1. Model a clean **all‑quad Catmull‑Clark cage** (quad topology is what makes Ptex‑face ↔ subdiv‑patch identity and reconstruction work).
2. Sculpt in a **multiresolution stack**; broad form is pushed into low levels (and ultimately the cage), fine detail accrues at high levels as frame‑relative detail vectors.
3. **Bake the delta** between a chosen low level and the high level into a **Ptex VDM** (per‑base‑face, vector, tangent‑space for deforming assets), storing `(high limit − low limit)` in the limit‑surface frame.
4. Render the **cage as a subdivision surface**, evaluate the Ptex VDM at dice rate against the smooth limit frame, with a conservative displacement bound.
5. When form changes, **reproject** detail onto the re‑edited base rather than re‑authoring the map — keeping displacement magnitudes high‑frequency and below the curvature‑radius bound.

## 8. Implications for this engine (sculptcore)

The design above is the "static base + detail field" dual of what sculptcore's dynamic topology already does. The dyntopo path keeps the surface valid by *adding real geometry* where detail exceeds what the current tessellation can carry — the topological equivalent of the §6 fix (move detail into geometry, not into an ever‑larger offset). If a displacement/VDM export or a multires mode is added later, the load‑bearing invariants are:

- a quad‑dominant base for Ptex‑face identity;
- sampling detail in a **limit‑surface‑derived frame** (not per‑face);
- gating baked displacement magnitude on the local `ρ_min = 1/|κ_max|` of the base, and **re‑fitting the cage** (not clamping the map) whenever an edit's low‑frequency component would push a texel past that bound.

---

## Sources

- [Ptex: Per‑Face Texture Mapping for Production Rendering (Burley & Lacewell, Disney)](https://www.researchgate.net/publication/220506313_Ptex_Per-Face_Texture_Mapping_for_Production_Rendering)
- [RenderMan — Per‑Face Textures (Ptex)](https://renderman.pixar.com/resources/RenderMan_20/ptexture.html)
- [RenderMan 24 — Subdivision Surfaces](https://renderman.atlassian.net/wiki/spaces/REN24/pages/21758634/Subdivision+Surfaces)
- [Displaced Subdivision Surfaces — Lee, Moreton, Hoppe (SIGGRAPH 2000)](https://hhoppe.com/dss.pdf)
- [Mudbox — Extract a Vector Displacement Map](https://download.autodesk.com/global/docs/mudbox2013/en_us/files/GUID-FF69D15F-7974-4B7E-BEB8-9CFA7BBA7299.htm) and [VDM Extraction options (Relative/Absolute/Object tangent space)](https://download.autodesk.com/global/docs/mudbox2013/en_us/files/VDM_Extraction_options.htm)
- [ZBrush / Maxon — Vector Displacement Maps (Tangent vs World)](https://help.maxon.net/zbr/en-us/Content/html/user-guide/3d-modeling/exporting-your-model/vector-displacement-maps/vector-displacement-maps.html)
- [ZBrush — Subdivision Levels (Reconstruct Subdivision, detail transfer)](https://help.maxon.net/zbr/en-us/Content/html/user-guide/3d-modeling/modeling-basics/subdivision-levels/subdivision-levels.html) and [Reconstruct Subdivision discussion](https://www.zbrushcentral.com/t/reconstruct-subdivision-levels/285052)
- [Blender — Multiresolution Modifier (Sculpt Base Mesh, Reshape)](https://docs.blender.org/manual/en/latest/modeling/modifiers/generate/multiresolution.html) and [Baking from multires data](https://code.blender.org/2011/06/baking-from-multires-data/)
- [Arnold — Vector Displacement](https://docs.arnoldrenderer.com/display/A5AFMUG/Vector+Displacement)
- [Self‑intersection of offsets of parametric surface patches (Patrikalakis–Maekawa–Cho, MIT)](https://web.mit.edu/hyperbook/Patrikalakis-Maekawa-Cho/node228.html) and [Computing non‑self‑intersecting offsets of NURBS surfaces](https://www.sciencedirect.com/science/article/abs/pii/S0010448501000811)
