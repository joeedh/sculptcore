#include "meshlog_base.h"

namespace sculptcore::meshlog {

  MeshLog::MeshLog()
  {
    entries.grow_one();
    curStep_ = 0;
    installCallbacks();
  }


  mesh::MeshCallbacks *MeshLog::callbacks()
  {
    return &cb_;
  }


  void MeshLog::setActiveMesh(mesh::Mesh *m)
  {
    if (m != active_mesh_) {
      /* Stamps index the active mesh's element ids; a mesh switch makes them
       * meaningless — invalidate wholesale. */
      chunk_stamp_.bump();
    }
    active_mesh_ = m;
    // Bind the brush's save-gate columns up front (before any dab op fires a
    // callback) so stampUndoGate never allocs mid-stroke. See stampUndoGate.
    if (m) {
      vertGate_.ensure(*m);
      faceGate_.ensure(*m);
      cornerGate_.ensure(*m);
      // Picked up, never created: a mesh with no weights should not grow a pool
      // because it was logged. Held so totalMemSize can size it after the mesh
      // itself is gone.
      deform_pool_.reset(m->deformPoolOrNull());
    }
  }


  void MeshLog::beginStep(bool hasDyntopo)
  {
    if (curStep_ != entries.size()) {
      // theoretically this should call all the right destructors
      entries.resize(curStep_);
    }
    entries.grow_one();
    entries.last().id = nextStepId_++;
    // Snapshot the pre-step active elements; undo/redo swap them back (see
    // swapActiveElems). Captured here so any setActiveElem during the step is the
    // post-step value the redo restores.
    entries.last().snapActiveVert = active_vert_;
    entries.last().snapActiveEdge = active_edge_;
    entries.last().snapActiveFace = active_face_;
    // A stroke pushes exactly one step, so bump the stroke id here. Masked to
    // 16 bits at read; only equality against the stamp within a step matters,
    // so the 65536-stroke wrap is harmless (see AttrSaver).
    strokeId_++;
    if (hasDyntopo) {
      pushTopoChunk();
    }
  }


  int MeshLog::curStrokeId() const
  {
    return strokeId_ & 0xffff;
  }


  int MeshLog::lastStepId()
  {
    return entries.size() > 0 ? entries.last().id : -1;
  }


  bool MeshLog::hasOpenStepFor(const mesh::Mesh *mesh, int id) const
  {
    return (!active_mesh_ || active_mesh_ == mesh) && curStep_ >= 0 &&
           curStep_ < entries.size() && entries[curStep_].id == id &&
           !entries[curStep_].finalized;
  }


  double MeshLog::stepMemSize(int id)
  {
    for (auto &entry : entries) {
      if (entry.id == id) {
        return entry.memSize();
      }
    }
    return 0.0;
  }


  double MeshLog::totalMemSize()
  {
    double tot = 0.0;
    for (auto &entry : entries) {
      tot += entry.memSize();
    }
    return tot + deformPoolMemSize();
  }


  double MeshLog::deformPoolMemSize()
  {
    return deform_pool_.ptr ? double(deform_pool_.ptr->byteSize()) : 0.0;
  }


  int MeshLog::entryCount()
  {
    return int(entries.size());
  }


  int MeshLog::freeStep(int id)
  {
    int idx = -1;
    for (int i = 0; i < int(entries.size()); i++) {
      if (entries[i].id == id) {
        idx = i;
        break;
      }
    }
    if (idx < 0 || idx >= curStep_) {
      return 0;
    }
    {
      // Move the dropped entry out so its dtor frees the chunks, then shift
      // the tail left (move-assign; raw chunk pointers transfer ownership).
      LogEntry dropped = std::move(entries[idx]);
      for (int i = idx; i < int(entries.size()) - 1; i++) {
        entries[i] = std::move(entries[i + 1]);
      }
      entries.pop_back();
    }
    curStep_--;
    return 1;
  }


  void MeshLog::finalizeStroke()
  {
    if (curEntry().finalized) {
      return;
    }
    // Only the still-active (last) topo chunk needs finalizing here; earlier
    // chunks were finalized at deactivation by pushTopoChunk.
    if (curEntry().topo_chunk_ && active_mesh_) {
      curEntry().topo_chunk_->finalizeStep(active_mesh_);
    }
    /* Created verts from earlier dabs may have been brush-deformed again by
       later dabs without a connectivity touch; the brush gate kept those
       displacements out of the element store and each chunk's end_body froze at
       its own dab. Refresh every topo chunk's Created-vert positions from the
       final mesh so redo lands exactly where the original stroke did. */
    if (active_mesh_) {
      for (LogChunk *chunk : curEntry().chunks) {
        if (chunk->type == LogChunkTypes::Topo) {
          static_cast<LogChunkTopo *>(chunk)->refreshCreatedVertData(active_mesh_);
          static_cast<LogChunkTopo *>(chunk)->refreshCreatedFaceData(active_mesh_);
        }
      }
    }
    curEntry().topo_chunk_ = nullptr;
    curEntry().finalized = true;
  }


