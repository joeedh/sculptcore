#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "spatial/spatial.h"
#include "subdiv/grid_domain.h"
#include "subdiv/grid_draw_source.h"
#include "subdiv/grid_tree.h"
#include "subdiv/multires.h"
#include "subdiv/multires_tuning.h"

#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

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

/** setActiveLevel without materializing the slot: writeback + down-prop debt
 * settling run on the chain; the slot builds on first mesh-path need
 * (the lazy-slot enter — grids-native hosts draw and sculpt without it).
 * Returns the actual active level. */
int Multires_setActiveLevelLazy(subdiv::Multires *mr, int level)
{
  if (!mr) {
    return 0;
  }
  level = level < 1 ? 1 : (level > mr->maxLevel() ? mr->maxLevel() : level);
  mr->setActiveLevel(level, /*propagate=*/true, /*materializeSlot=*/false);
  return mr->activeLevel();
}

/** Dense level vert count (the grid domain / lazy-slot id space; boundary
 * verts counted once, unlike Multires_levelSampleCount's replicas). */
int Multires_levelVertCount(subdiv::Multires *mr, int level)
{
  if (!mr || level < 1 || level > mr->maxLevel()) {
    return 0;
  }
  return mr->refiner.levels[level - 1].vertCount;
}

/** Report the level's chosen acceleration granularities into `out`
 * (8 ints; short arrays are filled up to `count`):
 *
 *   0 grid-tree leaf vert target   1 grid-tree leaf count
 *   2 draw-node tri target         3 draw-node count
 *   4 level vert count             5 grid count
 *   6 grid side (cells)            7 slot SpatialTree leaf limit
 *
 * Reports what is BUILT where a structure exists (the grid tree, the draw
 * source), and what would be chosen where one does not — so a bench can read
 * a lazy session without materializing anything it is about to time. */
int Multires_tuningStats(subdiv::Multires *mr, int level, int *out, int count)
{
  if (!mr || !out || count < 1 || level < 1 || level > mr->maxLevel()) {
    return 0;
  }
  const int verts = mr->refiner.levels[level - 1].vertCount;
  const int side = subdiv::GridsStore::sideForLevel(level);
  const subdiv::MultiresTuning t =
      subdiv::multiresAutoTune(verts, mr->store.gridCount(), side);

  int leafTarget = t.gridLeafVertTarget, leaves = 0;
  if (mr->hasGridDomain(level)) {
    if (subdiv::GridTree *tree = mr->gridDomain(level)->ensureTree()) {
      leaves = int(tree->leaves.size());
    }
  }
  int triTarget = t.drawNodeTriTarget, nodes = 0;
  if (subdiv::GridDrawSource *src = mr->drawSource()) {
    if (src->level() == level) {
      triTarget = src->nodeTriTarget();
      nodes = src->nodeCount();
    }
  }

  const int vals[8] = {leafTarget,
                       leaves,
                       triTarget,
                       nodes,
                       verts,
                       mr->store.gridCount(),
                       side,
                       t.slotLeafLimit};
  const int n = count < 8 ? count : 8;
  for (int i = 0; i < n; i++) {
    out[i] = vals[i];
  }
  return n;
}

/** Copy the grid domain's dense mask into `out` (levelVertCount floats).
 * Builds the domain if needed (mask exchange implies the level is in use). */
int Multires_readDomainMask(subdiv::Multires *mr, int level, float *out, int count)
{
  if (!mr || !out || level < 1 || level > mr->maxLevel()) {
    return 0;
  }
  subdiv::GridLevelDomain *d = mr->gridDomain(level);
  if (count != d->vertCount()) {
    return 0;
  }
  for (int v = 0; v < count; v++) {
    out[v] = d->mask[v];
  }
  return count;
}

/** Write the grid domain's dense mask (levelVertCount floats) and land it in
 * the store channel (all seam replicas), marking the draw source — the
 * lazy-slot mask import path (CD_GRID_PAINT_MASK -> domain, no slot column
 * involved). */
