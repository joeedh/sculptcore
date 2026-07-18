#include "litestl/util/alloc.h"
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

/** Serialize the grids store into a freshly-allocated buffer (*out_size = byte
 * count; free with freeMeshBuffer). Returns nullptr on failure. Undo seam for
 * ops that rewrite the store wholesale (down-refit, stack delete). */
uint8_t *Multires_serializeStore(subdiv::Multires *mr, int *out_size)
{
  *out_size = 0;
  if (!mr) {
    return nullptr;
  }
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  if (!mr->store.write(ss)) {
    return nullptr;
  }
  std::string s = ss.str();
  uint8_t *buf =
      static_cast<uint8_t *>(litestl::alloc::alloc("multires store buffer", s.size()));
  std::memcpy(buf, s.data(), s.size());
  *out_size = int(s.size());
  return buf;
}

/** Replace the store from a Multires_serializeStore blob (same cage topology),
 * then invalidate every derived level. Deactivates the current level — the
 * caller must Multires_setActiveLevel + re-fetch mesh/tree afterwards.
 * Returns 1 on success. */
int Multires_restoreStore(subdiv::Multires *mr, const uint8_t *data, int size)
{
  if (!mr || !data || size <= 0) {
    return 0;
  }
  std::string s(reinterpret_cast<const char *>(data), size_t(size));
  std::stringstream ss(s, std::ios::in | std::ios::out | std::ios::binary);
  if (!mr->store.read(ss)) {
    return 0;
  }
  mr->invalidateAll();
  return 1;
}
}
