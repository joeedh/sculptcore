#pragma once

/** VDM tile-delta undo chunk (displacementAndSubSurf plan, V2): rides the
 * dab's MeshLog step via MeshLog::appendChunk, so one undo press reverts the
 * dab's vertex/topology edits AND its texel edits together. VdmDelta is a
 * self-inverse swap (vdm_store.h), so undo and redo are the same operation.
 *
 * The chunk holds a non-owning VdmStore pointer: the store must outlive the
 * log history that references it (the owner clears the log when destroying
 * the store, exactly like the mesh itself). Spatial AABB pads are left
 * stale-loose on undo (conservative — never a missed hit); the next dab's
 * bound export re-tightens them.
 */

#include "meshlog/meshlog.h"
#include "vdm_store.h"

namespace sculptcore::vdm {

struct VdmLogChunk : public meshlog::LogChunk {
  VdmStore *store = nullptr;
  VdmDelta delta;

  VdmLogChunk(VdmStore *store, VdmDelta &&delta)
      : meshlog::LogChunk(meshlog::LogChunkTypes::External), store(store),
        delta(std::move(delta))
  {
  }

  void undo(mesh::Mesh * /*m*/, spatial::SpatialTree * /*tree*/) override
  {
    if (store) {
      store->applyDelta(delta);
    }
  }
  void redo(mesh::Mesh * /*m*/, spatial::SpatialTree * /*tree*/) override
  {
    if (store) {
      store->applyDelta(delta);
    }
  }
  double memSize() override
  {
    double bytes = double(sizeof(VdmLogChunk));
    for (const VdmDelta::Entry &e : delta.entries) {
      bytes += double(e.texels.size() * sizeof(float3)) + sizeof(VdmDelta::Entry);
    }
    return bytes;
  }
};

} // namespace sculptcore::vdm
