#include "meshlog_base.h"

namespace sculptcore::meshlog {

  bool MeshLog::previewActive() const
  {
    return preview_.active;
  }


  void MeshLog::commitPreviewDab()
  {
    preview_.active = false;
    preview_.vertIdx.clear();
    preview_.vertRows.clear();
    preview_.gatedVert.clear();
    preview_.gatedFace.clear();
    preview_.gatedCorner.clear();
    preview_.seenIdx.clear();
  }


  void MeshLog::capturePreviewNodes(mesh::Mesh *m, std::span<spatial::SpatialNode *> nodes)
  {

    mesh::AttrGroup &grp = m->v.attrs;

    Vector<mesh::AttrRef> elemRefs;
    for (mesh::AttrRef &ref : grp.attrs) {
      if (ref.flag & (mesh::AttrFlag::NOCOPY | mesh::AttrFlag::TOPO)) {
        continue;
      }
      elemRefs.append(ref);
    }
    litestl::util::span<const mesh::AttrRef> elemRefSpan(elemRefs.data(),
                                                         elemRefs.size());

    for (spatial::SpatialNode *node : nodes) {
      for (int v : node->unique_verts()) {
        if (v < 0 || size_t(v) >= m->v.capacity() || m->v.freemap[v]) {
          continue;
        }
        if (!preview_.seenIdx.add(v)) {
          continue;
        }
        preview_.vertIdx.append(v);
        preview_.vertRows.grow_one();
        preview_.vertRows.last().captureFrom(&preview_.vertLayout, grp, v);

        if (vertGate_.needsData(v, curStrokeId(), 0xffff)) {
          elemStore(mesh::ElemType::VERTEX)->data.appendFrom(grp, v, elemRefSpan);
          vertGate_.updateSaved(v, curStrokeId(), 0xffff);
        }
      }
    }
  }


  void
  MeshLog::beginPreviewDab(mesh::Mesh *m, spatial::SpatialTree *tree, float3 center, float radius)
  {
    if (!m || !tree) {
      return;
    }
    /* Anchored/Drag Dot calls this BEFORE the tick's applyDab(), which is
     * normally what binds the vertGate_/faceGate_ .strokeid columns via
     * setActiveMesh(). capturePreviewRegion() below reads vertGate_ directly,
     * so bind it here too or the first preview dab of a stroke reads an
     * unensured builtin attribute column and crashes. Idempotent/cheap when
     * already bound to this mesh. */
    setActiveMesh(m);
    /* pushTopoChunk() (called unconditionally at the end of every applyDab)
     * always appends a fresh, still-empty chunk for the NEXT dab to reuse —
     * so if one is already sitting there as topo_chunk_, it's already
     * counted in chunks.size() even though this preview dab's topology
     * writes will land inside that very chunk (getTopoChunk() reuses it
     * rather than appending a new one). Exclude it from the baseline so
     * rollback's pop loop below undoes it too. */
    preview_.chunkBaseline = curEntry().chunks.size() - (curEntry().topo_chunk_ ? 1 : 0);
    preview_.vertIdx.clear();
    preview_.vertRows.clear();
    preview_.gatedVert.clear();
    preview_.gatedFace.clear();
    preview_.gatedCorner.clear();
    preview_.seenIdx.clear();

    mesh::AttrGroup &grp = m->v.attrs;
    // Topology chunks restore connectivity; frozen columns have no readable pages.
    preview_.vertLayout.build(grp, true);

    capturePreviewRegion(m, tree, center, radius);

    /* Flip active on only now, after the row snapshot above, so stampUndoGate
     * doesn't mistake this snapshot pass for a preview-dab touch. */
    preview_.active = true;
  }


  void
  MeshLog::extendPreviewDab(mesh::Mesh *m, spatial::SpatialTree *tree, float3 center, float radius)
  {
    if (!m || !tree) {
      return;
    }
    if (!preview_.active) {
      beginPreviewDab(m, tree, center, radius);
      return;
    }
    capturePreviewRegion(m, tree, center, radius);
  }