int Multires_writeDomainMask(subdiv::Multires *mr, int level, const float *values, int count)
{
  if (!mr || !values || level < 1 || level > mr->maxLevel()) {
    return 0;
  }
  subdiv::GridLevelDomain *d = mr->gridDomain(level);
  if (count != d->vertCount()) {
    return 0;
  }
  d->ensureMaskChannel();
  for (int v = 0; v < count; v++) {
    d->mask[v] = values[v];
  }
  d->flushMaskToStore();
  if (subdiv::GridDrawSource *ds = mr->drawSource()) {
    ds->markAllData();
  }
  return count;
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
  // Fold any pending mesh-path edits on the active level (guarded: a stale
  // slot after grids strokes is healed, not diffed), then read the CHAIN —
  // the same positions a materialized slot would carry, without paying the
  // level-mesh + tree build. This used to setActiveLevel(level), so every
  // save below top materialized the top slot.
  mr->writeback(mr->activeLevel());
  const litestl::util::Vector<float3> &pos = mr->levelPositions(level);
  litestl::util::Vector<int> gridVerts;
  mr->levelGridVertsOut(level, gridVerts);
  const int sample_num = int(gridVerts.size());
  for (int i = 0; i < sample_num; i++) {
    const int vid = gridVerts[i];
    if (vid < 0 || vid >= int(pos.size())) {
      out[i][0] = out[i][1] = out[i][2] = 0.0f;
      continue;
    }
    const float3 co = pos[vid];
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
 * magic, level count, then one byte of flags per level (index 0 unused,
 * mirroring Multires's own indexing): bit 0 is the position debt, bit 1 says
 * grid channels owe as well and is followed -- after the byte array -- by that
 * level's list of owing channel names. The debt is genuine multires state that the grids
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
    // bit 0: the position debt propagateDown settles. bit 1: at least one grid
    // channel owes the level below (propagateAttrsDown). A blob with no bit 1
    // anywhere is byte-identical to the header this format started with.
    uint8_t bits = mr->downPropDebt(l) ? 1 : 0;
    if (mr->store.anyChannelLevelDebt(l)) {
      bits |= 2;
    }
    buf[int(sizeof(kDebtMagic) + sizeof(n)) + l] = bits;
  }
  // Which channels owe, by name, for every level whose bit 1 is set — ascending
  // level order, so the reader needs no offsets. Names, not indices, because
  // removeChannel shifts every index above it and an undo step outlives that.
  auto putBytes = [&buf](const void *p, size_t nbytes) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < nbytes; i++) {
      buf.append(b[i]);
    }
  };
  for (int l = 0; l <= n; l++) {
    if (!mr->store.anyChannelLevelDebt(l)) {
      continue;
    }
    int32_t cnt = 0;
    for (int c = 0; c < mr->store.channelCount(); c++) {
      cnt += mr->store.channelLevelDebt(l, c) ? 1 : 0;
    }
    putBytes(&cnt, sizeof(cnt));
    for (int c = 0; c < mr->store.channelCount(); c++) {
      if (!mr->store.channelLevelDebt(l, c)) {
        continue;
      }
      const int32_t len = int32_t(mr->store.channelName(c).size());
      putBytes(&len, sizeof(len));
      putBytes(mr->store.channelName(c).c_str(), size_t(len));
    }
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
  std::vector<std::vector<std::string>> attrDebt;
  if (haveDebt) {
    debt.resize(size_t(n) + 1, 0);
    for (int l = 0; l <= n; l++) {
      char b = 0;
      haveDebt = haveDebt && bool(ss.read(&b, 1));
      debt[size_t(l)] = b;
    }
    // The per-level channel-name lists that bit 1 announces, ascending. A
    // header written before they existed sets no bit 1 and so reads nothing
    // here, landing on the store bytes exactly where it used to.
    attrDebt.resize(size_t(n) + 1);
    for (int l = 0; haveDebt && l <= n; l++) {
      if (!(debt[size_t(l)] & 2)) {
        continue;
      }
      int32_t cnt = 0;
      haveDebt = bool(ss.read(reinterpret_cast<char *>(&cnt), sizeof(cnt))) && cnt >= 0;
      for (int i = 0; haveDebt && i < cnt; i++) {
        int32_t len = 0;
        haveDebt = bool(ss.read(reinterpret_cast<char *>(&len), sizeof(len))) && len >= 0;
        if (!haveDebt) {
          break;
        }
        std::string nm;
        nm.resize(size_t(len));
        haveDebt = len == 0 || bool(ss.read(&nm[0], len));
        attrDebt[size_t(l)].push_back(nm);
      }
    }
  }
  if (!mr->store.read(ss)) {
    return 0;
  }
  mr->invalidateAll();
  mr->clearDownPropDebt();
  // Channel debt needs no clearing pass: store.read replaced every channel, and
  // a freshly read level starts out owing nothing.
  for (int l = 0; haveDebt && l <= n; l++) {
    mr->setDownPropDebt(l, (debt[size_t(l)] & 1) != 0);
    for (const std::string &nm : attrDebt[size_t(l)]) {
      const int c = mr->store.findChannel(litestl::util::string(nm.c_str()));
      if (c >= 0) {
        mr->store.setChannelLevelDebt(l, c, true);
      }
    }
  }
  return 1;
}