  void MeshLog::endStep()
  {
    finalizeStroke();
    curStep_++;
    trimHistory();
  }


  void MeshLog::setMaxUndoSteps(int n)
  {
    maxUndoSteps_ = n;
    trimHistory();
  }


  int MeshLog::maxUndoSteps() const
  {
    return maxUndoSteps_;
  }


  bool MeshLog::hasTopoChunk() const
  {
    return curEntry().hasTopoChunk;
  }


  void MeshLog::pushTopoChunk()
  {
    if (curStep_ < 0 || curStep_ >= entries.size()) {
      fprintf(stderr, "Error: getTopoChunk called with no current undo entry\n");
      abort();
    }
    /* Finalize the outgoing chunk NOW (end of its dab), not at endStep: each
       chunk must capture its Created records' end-state while it is still the
       active chunk. Capturing at end-of-step instead would snapshot connectivity
       a LATER dab rewired (referencing verts a later chunk creates), so per-chunk
       redo would re-own a face before its verts exist. */
    if (curEntry().topo_chunk_ && active_mesh_) {
      curEntry().topo_chunk_->finalizeStep(active_mesh_);
    }
    curEntry().topo_chunk_ = litestl::alloc::New<LogChunkTopo>("LogChunkTopo");
    curEntry().hasTopoChunk = true;
    curEntry().chunks.append(curEntry().topo_chunk_);
    bumpChunkStampGen();
  }


  LogChunkTopo *MeshLog::getTopoChunk()
  {
    if (curEntry().topo_chunk_) {
      return curEntry().topo_chunk_;
    }
    pushTopoChunk();
    return curEntry().topo_chunk_;
  }


  LogChunkElems *MeshLog::elemStore(mesh::ElemType domain)
  {
    if (curStep_ < 0 || curStep_ >= entries.size()) {
      fprintf(stderr, "Error: elemStore called with no current undo entry\n");
      abort();
    }
    for (LogChunk *chunk : curEntry().chunks) {
      if (chunk->type != LogChunkTypes::Elems) {
        continue;
      }
      LogChunkElems *store = static_cast<LogChunkElems *>(chunk);
      if (store->domain == domain) {
        return store;
      }
    }
    auto *store = litestl::alloc::New<LogChunkElems>("LogChunkElems", domain);
    curEntry().chunks.append(store);
    return store;
  }


  LogChunkReorder *MeshLog::pushReorderChunk(Vector<int> vmap,
                                    Vector<int> emap,
                                    Vector<int> cmap,
                                    Vector<int> lmap,
                                    Vector<int> fmap)
  {
    if (curStep_ < 0 || curStep_ >= entries.size()) {
      fprintf(stderr, "Error: pushReorderChunk called with no current undo entry\n");
      abort();
    }
    auto *chunk = litestl::alloc::New<LogChunkReorder>("LogChunkReorder",
                                                       std::move(vmap),
                                                       std::move(emap),
                                                       std::move(cmap),
                                                       std::move(lmap),
                                                       std::move(fmap));
    curEntry().chunks.append(chunk);
    return chunk;
  }


  void MeshLog::appendChunk(LogChunk *chunk)
  {
    if (curStep_ < 0 || curStep_ >= entries.size()) {
      fprintf(stderr, "Error: appendChunk called with no current undo entry\n");
      abort();
    }
    curEntry().chunks.append(chunk);
  }


  LogChunkReorder *MeshLog::pushReorderStep(Vector<int> vmap,
                                   Vector<int> emap,
                                   Vector<int> cmap,
                                   Vector<int> lmap,
                                   Vector<int> fmap)
  {
    beginStep(false);
    auto *chunk = pushReorderChunk(std::move(vmap),
                                   std::move(emap),
                                   std::move(cmap),
                                   std::move(lmap),
                                   std::move(fmap));
    endStep();
    return chunk;
  }


  void MeshLog::reorderForLocality(spatial::SpatialTree *tree)
  {
    if (!tree) {
      return;
    }
    Vector<int> vmap, emap, cmap, lmap, fmap;
    tree->computeLocalityMaps(vmap, emap, cmap, lmap, fmap);
    pushReorderStep(vmap, emap, cmap, lmap, fmap);
    tree->applyReorderIncremental(vmap, emap, cmap, lmap, fmap);
    /* Element ids just moved; the recorded-in-chunk stamps are id-keyed. */
    chunk_stamp_.bump();
  }