  void MeshLog::rollbackPreviewDab(mesh::Mesh *m, spatial::SpatialTree *tree)
  {
    if (!preview_.active || !m) {
      preview_.active = false;
      return;
    }

    /* The preview dab's own applyDab() just froze topology on the way out
     * (its normal per-dab bracket); the topo-chunk undo below walks live
     * TOPO links (disk/radial cycles) via raw alloc/release, same as
     * MeshLog::undo() -- thaw first or it silently no-ops on unmaterialized
     * pages (see thawForTopoChunks). */
    thawForTopoChunks(m);

    /* Only LogChunkTopo chunks belong to this preview dab (see class comment);
     * a LogChunkElems chunk can land in this same index range when this is the
     * step's first dab (the step-wide, gated-once-per-stroke capture happens
     * to fire during it) -- that chunk owns the pre-STEP baseline the real
     * MeshLog::undo() needs later, and must survive every preview rollback in
     * this stroke, not just this one. Walk top-down and excise only the Topo
     * chunks, leaving everything else (and its relative order) untouched. */
    auto &chunks = curEntry().chunks;
    for (int i = int(chunks.size()) - 1; i >= int(preview_.chunkBaseline); i--) {
      LogChunk *c = chunks[i];
      if (c->type != LogChunkTypes::Topo && c->type != LogChunkTypes::PreparedData) {
        continue;
      }
      c->undo(m, tree);
      chunks.remove_at(i, false);
      litestl::alloc::Delete(c);
    }
    /* The active topo chunk (if any) was just deleted above; the next touch
     * lazily allocates a fresh one via getTopoChunk(). Bump the per-chunk
     * dedup stamps too, since the deleted chunk's recorded elements must be
     * eligible to record again in whatever chunk comes next. */
    curEntry().topo_chunk_ = nullptr;
    bumpChunkStampGen();

    /* The just-deleted chunk(s) stamped vertGate_/faceGate_ (via stampUndoGate)
     * for every element they topologically touched — that stamp tells the
     * brush's LogChunkElems capture "a topo chunk already owns this element's
     * pre-step body, skip me". With the chunk gone, nothing owns it anymore,
     * so every element stampUndoGate newly gated during this preview dab (as
     * opposed to the element store's own, non-rolled-back capture, which
     * stamps the same gate through a different call site) must be un-gated —
     * see stampUndoGate. */
    for (int idx : preview_.gatedVert) {
      if (idx >= 0 && size_t(idx) < m->v.capacity()) {
        vertGate_.resetElem(idx);
      }
    }
    for (int idx : preview_.gatedFace) {
      if (idx >= 0 && size_t(idx) < m->f.capacity()) {
        faceGate_.resetElem(idx);
      }
    }
    for (int idx : preview_.gatedCorner) {
      if (idx >= 0 && size_t(idx) < m->c.capacity()) {
        cornerGate_.resetElem(idx);
      }
    }

    mesh::AttrGroup &grp = m->v.attrs;
    for (int i = 0; i < int(preview_.vertIdx.size()); i++) {
      int idx = preview_.vertIdx[i];
      if (idx < 0 || size_t(idx) >= m->v.capacity() || m->v.freemap[idx]) {
        continue;
      }
      // ChunkElemRow::writeTo zeroes NOCOPY cells (.spatial.v.node) by contract --
      // correct for LogChunkTopo recreation (ownership re-derived after), wrong
      // here since this vert was never recreated. Preserve the live ownership stamp.
      int savedNode = tree ? tree->treeMesh.v.node[idx] : 0;
      preview_.vertRows[i].writeTo(grp, idx);
      if (tree) {
        tree->treeMesh.v.node[idx] = savedNode;
        if (savedNode) {
          using namespace sculptcore::spatial;
          SpatialNode *node = tree->node_from_id(savedNode);
          node->update(NodeFlags::Spatial_UpdateGPU | NodeFlags::Spatial_RegenBounds |
                       NodeFlags::Spatial_UpdateNormals);
        }
      }
    }

    preview_.active = false;
    preview_.seenIdx.clear();
  }

} // namespace sculptcore::meshlog
