#pragma once

/** Surface-roughness / fidelity metrics for the brush displacement-base A/B
 * (documentation/plans/2026-07-26-0909-brush-displacement-base-attribute.md
 * §9.1). Scores either the live surface or the derived displacement base over
 * an explicit vertex region, so "the base left the stroke-start surface" is
 * measured directly rather than through its downstream symptom. */

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::debug_app {

/** Which point set the metric scores. */
enum class RoughnessPoints {
  // The live mesh positions.
  Live,
  // The brush's displacement base: `co - disp` on the disp path,
  // `.brush.orig.co` on the legacy path, live co for unstamped verts.
  Base,
};

struct RoughnessResult {
  int verts = 0; // interior region verts scored
  int edges = 0; // interior region edges scored
  // Normal-component one-ring roughness r(v) = dot(v - centroid, n) / h.
  float rms = 0.0f;
  float p95 = 0.0f;
  float maxr = 0.0f;
  // Fidelity guard, always from the LIVE positions so that "less noise" can't
  // be won by depositing less displacement.
  float maxDisp = 0.0f; // max |dot(co, up) - rest| over the region
  float volume = 0.0f;  // area-weighted sum of that same signed height
  // Mean |dihedral angle| over interior region edges, radians.
  float dihedral = 0.0f;
};

/** Score `region`'s interior verts. A vert is interior when it, its whole
 * one-ring, and every incident edge's radial pair are in-region and manifold.
 * `strokeGen` keys the base attrs; `up`/`rest` define the fidelity guard's
 * signed height (for the flat-grid fixture: up = +Z, rest = the grid plane). */
RoughnessResult computeRoughness(mesh::Mesh *m,
                                 const litestl::util::Vector<int> &region,
                                 RoughnessPoints pts,
                                 uint32_t strokeGen,
                                 litestl::math::float3 up = {0.0f, 0.0f, 1.0f},
                                 float rest = 0.0f);

/** Collect every live vert within `radius` of any point in `centers`. */
void collectRegion(mesh::Mesh *m,
                   const litestl::util::Vector<litestl::math::float3> &centers,
                   float radius,
                   litestl::util::Vector<int> &out);

} // namespace sculptcore::debug_app