/** Declare that the host can PERSIST attribute `name` (a mesh::AttrType) per
 * grid element, so brushes may write it there. Blender declares exactly one:
 * the scalar "mask". Everything undeclared and not TEMP is Derived — the brush
 * writes the cage attribute and the grid data is re-subdivided from it. */
void Multires_declareHostGridAttr(subdiv::Multires *mr, const char *name, int type)
{
  if (mr && name) {
    mr->gridAttrs().declareHostAttr(name, mesh::AttrType(type));
  }
}

void Multires_clearHostGridAttrs(subdiv::Multires *mr)
{
  if (mr) {
    mr->gridAttrs().clearHostAttrs();
  }
}

/** Where a write to `name` must land: 0 none, 1 host, 2 derived, 3 temp
 * (subdiv::GridAttrStorage). `flags` is a mesh::AttrFlag bitmask. */
int Multires_gridAttrStorage(subdiv::Multires *mr, const char *name, int type, int flags)
{
  if (!mr || !name) {
    return 0;
  }
  return int(mr->gridAttrs().storageFor(name, mesh::AttrType(type), mesh::AttrFlag(flags)));
}

/** The OpenSubdiv face-varying linear rule for UV subdivision — Blender's
 * `uv_smooth` enum passes through unmapped (subdiv::UvSmooth). */
void Multires_setUvSmooth(subdiv::Multires *mr, int mode)
{
  if (mr && mode >= 0 && mode <= int(subdiv::UvSmooth::SmoothAll)) {
    mr->gridAttrs().setUvSmooth(subdiv::UvSmooth(mode));
  }
}

/** A cage attribute changed: drop its derived grid layer. A null or empty
 * `name` drops every layer. */
void Multires_invalidateGridAttr(subdiv::Multires *mr, const char *name)
{
  if (!mr) {
    return;
  }
  if (name && name[0]) {
    mr->gridAttrs().invalidate(name);
  }
  else {
    mr->gridAttrs().invalidateAll();
  }
}

/** Re-stamp a resident level mesh with the derived cage attributes (uv, color,
 * group) after something invalidated them. No-op when the level is lazy — a
 * later materialize() stamps it on the way in. */
void Multires_syncSlotAttrs(subdiv::Multires *mr, int level)
{
  if (!mr || level < 1 || level > mr->maxLevel()) {
    return;
  }
  if (subdiv::MultiresSlot *slot = mr->findSlot(level)) {
    if (slot->mesh) {
      mr->assignDerivedAttrs(*slot->mesh, level);
    }
  }
}

/** Push the resident level mesh's per-cell INT face attribute back onto the
 * cage (Multires::scatterFaceIntToCage — the return route for a mesh-path
 * face-set edit on multires) and tell both draw paths what moved: the grids
 * source refills only the touched grids, and the slot's tree re-uploads.
 * Returns the number of cage faces changed. */
