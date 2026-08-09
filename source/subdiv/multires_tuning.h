#pragma once

/** Size-derived tuning for a multires level's acceleration structures.
 *
 * Three granularities are picked here, all from the level's own size (vert
 * count, grid count, grid side) rather than fixed constants:
 *
 *   - `gridLeafVertTarget` — GridTree leaf size (grids-native brush path).
 *     `GridTree::build` clusters whole cage faces, so the target is measured in
 *     those blocks and a target below one block is inert. The sweep found the
 *     optimum follows the block and not the level's vert count, which is why
 *     this one alone does not scale with size.
 *   - `drawNodeTriTarget` — GridDrawSource partition (the external-draw path).
 *     One node is one host draw call and one GPU buffer, so a fixed tri target
 *     makes draw calls grow linearly with the level too (level 6 on a 4k-face
 *     cage would be ~16k of them). Bigger nodes cost more per-dab refill, so
 *     this is a real two-sided trade — see the sweep results in
 *     claudeMemory/research/multires-autotune.md.
 *   - `slot*` — the materialized level mesh's SpatialTree (mesh-path tools
 *     only; lazy sessions never build one).
 *
 * Every field can be overridden from the environment for sweeps, and
 * `SC_MR_AUTOTUNE=0` restores the pre-autotune fixed constants
 * (GridTree::kDefaultLeafVertTarget, GridDrawSource::kNodeTriTarget,
 * SpatialTree's member initializers) as an A/B baseline.
 */

#include <cstdint>

namespace sculptcore::subdiv {

struct MultiresTuning {
  int gridLeafVertTarget = 512;
  int drawNodeTriTarget = 2048;
  int slotLeafLimit = 512;
  int slotDepthLimit = 10;
  int slotGpuTriTarget = 2048;
};

/** Pick the granularities for a level of `vertCount` verts made of
 * `gridCount` grids of `gridSide` cells per side. Pure function of those
 * three numbers (plus the env overrides), so a level's structures agree
 * however they are reached. */
MultiresTuning multiresAutoTune(int64_t vertCount, int gridCount, int gridSide);

} // namespace sculptcore::subdiv
