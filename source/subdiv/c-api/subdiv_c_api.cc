#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "spatial/spatial.h"
#include "subdiv/multires.h"

#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

using namespace sculptcore;

extern "C" {

/** Build a multires stack over `cage` (NOT owned; the caller keeps it alive for
 * the stack's lifetime) with `levels` uniform CC refinements. Tree params
 * (0 = default) apply to every level materialization. No level is active yet —
 * call Multires_setActiveLevel next. */
subdiv::Multires *Multires_new(
    mesh::Mesh *cage, int levels, int leafLimit, int depthLimit, int gpuTriTarget)
{
  if (!cage || levels < 1) {
    return nullptr;
  }
  auto *mr = litestl::alloc::New<subdiv::Multires>("Multires");
  mr->treeLeafLimit = leafLimit;
  mr->treeDepthLimit = depthLimit;
  mr->treeGpuTriTarget = gpuTriTarget;
  mr->init(*cage, levels);
  return mr;
}

void Multires_free(subdiv::Multires *mr)
{
  if (mr) {
    litestl::alloc::Delete(mr);
  }
}

/** Write back the outgoing level, materialize + activate `level` (clamped to
 * [1, maxLevel]). Returns the active level. Fetch the slot's mesh/tree with
 * Multires_activeMesh/Tree — pointers change on every switch. */
int Multires_setActiveLevel(subdiv::Multires *mr, int level)
{
  if (!mr) {
    return 0;
  }
  level = level < 1 ? 1 : (level > mr->maxLevel() ? mr->maxLevel() : level);
  mr->setActiveLevel(level);
  return mr->activeLevel();
}

/** The active level's materialized mesh / spatial tree — NON-OWNING views (the
 * stack owns both; never free them). Null when no level is active. */
mesh::Mesh *Multires_activeMesh(subdiv::Multires *mr)
{
  if (!mr || mr->activeLevel() < 1) {
    return nullptr;
  }
  subdiv::MultiresSlot *slot = mr->findSlot(mr->activeLevel());
  return slot ? slot->mesh : nullptr;
}

spatial::SpatialTree *Multires_activeTree(subdiv::Multires *mr)
{
  if (!mr || mr->activeLevel() < 1) {
    return nullptr;
  }
  subdiv::MultiresSlot *slot = mr->findSlot(mr->activeLevel());
  return slot ? slot->tree : nullptr;
}

int Multires_writeback(subdiv::Multires *mr, int level)
{
  return mr ? mr->writeback(level) : 0;
}

int Multires_downRefit(subdiv::Multires *mr, int level)
{
  return mr ? mr->downRefit(level) : 0;
}

/** Number of levels in the stack. */
int Multires_maxLevel(subdiv::Multires *mr)
{
  return mr ? mr->maxLevel() : 0;
}

/** Append one finer level (zero displacement, i.e. a smooth subdivision of the
 * current finest surface) and make it active, preserving existing detail.
 * Returns the new maxLevel (unchanged at the level cap). The app calls this
 * when the host's own multires gains a level, so both stacks stay in step. */
int Multires_addLevel(subdiv::Multires *mr)
{
  return mr ? mr->addLevel() : 0;
}

/** Pop the finest level — addLevel()'s inverse. Returns the new maxLevel
 * (unchanged when only one level remains). Pending edits on the active level
 * are folded first, so detail at the surviving levels is kept. */
int Multires_removeTopLevel(subdiv::Multires *mr)
{
  return mr ? mr->removeTopLevel() : 0;
}

/** Number of grid samples at `level`: gridCount * (2^(level-1)+1)^2 — the
 * element count the `out` buffer for Multires_levelPositionsOut must hold (each
 * element is 3 floats). Boundary/seam verts are counted once per grid that owns
 * them (replicated slots), matching Blender's per-loop MDISPS layout. */
int Multires_levelSampleCount(subdiv::Multires *mr, int level)
{
  if (!mr) {
    return 0;
  }
  litestl::util::Vector<int> gridVerts;
  mr->levelGridVertsOut(level, gridVerts);
  return int(gridVerts.size());
}

/** Dump absolute object-space positions of `level`'s grid samples into `out`
 * (Multires_levelSampleCount entries of 3 floats), grid-major row-major:
 * `out[g*w*w + v*w + u]` is lattice `(u,v)` of grid `g`, `w = 2^(level-1)+1`.
 * This is SculptCore's native grid order (`+u` = corner edge, `+v` = previous
 * corner edge); any `u`<->`v` transpose vs Blender's MDISPS is applied by the
 * Blender-side mapping. Materializes `level` as a side effect. Returns the
 * sample count, 0 on failure. */
int Multires_levelPositionsOut(subdiv::Multires *mr, int level, float (*out)[3])
{
  if (!mr || !out) {
    return 0;
  }
  level = level < 1 ? 1 : (level > mr->maxLevel() ? mr->maxLevel() : level);
  mr->setActiveLevel(level);
  subdiv::MultiresSlot *slot = mr->findSlot(level);
  if (!slot || !slot->mesh) {
    return 0;
  }
  mesh::Mesh *m = slot->mesh;
  litestl::util::Vector<int> gridVerts;
  mr->levelGridVertsOut(level, gridVerts);
  const int sample_num = int(gridVerts.size());
  for (int i = 0; i < sample_num; i++) {
    const int vid = gridVerts[i];
    if (vid < 0) {
      out[i][0] = out[i][1] = out[i][2] = 0.0f;
      continue;
    }
    const float3 co = m->v.co[vid];
    out[i][0] = co[0];
    out[i][1] = co[1];
    out[i][2] = co[2];
  }
  return sample_num;
}

/** Seed `level` from grid-sample absolute positions (the A2 layout:
 * Multires_levelSampleCount entries, grid-major row-major, boundary replicas
 * included), then write them back into the store as `level` displacement over
 * the discrete base. `positions` is in SculptCore grid order (`+u` corner edge,
 * `+v` previous corner edge); a Blender caller applies the MDISPS<->grid
 * transpose while filling the buffer. Replicated seam samples must carry equal
 * values (they do from A2 / consistent MDISPS); scatter is last-writer-wins.
 * The seed lands wholly at this level, so it is then cascaded down (see
 * Multires::propagateDown) to give every coarser level a surface that
 * summarizes it rather than the bare discrete base — this level's own surface
 * is preserved exactly. When anything changed the level is rematerialized from
 * the store, so previously fetched active mesh/tree pointers are invalid —
 * re-fetch via Multires_activeMesh/Tree. Returns the changed-vert count, -1 on
 * a sample-count mismatch, 0 on failure. */
int Multires_fromLevelPositions(
    subdiv::Multires *mr, int level, const float (*positions)[3], int sample_num)
{
  if (!mr || !positions) {
    return 0;
  }
  level = level < 1 ? 1 : (level > mr->maxLevel() ? mr->maxLevel() : level);
  subdiv::MultiresSlot *slot = mr->setActiveLevel(level);
  if (!slot || !slot->mesh) {
    return 0;
  }
  litestl::util::Vector<int> gridVerts;
  mr->levelGridVertsOut(level, gridVerts);
  if (sample_num != int(gridVerts.size())) {
    return -1;
  }
  mesh::Mesh *m = slot->mesh;
  for (int i = 0; i < sample_num; i++) {
    const int vid = gridVerts[i];
    if (vid < 0) {
      continue;
    }
    m->v.co[vid] = float3(positions[i][0], positions[i][1], positions[i][2]);
  }
  const int changed = mr->writeback(level);
  if (changed > 0) {
    // Hand the freshly seeded surface down the pyramid so switching to a
    // coarser level shows a summary of it, not the discrete base. Each step
    // preserves the surface of the level it reads, so `level` is untouched.
    for (int l = level; l >= 2; l--) {
      mr->propagateDown(l);
    }
    // The slot's tree + normals were built from pre-seed positions; drop it
    // and rematerialize from the store so the active mesh/tree pair is
    // consistent (slot pointers change — callers re-fetch, like downRefit).
    mr->invalidateAbove(level - 1);
    mr->setActiveLevel(level);
  }
  return changed;
}

/** Seed `level` from grid-sample absolute positions without materializing
 * anything (Multires::seedLevelPositions — same sample layout as
 * Multires_fromLevelPositions). Down-propagation is deferred as debt; any
 * resident slot of the level is dropped, so call Multires_setActiveLevel
 * afterwards. The fast mode-enter path. */
int Multires_seedLevelPositions(
    subdiv::Multires *mr, int level, const float (*positions)[3], int sample_num)
{
  if (!mr || !positions) {
    return 0;
  }
  level = level < 1 ? 1 : (level > mr->maxLevel() ? mr->maxLevel() : level);
  return mr->seedLevelPositions(level, positions, sample_num);
}

/** Geometry -> VDM capture (X4 stage 2): move `level`'s grids-store disp into
 * the Ptex VDM store's texels, zero the disp, drop the surface onto the
 * smooth base. Returns texels written; caller owns undo snapshots + the
 * spatial refresh of the attached level mesh. */
int Multires_captureToVdm(subdiv::Multires *mr, void *vstore, int level)
{
  if (!mr || !vstore) {
    return 0;
  }
  return mr->captureDetailToVdm(level,
                                *static_cast<sculptcore::vdm::VdmStore *>(vstore));
}

/** Down-propagation debt header, written AHEAD of the grids store's own bytes:
 * magic, level count, then one byte per level (index 0 unused, mirroring
 * Multires's own indexing). The debt is genuine multires state that the grids
 * store does not hold — a level whose displacement is zero still differs from
 * the restriction of the level above it, so "does this level owe the one
 * below?" cannot be recomputed from the store — and dropping it across an undo
 * would re-open the coarse-levels-do-not-follow-a-fine-edit bug on the restored
 * state. It leads rather than trails because GridsStore::read slurps its stream
 * to EOF, which would swallow anything written after it. Kept out of
 * GridsStore::write so the on-disk grid format is unchanged; a blob with no
 * header (an undo step pushed before this landed) restores with no debt. */
static const char kDebtMagic[4] = {'M', 'R', 'D', 'P'};

/** Serialize the grids store into a freshly-allocated buffer (*out_size = byte
 * count; free with freeMeshBuffer). Returns nullptr on failure. Undo seam for
 * ops that rewrite the store wholesale (down-refit, stack delete); carries the
 * down-propagation debt header described above. */
uint8_t *Multires_serializeStore(subdiv::Multires *mr, int *out_size)
{
  *out_size = 0;
  if (!mr) {
    return nullptr;
  }
  // Assembled in a byte vector rather than a stringstream: this runs at the end
  // of every stroke, and a stringstream round trip copies the whole ~23 MB store
  // an extra time.
  litestl::util::Vector<uint8_t> buf;
  const int32_t n = mr->maxLevel();
  buf.resize<false>(sizeof(kDebtMagic) + sizeof(n) + size_t(n) + 1);
  std::memcpy(buf.data(), kDebtMagic, sizeof(kDebtMagic));
  std::memcpy(buf.data() + sizeof(kDebtMagic), &n, sizeof(n));
  for (int l = 0; l <= n; l++) {
    buf[int(sizeof(kDebtMagic) + sizeof(n)) + l] = mr->downPropDebt(l) ? 1 : 0;
  }
  // Fast lz4, not lz4hc: this blob is an undo snapshot taken at the end of every
  // stroke and never written to disk, so a ~20x compress stall to save a few
  // percent of RAM is the wrong trade.
  if (!mr->store.writeBytes(buf, io::kFastCompressLevel)) {
    return nullptr;
  }
  // Hand the vector's own heap block over rather than copying ~9 MB again;
  // freeMeshBuffer releases it through the same allocator that grew it.
  *out_size = int(buf.size());
  uint8_t *out = buf.steal_data();
  if (!out) {
    out = static_cast<uint8_t *>(litestl::alloc::alloc("multires store buffer", buf.size()));
    std::memcpy(out, buf.data(), buf.size());
  }
  return out;
}

/** Replace the store from a Multires_serializeStore blob (same cage topology),
 * then invalidate every derived level. Deactivates the current level — the
 * caller must Multires_setActiveLevel + re-fetch mesh/tree afterwards (and
 * because the level is deactivated, that call cannot propagate: the restored
 * state is reproduced verbatim, not re-derived). Restores the debt header
 * when present, so an undo lands on a state that will still push a fine edit
 * downward the next time the user switches levels. Returns 1 on success. */
int Multires_restoreStore(subdiv::Multires *mr, const uint8_t *data, int size)
{
  if (!mr || !data || size <= 0) {
    return 0;
  }
  std::string s(reinterpret_cast<const char *>(data), size_t(size));
  std::stringstream ss(s, std::ios::in | std::ios::out | std::ios::binary);
  // Debt header first — not optional, GridsStore::read consumes to EOF. A
  // headerless blob rewinds so the store still sees byte 0 and restores with no
  // debt: reproduce that snapshot, never mutate it on the next switch.
  bool haveDebt = false;
  int32_t n = 0;
  char magic[sizeof(kDebtMagic)] = {0};
  if (ss.read(magic, sizeof(magic)) && std::memcmp(magic, kDebtMagic, sizeof(magic)) == 0 &&
      ss.read(reinterpret_cast<char *>(&n), sizeof(n)) && n >= 0)
  {
    haveDebt = true;
  }
  else {
    ss.clear();
    ss.seekg(0);
  }
  std::string debt;
  if (haveDebt) {
    debt.resize(size_t(n) + 1, 0);
    for (int l = 0; l <= n; l++) {
      char b = 0;
      haveDebt = haveDebt && bool(ss.read(&b, 1));
      debt[size_t(l)] = b;
    }
  }
  if (!mr->store.read(ss)) {
    return 0;
  }
  mr->invalidateAll();
  mr->clearDownPropDebt();
  for (int l = 0; haveDebt && l <= n; l++) {
    mr->setDownPropDebt(l, debt[size_t(l)] != 0);
  }
  return 1;
}
}
