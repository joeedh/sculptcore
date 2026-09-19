#include "meshlog_base.h"

namespace sculptcore::meshlog {

  void MeshLog::undo(mesh::Mesh *m, spatial::SpatialTree *tree)
  {
    if (curStep_ <= 0) {
      return;
    }

    curStep_--;

    if (curStep_ < 0 || curStep_ >= entries.size()) {
      return;
    }
    thawForTopoChunks(m);
    /* Undo chunks in REVERSE creation order so each element swap operates on the
       still-post-step topology, where its captured indices are all live. The topo
       chunk and the brush's LogChunkElems store no longer overlap on dyntopo-moved
       verts: stampUndoGate excludes them from the element store, leaving the topo
       chunk the sole, authoritative owner of their pre-step body. */
    auto &chunks = curEntry().chunks;
    for (int i = int(chunks.size()) - 1; i >= 0; i--) {
      chunks[i]->undo(m, tree);
    }
    swapActiveElems(curEntry());
    resyncNgonCount(m, curEntry());
  }


  void MeshLog::redo(mesh::Mesh *m, spatial::SpatialTree *tree)
  {
    if (curStep_ < 0 || curStep_ >= entries.size()) {
      return;
    }
    thawForTopoChunks(m);
    /* Forward creation order, one chunk fully applied before the next: each topo
       chunk reproduces its own dab's end-state (captured at deactivation, not
       end-of-step), so a face rewired across dabs is re-owned only after its
       dab's chunk recreates the verts it now references. */
    for (LogChunk *chunk : curEntry().chunks) {
      chunk->redo(m, tree);
    }
    swapActiveElems(curEntry());
    resyncNgonCount(m, curEntry());
    curStep_++;
  }


  void MeshLog::resyncNgonCount(mesh::Mesh *m, LogEntry &e)
  {
    if (!m) {
      return;
    }
    for (LogChunk *chunk : e.chunks) {
      if (chunk->type == LogChunkTypes::Topo) {
        m->recountNgons();
        return;
      }
    }
  }


  void MeshLog::swapActiveElems(LogEntry &e)
  {
    std::swap(active_vert_, e.snapActiveVert);
    std::swap(active_edge_, e.snapActiveEdge);
    std::swap(active_face_, e.snapActiveFace);
  }


  void MeshLog::thawForTopoChunks(mesh::Mesh *m)
  {
    if (!m || !m->topo_frozen) {
      return;
    }
    for (LogChunk *chunk : curEntry().chunks) {
      if (chunk->type == LogChunkTypes::Topo) {
        m->thawTopo();
        return;
      }
    }
  }


  void MeshLog::trimHistory()
  {
    if (maxUndoSteps_ < 0) {
      return;
    }
    while (int(entries.size()) > maxUndoSteps_ && curStep_ > 0) {
      entries.pop_front();
      curStep_--;
    }
  }


  void MeshLog::stampUndoGate(LogElemKind kind, int idx)
  {
    if (kind == LogElemKind::Vert) {
      /* Only a topology touch (this call site) newly stamping a previously-
       * unstamped vertex, while a preview dab is active, needs undoing on
       * rollback — the element store's OWN capture (a separate call site,
       * see LogChunkElems) also stamps this same gate but its row is never
       * rolled back, so it must never be un-stamped. See rollbackPreviewDab. */
      if (preview_.active && vertGate_.needsData(idx, curStrokeId(), 0xffff)) {
        preview_.gatedVert.append(idx);
      }
      vertGate_.updateSaved(idx, curStrokeId(), 0xffff);
    } else if (kind == LogElemKind::Face) {
      if (preview_.active && faceGate_.needsData(idx, curStrokeId(), 0xffff)) {
        preview_.gatedFace.append(idx);
      }
      faceGate_.updateSaved(idx, curStrokeId(), 0xffff);
    } else if (kind == LogElemKind::Corner) {
      // Keeps topo-touched corners out of the UV-reprojection element-store
      // capture: its undo replays after the topo restore and would stomp the
      // slot's restored pre-stroke corner with a foreign mid-stroke row.
      if (preview_.active && cornerGate_.needsData(idx, curStrokeId(), 0xffff)) {
        preview_.gatedCorner.append(idx);
      }
      cornerGate_.updateSaved(idx, curStrokeId(), 0xffff);
    }
  }

} // namespace sculptcore::meshlog
