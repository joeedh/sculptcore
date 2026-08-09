#include "multires_tuning.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace sculptcore::subdiv {

namespace {

int64_t clampi(int64_t v, int64_t lo, int64_t hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

/* An env override, or `def` when unset/unparseable. Read every call: these
 * exist for sweeps, and a bench flips them between runs of one process. */
int envInt(const char *name, int def)
{
  const char *s = getenv(name);
  if (!s || !*s) {
    return def;
  }
  char *end = nullptr;
  long v = strtol(s, &end, 10);
  if (end == s || v <= 0) {
    return def;
  }
  return int(v);
}

bool autoTuneEnabled()
{
  const char *s = getenv("SC_MR_AUTOTUNE");
  return !(s && s[0] == '0');
}

// Constants below come from the level sweep in
// claudeMemory/research/multires-autotune.md (addon repo).

// Leaf size in cage-face blocks: GridTree::build clusters whole cage faces, so
// one block (4 grids of (S+1)^2) is the floor, and the sweep found the optimum
// follows the block rather than the vert count. Worth ~0.3 ms/dab at 4 M verts.
constexpr int kLeafBlocks = 2;
constexpr int kMinLeafVerts = 512;
constexpr int kMaxLeafVerts = 4096;

// Draw-node size. The host pays per node per frame, the provider refills whole
// dirty nodes per dab; their geometric mean grows both as sqrt(tris), with the
// constant placing the measured knee (2.0 M tris) at ~8192 tris/node.
constexpr double kDrawTriShape = 33.0;
constexpr int kMinDrawTris = 2048;
constexpr int kMaxDrawTris = 65536;

} // namespace

MultiresTuning multiresAutoTune(int64_t vertCount, int gridCount, int gridSide)
{
  MultiresTuning t;
  const bool autoTune = autoTuneEnabled();

  if (autoTune && gridSide > 0) {
    const int64_t blockVerts = 4 * int64_t(gridSide + 1) * int64_t(gridSide + 1);
    t.gridLeafVertTarget =
        int(clampi(kLeafBlocks * blockVerts, kMinLeafVerts, kMaxLeafVerts));
  }

  const int64_t tris = int64_t(gridCount) * int64_t(gridSide) * int64_t(gridSide) * 2;
  if (autoTune && tris > 0) {
    t.drawNodeTriTarget = int(clampi(int64_t(std::sqrt(kDrawTriShape * double(tris))),
                                     kMinDrawTris, kMaxDrawTris));
  }

  if (autoTune && vertCount > 0) {
    // The materialized level mesh, sized like SpatialTree::autoTuneLimits. No
    // benchmark backs this one: lazy sessions never build a slot tree.
    t.slotLeafLimit = int(clampi(vertCount / 16, 64, 512));
    t.slotGpuTriTarget = int(clampi(tris / 256, 2048, 65536));
  }

  t.gridLeafVertTarget = envInt("SC_MR_LEAF_TARGET", t.gridLeafVertTarget);
  t.drawNodeTriTarget = envInt("SC_MR_DRAW_TRIS", t.drawNodeTriTarget);
  t.slotLeafLimit = envInt("SC_MR_SLOT_LEAF", t.slotLeafLimit);
  t.slotDepthLimit = envInt("SC_MR_SLOT_DEPTH", t.slotDepthLimit);
  t.slotGpuTriTarget = envInt("SC_MR_SLOT_GPU_TRIS", t.slotGpuTriTarget);
  return t;
}

} // namespace sculptcore::subdiv