int Multires_scatterFaceIntToCage(subdiv::Multires *mr, int level, const char *name)
{
  if (!mr || !name || !name[0]) {
    return 0;
  }
  litestl::util::Vector<int> touched;
  const int changed = mr->scatterFaceIntToCage(level, name, touched);
  if (!changed) {
    return 0;
  }
  if (subdiv::GridDrawSource *ds = mr->drawSource()) {
    ds->markGrids(std::span<const int>(touched.data(), touched.size()));
  }
  if (subdiv::MultiresSlot *slot = mr->findSlot(level)) {
    if (slot->tree) {
      for (spatial::SpatialNode *leaf : slot->tree->leaves()) {
        leaf->flag |= spatial::Spatial_UpdateGPU;
      }
    }
  }
  return changed;
}

/** Push a level's per-vertex FLOAT4 attribute back onto the cage
 * (Multires::scatterVertFloat4ToCage — the return route for painted colour on
 * multires, whose only persistent home is the base mesh) and tell both draw
 * paths what moved, exactly as the face twin does. Returns the number of cage
 * verts changed.
 *
 * The redraw is the point, not a side effect: the scatter re-derives the
 * touched grids from the cage it just wrote, so what is on screen after this
 * call is the cage-resolution paint that will still be there after a reload.
 * Called per dab, that is what makes colour a `Derived` attribute in fact and
 * not just in name.
 *
 * `dabs` is that dab region, 4 floats each ({x, y, z, radius}, object space);
 * pass null / 0 outside a stroke. Its grids re-derive even when no cage vert
 * moved, which is how a dab finer than a base face comes out painting nothing
 * rather than painting and then evaporating. */
int Multires_scatterVertFloat4ToCage(
    subdiv::Multires *mr, int level, const char *name, const float *dabs, int dab_count)
{
  if (!mr || !name || !name[0]) {
    return 0;
  }
  litestl::util::Vector<int> touched;
  const int changed = mr->scatterVertFloat4ToCage(level, name, dabs, dab_count, touched);
  if (touched.size() == 0) {
    return 0;
  }
  if (subdiv::GridDrawSource *ds = mr->drawSource()) {
    ds->markGrids(std::span<const int>(touched.data(), touched.size()));
  }
  if (subdiv::MultiresSlot *slot = mr->findSlot(level)) {
    if (slot->tree) {
      for (spatial::SpatialNode *leaf : slot->tree->leaves()) {
        leaf->flag |= spatial::Spatial_UpdateGPU;
      }
    }
  }
  return changed;
}

/** The cage's "no face set" group id (mesh::Mesh::default_group_id) — the
 * grids fset stream leaves it untinted, mirroring what
 * sc_external_draw_set_default_group does for the mesh path. Drops the derived
 * face-set colors so the next draw re-tints. */
void Multires_setDefaultGroupId(subdiv::Multires *mr, int group)
{
  if (!mr || !mr->cage() || mr->cage()->default_group_id == group) {
    return;
  }
  mr->cage()->default_group_id = group;
  // Resident slots derived their copy from the old id (assignDerivedAttrs);
  // leaving it stale would make a face-set edit on one read as a whole-mesh
  // change on the way back to the cage.
  for (int level = 1; level <= mr->maxLevel(); level++) {
    if (subdiv::MultiresSlot *slot = mr->findSlot(level)) {
      if (slot->mesh) {
        slot->mesh->default_group_id = group;
      }
    }
  }
  mr->gridAttrs().invalidate("group");
}

/** Read the derived grid samples of cage attribute `name` at `level` into
 * `out` — gridCount·(S+1)² samples of N floats, grid-major then row-major
 * lattice order. Returns the floats written, or 0. The parity gate reads
 * UVs through this. */
int Multires_gridAttrSamplesOut(
    subdiv::Multires *mr, int level, const char *name, float *out, int count)
{
  if (!mr || !name || !out || level < 1 || level > mr->maxLevel()) {
    return 0;
  }
  int comps = 0;
  const float *src = mr->gridAttrs().samples(level, name, &comps);
  if (!src || comps == 0) {
    return 0;
  }
  const int S = mr->refiner.levels[level - 1].gridSide;
  const int total = mr->refiner.gridCount() * (S + 1) * (S + 1) * comps;
  if (count < total) {
    return 0;
  }
  memcpy(out, src, size_t(total) * sizeof(float));
  return total;
}
}