  bool MeshLog::compactIfFragmented(spatial::SpatialTree *tree, double vertRatioThreshold)
  {
    if (!tree || curStep_ < 0 || curStep_ >= entries.size()) {
      return false;
    }
    if (tree->fragmentationStats().vertRatio < vertRatioThreshold) {
      return false;
    }
    /* Scoped (mechanism-B) compaction: relocate only the fragmented region, apply
     * O(region). The undo chunk stores the map SPARSELY — only the moved slots'
     * target values (O(moved)), not the full capacity-sized bijection — and
     * reconstructs it transiently on undo/redo. The scoped forward leaves the exact
     * mesh + tree state a full apply would (proven by test_partial_matches_full). */
    Vector<spatial::SpatialNode *> dirty;
    tree->selectFragmentedLeaves(2.0, dirty);
    if (dirty.size() == 0) {
      return false;
    }
    Vector<int> vmap, emap, cmap, lmap, fmap;
    Vector<int> moved[5];
    tree->computeLocalityMapsPartial(dirty, vmap, emap, cmap, lmap, fmap, moved);

    /* Freeze the stroke's topo chunks NOW, against the pre-reorder mesh, so their
     * end_body holds pre-reorder connectivity (the redo path replays them before
     * reorder.redo). endStep's finalize is then a no-op via the `finalized` guard. */
    finalizeStroke();

    auto *chunk = litestl::alloc::New<LogChunkReorder>("LogChunkReorder");
    chunk->scoped = true;
    Vector<int> *maps[5] = {&vmap, &emap, &cmap, &lmap, &fmap};
    Vector<int> *mvs[5] = {&chunk->mv, &chunk->me, &chunk->mc, &chunk->ml, &chunk->mf};
    Vector<int> *vals[5] = {
        &chunk->vval, &chunk->eval, &chunk->cval, &chunk->lval, &chunk->fval};
    for (int k = 0; k < 5; k++) {
      *mvs[k] = moved[k]; // moved slots (from)
      vals[k]->resize(int(moved[k].size()));
      for (int i = 0; i < int(moved[k].size()); i++) {
        (*vals[k])[i] = (*maps[k])[moved[k][i]]; // target slots (to)
      }
    }
    curEntry().chunks.append(chunk);

    tree->applyReorderIncremental(
        vmap, emap, cmap, lmap, fmap, moved[0], moved[1], moved[2], moved[3], moved[4]);
    /* Element ids just moved; the recorded-in-chunk stamps are id-keyed. */
    chunk_stamp_.bump();
    return true;
  }


  void MeshLog::bumpChunkStampGen()
  {
    chunk_stamp_.bump();
  }


  void MeshLog::installCallbacks()
  {
    auto fwd = [this](LogElemKind kind) {
      return [this, kind](int idx) {
        if (!active_mesh_) {
          return;
        }
        /* Fast path: element already recorded by the active chunk — nothing
         * to capture, and its undo gate was stamped on first touch. */
        if (chunk_stamp_.hit(kind, idx)) {
          return;
        }
        getTopoChunk()->onChange(kind, active_mesh_, idx);
        chunk_stamp_.set(kind, idx);
        stampUndoGate(kind, idx);
      };
    };
    auto fwdCreate = [this](LogElemKind kind) {
      return [this, kind](int idx) {
        if (!active_mesh_) {
          return;
        }
        getTopoChunk()->onCreate(kind, active_mesh_, idx);
        chunk_stamp_.set(kind, idx);
        stampUndoGate(kind, idx);
      };
    };
    auto fwdKill = [this](LogElemKind kind) {
      return [this, kind](int idx) {
        if (!active_mesh_) {
          return;
        }
        getTopoChunk()->onKill(kind, active_mesh_, idx);
        chunk_stamp_.clear(kind, idx);
      };
    };

    cb_.onVertCreate = fwdCreate(LogElemKind::Vert);
    cb_.onVertChange = fwd(LogElemKind::Vert);
    cb_.onVertKill = fwdKill(LogElemKind::Vert);

    cb_.onEdgeCreate = fwdCreate(LogElemKind::Edge);
    cb_.onEdgeChange = fwd(LogElemKind::Edge);
    cb_.onEdgeKill = fwdKill(LogElemKind::Edge);

    cb_.onCornerCreate = fwdCreate(LogElemKind::Corner);
    cb_.onCornerChange = fwd(LogElemKind::Corner);
    cb_.onCornerKill = fwdKill(LogElemKind::Corner);

    cb_.onListCreate = fwdCreate(LogElemKind::List);
    cb_.onListChange = fwd(LogElemKind::List);
    cb_.onListKill = fwdKill(LogElemKind::List);

    cb_.onFaceCreate = fwdCreate(LogElemKind::Face);
    cb_.onFaceChange = fwd(LogElemKind::Face);
    cb_.onFaceKill = fwdKill(LogElemKind::Face);
  }

} // namespace sculptcore::meshlog
