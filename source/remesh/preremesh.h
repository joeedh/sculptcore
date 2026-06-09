#pragma once

#include <cstdint>

/* Tier 9 input pre-remesh primitives. The field-aligned tangential smooth lives
 * here (rather than in remesh.cc) because it needs both the dyntopo operators and
 * the cross field; the convergence driver (9b) and pipeline glue join it later. */

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::remesh {

/* Botsch-Kobbelt remesh pass: drive `m` toward edge length `L` using dyntopo's
 * split-long / collapse-short / flip / tangential-smooth quartet over a single
 * whole-mesh sphere. Geometry only, no feature preservation (callers that need
 * feature pinning set it up on the mesh first). Shared by --solve decimation and
 * the Tier-9 pre-remesh.
 *
 * @p size_attr optional per-vertex FLOAT attribute name holding a relative size
 * scale s(v) (1 = nominal). When present each edge's split/collapse band is
 * scaled by the mean endpoint s — refine where s < 1, coarsen where s > 1 (a
 * curvature size field). null = uniform target `L` everywhere (default). */
void bkRemeshToTarget(mesh::Mesh &m, float L, uint32_t seed,
                      const char *size_attr = nullptr);

/* Tangential smooth blending isotropic and field-aligned relaxation.
 *
 * @p align in [0,1]: 0 is the classic isotropic one-ring-centroid relaxation
 * (one-ring centroid → Newell-plane tangent projection → edge-scale clamp); 1
 * steers the move with the per-face cross field (.remesh.f.theta) so vertices
 * relax toward straightened u/v isolines. Intermediate values lerp the two target
 * centroids. The tangent-plane projection + edge-scale clamp safety rail applies
 * in both, so the move is always bounded and never shrinks volume. With align > 0
 * the field is read from .remesh.f.theta; where a vertex has no field (attr absent
 * or no incident face contributed) it degrades to isotropic at that vertex. */
void tangentialSmooth(mesh::Mesh &m, int iters, float lambda, float align);

} // namespace sculptcore::remesh
