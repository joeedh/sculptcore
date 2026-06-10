#pragma once

#include <cstdint>

/* Tier 9 input pre-remesh primitives. The field-aligned tangential smooth lives
 * here (rather than in remesh.cc) because it needs both the dyntopo operators and
 * the cross field; the convergence driver (9b) and pipeline glue join it later. */

namespace sculptcore::mesh {
struct Mesh;
}
namespace sculptcore::dyntopo {
struct DynTopoTrace;
}

namespace sculptcore::remesh {

/* Tier 9c feature classification: tag `m`'s open-boundary / non-manifold edges
 * and dihedral-sharp edges (face-normal angle > @p sharp_angle, radians) into the
 * mesh::boundary EDGE_SHARP overlay, then recompute the per-vertex class so the
 * feature-preserving dyntopo path (preserve_features) pins them. Mirrors the
 * cross-field's own boundary+dihedral classification (feature_tag.cc); writes the
 * persistent .boundary.* overlays the pre-pass's BK / smooth read. Idempotent —
 * reclassifies every edge from current geometry, so it tracks the evolving mesh. */
void classifyFeatures(mesh::Mesh &m, float sharp_angle);

/* Botsch-Kobbelt remesh pass: drive `m` toward edge length `L` using dyntopo's
 * split-long / collapse-short / flip / tangential-smooth quartet over a single
 * whole-mesh sphere. Geometry only by default.
 *
 * @p size_attr optional per-vertex FLOAT attribute name holding a relative size
 * scale s(v) (1 = nominal). When present each edge's split/collapse band is
 * scaled by the mean endpoint s — refine where s < 1, coarsen where s > 1 (a
 * curvature size field). null = uniform target `L` everywhere (default).
 *
 * @p preserve_features pins the mesh::boundary feature edges/verts (caller must
 * have run classifyFeatures first): feature verts don't collapse/flip/smooth, and
 * a feature edge collapses only along its own collinear curve. Also disables BK's
 * internal tangential smooth (fresh split midpoints aren't yet classified, so it
 * would drift them off a crease) — the feature-preserving caller relaxes with its
 * own pinned smooth instead. Default false = the plain geometry-only decimation.
 *
 * @p trace optional granular per-round quality trace (dyntopo_trace.h); the dab
 * appends one RoundQuality per round. null (default) = no tracing, zero cost. */
void bkRemeshToTarget(mesh::Mesh &m, float L, uint32_t seed,
                      const char *size_attr = nullptr,
                      bool preserve_features = false,
                      dyntopo::DynTopoTrace *trace = nullptr);

/* Tangential smooth blending isotropic and field-aligned relaxation.
 *
 * @p align in [0,1]: 0 is the classic isotropic one-ring-centroid relaxation
 * (one-ring centroid → Newell-plane tangent projection → edge-scale clamp); 1
 * steers the move with the per-face cross field (.remesh.f.theta) so vertices
 * relax toward straightened u/v isolines. Intermediate values lerp the two target
 * centroids. A tangent-plane projection + edge-scale clamp safety rail bounds the
 * move in both. A vertex where the field engages projects against the vertex fan
 * normal (sum of incident-triangle normals), which stays on the surface for a
 * flat-face vert whose 1-ring reaches across a crease — the ring Newell tents
 * there and would let the aligned target push it off-surface. Field-less verts
 * (including all of align == 0) project against the ring-polygon Newell normal:
 * the classic relaxation, byte-identical. With align > 0 the field is read from
 * .remesh.f.theta; where a vertex has no field (attr absent or no incident face
 * contributed) it degrades exactly to the isotropic move at that vertex.
 *
 * @p fold_guard cancels any per-vertex move that NEWLY folds the vertex fan (a
 * tri flipping against the fan normal, or an unfolded adjacent fan-tri pair
 * creasing past 90°); already-folded fans stay free to relax flat. Off by
 * default — it trades smoothing progress for fold safety and breaks the exact
 * iso/field equivalence contracts, so only the 9b driver opts in. */
void tangentialSmooth(mesh::Mesh &m, int iters, float lambda, float align,
                      bool fold_guard = false);

/* Tier 9b convergence-driver parameters. The driver runs entirely on its mesh
 * argument; the pipeline maps RemeshParams onto this in Tier 9d. Defaults match
 * the plan's param table; @p target must be set (> 0) by the caller — 0 is the
 * driver's no-op guard, not "use target_edge_length" (that resolution is the
 * pipeline's job). */
struct PreRemeshParams {
  int iters = 5;             // outer convergence iterations
  float target = 0.0f;      // base pre-pass edge length; <= 0 ⇒ no-op
  bool density = true;      // grade the BK band by a per-vertex curvature size field
  /* Bounded-gradation cap on the size field (Tier 3b): the goal length may grow
   * by at most (1 + gradation) per edge hop, applied to the density before every
   * BK pass. A steep size step between neighboring verts makes BK split/collapse
   * pathologically at the cliff (out-of-band edges it can never settle), so this
   * is on by default; 0 disables (A/B only). Typical range 0.3–1.0. */
  float gradation = 0.5f;
  int gradation_iters = 10;
  float align = 1.0f;       // isotropic(0) ↔ field-aligned(1) smooth blend
  int field_cadence = 2;    // recompute the rough cross field every N outer iters
  int bootstrap_iters = 2;  // isotropic denoise sweeps before field-aligned begins
  uint32_t seed = 1u;
  int smooth_iters = 5;        // inner field-aligned smooth sweeps per outer iter
  float smooth_lambda = 0.5f;  // per-sweep relaxation factor
  float density_min = 0.25f;   // size-field clamps (mirror Tier 3) when density=true
  float density_max = 4.0f;
  /* Early-out: stop once an outer iter's field-aligned smooth moves every vertex
   * less than converge_eps · target. 0 = run all `iters`. */
  float converge_eps = 0.0f;
  /* Tier 9c: pin boundary loops + dihedral-sharp creases (classifyFeatures) so the
   * iterated flow doesn't erode features. sharp_angle is the dihedral threshold
   * (radians; default 45°, matching the cross field). */
  bool preserve_features = true;
  float sharp_angle = 0.785398f;
  /* Optional granular per-round quality trace (dyntopo_trace.h). When set, every
   * outer iter's BK dab appends its rounds, each stamped with the outer `iter`, so
   * the whole pre-pass is one continuous time-series — the place to see the split
   * bug's sliver oscillation at multi-iter scale. null (default) = no tracing. */
  dyntopo::DynTopoTrace *trace = nullptr;
};

/* Driver-run stats (an optional out-param; the pipeline copies these into its
 * RemeshRunReport). iters_run counts outer iterations actually executed;
 * converged = the converge_eps early-out fired before p.iters. */
struct PreRemeshStats {
  int iters_run = 0;
  bool converged = false;
};

/* Tier 9b convergence driver: iterate bootstrap → rough cross field (cadenced) →
 * Botsch-Kobbelt to a size field → field-aligned smooth, until @p p.iters or the
 * smooth's max vertex move falls below converge_eps·target. With p.preserve_features
 * (9c) it classifies boundary + dihedral-sharp creases each outer iter and pins them
 * through both the BK collapse and the smooth, so the flow follows features instead
 * of eroding them. Writes its rough field into .remesh.f.theta, (when p.density) the
 * size field into .remesh.v.density + an internal scale attr, and (when preserving)
 * the .boundary.* feature overlays — left in place for the caller to inspect. The
 * geometry-only pre-pass for Tier 9; reproject restores detail afterward. */
void preRemesh(mesh::Mesh &m, const PreRemeshParams &p,
               PreRemeshStats *stats = nullptr);

} // namespace sculptcore::remesh
