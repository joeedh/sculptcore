#pragma once

/** Cross-carrier bakes (displacementAndSubSurf plan, X4): move detail between
 * the VDM texel carrier and real geometry.
 *
 * `applyToVerts` — VDM → geometry extraction: displace every vertex by the
 * store's field sampled at its own parameterization (Ptex `.ptex.c.grid` /
 * `.ptex.c.uv` corner attrs, else the mesh's active UV corner layer) through
 * the F3 frame, exactly as the fragment/tessellated tiers render it (the
 * bake ≡ render rule: same sample seam, same `t ⊥ n`, `b = n × t` frame
 * construction as the splatter). Requires current frames
 * (`displace::updateFramesAll`). Optionally clears the store afterwards
 * (plain removal — the caller owns any undo snapshot; spatial AABB pads go
 * stale-loose, which is conservative).
 */

#include "vdm_store.h"

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::vdm {

struct VdmBakeStats {
  int vertsMoved = 0;
  int tilesCleared = 0;
};

VdmBakeStats applyToVerts(mesh::Mesh &m, VdmStore &store, bool clearStore);

} // namespace sculptcore::vdm
