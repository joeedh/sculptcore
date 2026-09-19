/**
# LogElem / LogChunkTopo

Full topological undo/redo log — see the meshlog intro (top of
`meshlog_base.h`) for the merged-record design this implements. Split out of
that file to keep it from growing into one monster header.
*/

#pragma once

#include "litestl/util/map.h"
#include "litestl/util/pool.h"
#include "litestl/util/vector.h"
#include "mesh/attribute.h"
#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "meshlog_row.h"
#include "meshlog_types.h"
#include "spatial/node.h"
#include "spatial/spatial.h"

#ifdef SCULPTCORE_WITH_ABSEIL
#include <../extern/abseil-cpp/absl/container/flat_hash_map.h>
#endif
#include <cstdint>
#include <cstdio>

namespace sculptcore::meshlog {
using litestl::util::Vector;

struct LogElem {
  int log_id;
  LogElemKind kind;
  LogOrigin origin;
  LogFate fate;
  int begin_mesh_index;
  int end_mesh_index;
  detail::ChunkElemRow *begin_body = nullptr;
  detail::ChunkElemRow *end_body = nullptr;
};

/**
 * Topological undo/redo chunk — see the file-level header comment for
 * the merged-record design.
 *
 * Records are appended in producer-event order, which is a valid
 * replay order because mesh topology ops fire create/kill events in
 * cascade order (kill_vertex fires the kill events for its incident
 * edges and faces before releasing the vert; make_face fires
 * create events for its corners after the underlying edges/verts
 * already exist).
 */

struct LogChunkTopo : public LogChunk {
  /* Slab sizes are a memory/allocation trade-off: chunks are per-dab, so an
   * 8000-slot slab retained ~1.3MB per barely-used chunk (hundreds per dyntopo
   * stroke) — far past the undo budget without the accounting seeing it. */
  util::Pool<LogElem, 512> records_pool;
  util::Pool<detail::ChunkElemRow, 256> bodies_pool;

  /* Shared row layouts, one per (kind, attr-count) seen by this chunk; rows
   * reference them by pointer, so they live exactly as long as the chunk. A
   * mid-step attr append changes the count and lazily gets a new layout;
   * earlier rows keep restoring their own prefix. */
  litestl::util::Vector<detail::RowLayout *, 8> layouts_;
  litestl::util::Vector<int, 8> layout_keys_; /* (int(kind) << 16) | count */

  detail::RowLayout *rowLayout(LogElemKind kind, mesh::AttrGroup &grp)
  {
    int key = (int(kind) << 16) | int(grp.attrs.size());
    for (int i = 0; i < int(layout_keys_.size()); i++) {
      if (layout_keys_[i] == key) {
        return layouts_[i];
      }
    }
    detail::RowLayout *l = litestl::alloc::New<detail::RowLayout>("meshlog RowLayout");
    l->build(grp);
    layout_keys_.append(key);
    layouts_.append(l);
    return l;
  }

  /** key: (uint8_t kind << 32) | uint32_t(mesh_index)  →  log_id */

#ifdef MESHLOG_ABSEIL_HASHMAP
  absl::flat_hash_map<int64_t, int> idx_to_log_id;
#else
  util::Map<int64_t, int64_t> idx_to_log_id;
#endif
  util::Map<int, LogElem *> by_log_id;
  int next_log_id = 0;

  LogChunkTopo() : LogChunk(LogChunkTypes::Topo)
  {
  }

  ~LogChunkTopo() override
  {
    // Explicit, and before the layouts go: ~ChunkElemRow releases its weight
    // slots through layout_->pool, and member pools would otherwise be destroyed
    // after this body — i.e. after the layouts they read.
    bodies_pool.clear();
    records_pool.clear();

    for (detail::RowLayout *l : layouts_) {
      litestl::alloc::Delete(l);
    }
  }

  void onCreate(LogElemKind kind, mesh::Mesh *m, int idx)
  {
    int64_t key = makeKey(kind, idx);

    // Defensive: stale mapping from a malformed prior sequence.
    int existing_id;
    if (lookupId(key, existing_id)) {
#ifndef MESHLOG_ABSEIL_HASHMAP
      idx_to_log_id.remove(key);
#else
      idx_to_log_id.erase(key);
#endif
    }

    LogElem *e = records_pool.alloc();
    e->log_id = next_log_id++;
    e->kind = kind;
    e->origin = LogOrigin::Created;
    e->fate = LogFate::Live;
    e->begin_mesh_index = idx;
    e->end_mesh_index = idx;
    e->begin_body = nullptr;
    e->end_body = nullptr;

#ifdef MESHLOG_ABSEIL_HASHMAP
    idx_to_log_id.emplace(key, e->log_id);
#else
    idx_to_log_id.insert(int64_t(key), int(e->log_id));
#endif
    by_log_id.insert(int(e->log_id), e);
  }

  void onChange(LogElemKind kind, mesh::Mesh *m, int idx)
  {
    int64_t key = makeKey(kind, idx);

    int existing_id;
    if (lookupId(key, existing_id)) {
      // Already a record for this element; nothing to do. Created records
      // snapshot at finalizeStep; Existed records already snapshotted on
      // first touch.
      return;
    }

    // First touch of a previously-existing element — take begin-snapshot.
    LogElem *e = records_pool.alloc();
    e->log_id = next_log_id++;
    e->kind = kind;
    e->origin = LogOrigin::Existed;
    e->fate = LogFate::Live;
    e->begin_mesh_index = idx;
    e->end_mesh_index = idx;
    e->begin_body = bodies_pool.alloc();
    e->end_body = nullptr;
    {
      mesh::AttrGroup &grp = group(m, kind);
      e->begin_body->captureFrom(rowLayout(kind, grp), grp, idx);
    }

#ifdef MESHLOG_ABSEIL_HASHMAP
    idx_to_log_id.emplace(int64_t(key), int(e->log_id));
#else
    idx_to_log_id.insert(int64_t(key), int(e->log_id));
#endif

    by_log_id.insert(int(e->log_id), e);
  }

  void onKill(LogElemKind kind, mesh::Mesh *m, int idx)
  {
    int64_t key = makeKey(kind, idx);

    int existing_id;
    if (lookupId(key, existing_id)) {
      LogElem *e = by_log_id.lookup(existing_id);

      if (e->origin == LogOrigin::Created) {
        // Create+kill within step: net no-op. Drop the record.
        dropRecord(e);
#ifdef MESHLOG_ABSEIL_HASHMAP
        idx_to_log_id.erase(key);
#else
        idx_to_log_id.remove(key);
#endif
        return;
      }

      // Existed && now Dead — begin_body already captured.
      e->fate = LogFate::Dead;
#ifdef MESHLOG_ABSEIL_HASHMAP
      idx_to_log_id.erase(key);
#else
      idx_to_log_id.remove(key);
#endif
      return;
    }

    // Killed without a prior change — snapshot now.
    LogElem *e = records_pool.alloc();
    e->log_id = next_log_id++;
    e->kind = kind;
    e->origin = LogOrigin::Existed;
    e->fate = LogFate::Dead;
    e->begin_mesh_index = idx;
    e->end_mesh_index = idx;
    e->begin_body = bodies_pool.alloc();
    e->end_body = nullptr;
    {
      mesh::AttrGroup &grp = group(m, kind);
      e->begin_body->captureFrom(rowLayout(kind, grp), grp, idx);
    }

    by_log_id.insert(int(e->log_id), e);
    // Do NOT map idx_to_log_id — element is dead.
  }

  /** Capture end-state for Created && Live records. Called from MeshLog::endStep. */
  void finalizeStep(mesh::Mesh *m)
  {
    for (LogElem &e : records_pool) {
      if (e.origin == LogOrigin::Created && e.fate == LogFate::Live) {
        if (!e.end_body) {
          e.end_body = bodies_pool.alloc();
        }
        mesh::AttrGroup &grp = group(m, e.kind);
        e.end_body->captureFrom(rowLayout(e.kind, grp), grp, e.end_mesh_index);
      }
    }
  }

  /* Refresh Created && Live VERT records' data columns (co/no/…) from the final
   * post-stroke mesh — see MeshLog::endStep. A vert created in an early dab and
   * then only brush-deformed (no connectivity touch) by later dabs had that
   * displacement dropped: the brush gate keeps created verts out of the element
   * store, and this chunk's end_body froze at its own dab's deactivation.
   * Connectivity stays frozen (later rewires are owned by later chunks' Existed
   * records). Dead/reused indices are skipped — their record is killed on redo,
   * so a refresh would be overwritten anyway. */
  void refreshCreatedVertData(mesh::Mesh *m)
  {
    mesh::AttrGroup &grp = m->v.attrs;
    for (LogElem &e : records_pool) {
      if (e.kind != LogElemKind::Vert || e.origin != LogOrigin::Created ||
          e.fate != LogFate::Live || !e.end_body)
      {
        continue;
      }
      int idx = e.end_mesh_index;
      if (idx < 0 || size_t(idx) >= m->v.capacity() || m->v.freemap[idx]) {
        continue;
      }
      e.end_body->refreshDataColumns(grp, idx);
    }
  }

  /* Face analogue of refreshCreatedVertData (see MeshLog::endStep). A face split
   * in from dyntopo in an early dab and then repainted by the poly-group / color
   * brush in a LATER dab had that face-attr change (poly `group`, …) dropped: the
   * brush gate keeps created faces out of the element store, and this chunk's
   * end_body froze at its own dab's deactivation. Refresh the Created-face data
   * columns (skips TOPO connectivity + NOCOPY temp state) from the final mesh so
   * redo restores the group the original stroke left. */
  void refreshCreatedFaceData(mesh::Mesh *m)
  {
    mesh::AttrGroup &grp = m->f.attrs;
    for (LogElem &e : records_pool) {
      if (e.kind != LogElemKind::Face || e.origin != LogOrigin::Created ||
          e.fate != LogFate::Live || !e.end_body)
      {
        continue;
      }
      int idx = e.end_mesh_index;
      if (idx < 0 || size_t(idx) >= m->f.capacity() || m->f.freemap[idx]) {
        continue;
      }
      e.end_body->refreshDataColumns(grp, idx);
    }
  }

  Vector<LogElem *> getSortedRecords()
  {
    Vector<LogElem *> records;
    records.clear();

    // build pre-sorted list of records
    // note we don't rely on the records_pool ordering
    records.ensure_capacity(records_pool.live_count());
    for (LogElem &e : records_pool) {
      records.append(&e);
    }
    records.sort(
        [](const LogElem *a, const LogElem *b) { return a->log_id - b->log_id; });
    return records;
  }

  /* The raw alloc/release/swap replay below bypasses the topology mutators
   * that keep derived boundary state current: restored elements carry their
   * persistent flags but no dirty marks, and the TEMP derived layers (UV-chart
   * edges, per-vert class) sit at defaults. Mark every live replayed vert/edge
   * boundary-dirty so the executors' lazy recompute re-derives them before the
   * next feature-aware stroke (dyntopo / bsmooth). O(records). */
  void markBoundaryDirty(mesh::Mesh *m, const Vector<LogElem *> &records)
  {
    for (LogElem *e : records) {
      const int idx =
          e->origin == LogOrigin::Created ? e->end_mesh_index : e->begin_mesh_index;
      if (e->kind == LogElemKind::Vert) {
        if (idx >= 0 && idx < int(m->v.capacity()) && !m->v.freemap[idx]) {
          mesh::boundary::markVertDirty(m, idx);
        }
      } else if (e->kind == LogElemKind::Edge) {
        if (idx >= 0 && idx < int(m->e.capacity()) && !m->e.freemap[idx]) {
          mesh::boundary::markEdgeDirty(m, idx);
        }
      }
    }
  }

  void undo(mesh::Mesh *m, spatial::SpatialTree *tree) override
  {
    Vector<LogElem *> records = getSortedRecords();

    /* The raw alloc/release below bypass make_face/kill_face, so the spatial
       tree's incremental face ownership (`.spatial.f.node`, a TEMP attr that
       ChunkElemRow does NOT log) is never updated by the restore itself. Drive
       the tree's add_face/remove_face here so its leaf face-sets + GPU buffers
       track the restored mesh; without it undo leaves a stale tree (nothing
       redrawn). No-op when undoing with no tree (the isolated operator tests). */
    if (tree) {
      /* Pre-pass (mesh still in post-step state, so connectivity is valid):
         drop ownership of faces about to be released or rewired, and of verts
         about to be released (else their leaf keeps a dangling unique_verts ref
         — the forward kill path never owned-removed them via callbacks). */

      for (LogElem *e : records) {
        if (e->origin == LogOrigin::Created && e->fate == LogFate::Live) {
          if (e->kind == LogElemKind::Face) {
            tree->flag_face_owner_normals(e->end_mesh_index);
            tree->remove_face(e->end_mesh_index, true);
          } else if (e->kind == LogElemKind::Vert)
            tree->remove_vert(e->end_mesh_index);
        } else if (e->kind == LogElemKind::Face && e->origin == LogOrigin::Existed &&
                   e->fate == LogFate::Live)
        {
          tree->flag_face_owner_normals(e->begin_mesh_index);
          tree->remove_face(e->begin_mesh_index, true);
        } else if (e->kind == LogElemKind::Corner && e->origin == LogOrigin::Existed &&
                   e->fate == LogFate::Live)
        {
          /* Row swap may re-point c.v (fan membership change with no face
             record); flag the current vert's owner skirt — the post-pass flags
             the restored side. */
          tree->flag_vert_skirt(m->c.v[e->begin_mesh_index]);
        }
      }
    }

    // Reverse order: undo dependents before underlying elements.
    for (int i = records.size() - 1; i >= 0; i--) {
      LogElem *e = records[i];
      mesh::ElemData *ed = elemData(m, e->kind);
      mesh::AttrGroup &grp = ed->attrs;

      if (e->origin == LogOrigin::Created && e->fate == LogFate::Live) {
        // It exists post-step; release it.
        ed->release(e->end_mesh_index);
      } else if (e->origin == LogOrigin::Existed && e->fate == LogFate::Live) {
        // Swap with live to revert to pre-step state.
        e->begin_body->swapWith(grp, e->begin_mesh_index);
      } else if (e->origin == LogOrigin::Existed && e->fate == LogFate::Dead) {
        // It was killed; bring it back at its original index.
        ed->alloc(e->begin_mesh_index);
        e->begin_body->writeTo(grp, e->begin_mesh_index);
      }
      // (Created && Dead) records were dropped at kill time.
    }

    if (tree) {
      // Post-pass (mesh now fully in pre-step state): re-own faces that came
      // back or were rewired. add_face re-derives the leaf and flags it for
      // tris/bounds/GPU regen.
      for (LogElem *e : records) {
        if (e->kind == LogElemKind::Corner && e->origin == LogOrigin::Existed &&
            e->fate == LogFate::Live)
        {
          // Restored side of a corner-row swap (see the pre-pass).
          tree->flag_vert_skirt(m->c.v[e->begin_mesh_index]);
          continue;
        }
        if (e->kind != LogElemKind::Face)
          continue;
        if (e->origin == LogOrigin::Existed && e->fate == LogFate::Dead) {
          tree->add_face(e->begin_mesh_index);
          tree->flag_face_owner_normals(e->begin_mesh_index);
        } else if (e->origin == LogOrigin::Existed && e->fate == LogFate::Live) {
          // double check face is in tree
          if (tree->treeMesh.f.node[e->begin_mesh_index] == 0) {
            tree->add_face(e->begin_mesh_index);
          }
          tree->flag_face_owner_normals(e->begin_mesh_index);
        }
      }
    }

    markBoundaryDirty(m, records);
  }

  void redo(mesh::Mesh *m, spatial::SpatialTree *tree) override
  {
    Vector<LogElem *> records = getSortedRecords();

    if (tree) {
      /* Pre-pass (mesh in pre-step state): drop ownership of faces about to be
         released or rewired, and of verts about to be released — else their
         leaf keeps a dangling unique_verts ref that an index-reusing recreate
         would resurrect into a double-owned vert. */
      for (LogElem *e : records) {
        if (e->origin != LogOrigin::Existed)
          continue;
        if (e->kind == LogElemKind::Face && e->fate == LogFate::Dead) {
          tree->flag_face_owner_normals(e->begin_mesh_index);
          tree->remove_face(e->begin_mesh_index, true);
        } else if (e->kind == LogElemKind::Face && e->fate == LogFate::Live) {
          if (tree->treeMesh.f.node[e->begin_mesh_index] != 0) {
            tree->flag_face_owner_normals(e->begin_mesh_index);
            tree->remove_face(e->begin_mesh_index, true);
          }
        } else if (e->kind == LogElemKind::Vert && e->fate == LogFate::Dead) {
          tree->remove_vert(e->begin_mesh_index);
        } else if (e->kind == LogElemKind::Corner && e->fate == LogFate::Live) {
          // Pre-swap side of a corner-row swap (see undo's pre-pass).
          tree->flag_vert_skirt(m->c.v[e->begin_mesh_index]);
        }
      }
    }

    // Forward order: allocate underlying before dependents reference them.
    for (int i = 0; i < records.size(); i++) {
      LogElem *e = records[i];
      mesh::ElemData *ed = elemData(m, e->kind);
      mesh::AttrGroup &grp = ed->attrs;

      if (e->origin == LogOrigin::Created && e->fate == LogFate::Live) {
        // Recreate at the recorded mesh index.
        ed->alloc(e->end_mesh_index);
        e->end_body->writeTo(grp, e->end_mesh_index);
      } else if (e->origin == LogOrigin::Existed && e->fate == LogFate::Live) {
        // Swap toggle — the body now holds the pre-step state, the live
        // mesh gets the post-step state back.
        e->begin_body->swapWith(grp, e->begin_mesh_index);
      } else if (e->origin == LogOrigin::Existed && e->fate == LogFate::Dead) {
        // It was killed during the step.
        ed->release(e->begin_mesh_index);
      }
    }

    if (tree) {
      // Post-pass (mesh in post-step state): re-own recreated/rewired faces.
      for (LogElem *e : records) {
        if (e->kind == LogElemKind::Corner && e->origin == LogOrigin::Existed &&
            e->fate == LogFate::Live)
        {
          // Restored (post-step) side of a corner-row swap.
          tree->flag_vert_skirt(m->c.v[e->begin_mesh_index]);
          continue;
        }
        if (e->kind != LogElemKind::Face)
          continue;
        if (e->origin == LogOrigin::Created && e->fate == LogFate::Live) {
          if (tree->treeMesh.f.node[e->end_mesh_index] == 0) {
            tree->add_face(e->end_mesh_index);
          }
          tree->flag_face_owner_normals(e->end_mesh_index);
        } else if (e->origin == LogOrigin::Existed && e->fate == LogFate::Live) {
          if (tree->treeMesh.f.node[e->begin_mesh_index] == 0) {
            tree->add_face(e->begin_mesh_index);
          }
          tree->flag_face_owner_normals(e->begin_mesh_index);
        }
      }
    }

    markBoundaryDirty(m, records);
  }

  // returns double (not size_t) because the prototype is inferred from
  // the TS binding side
  double memSize() override
  {
    double tot = double(sizeof(*this));
    // Slab pools retain full-slab capacity, not just live objects — count it,
    // or a stroke's chunks blow past the undo budget invisibly.
    tot += double(records_pool.capacity()) * double(sizeof(LogElem));
    tot += double(bodies_pool.capacity()) * double(sizeof(detail::ChunkElemRow));
    for (detail::RowLayout *l : layouts_) {
      tot += l->memSize();
    }
    // Rough per-entry hash-map overhead.
    tot += double(idx_to_log_id.size() * 3 * 16) + double(by_log_id.size() * 3 * 16);

    // Row heap payloads (the slot itself is already in the capacity term).
    for (auto &e : bodies_pool) {
      tot += e.memSize() - double(sizeof(e));
    }

    return tot;
  }

  static int64_t makeKey(LogElemKind kind, int idx)
  {
    return (int64_t(uint8_t(kind)) << 32) | int64_t(uint32_t(idx));
  }

  static mesh::ElemData *elemData(mesh::Mesh *m, LogElemKind kind)
  {
    switch (kind) {
    case LogElemKind::Vert:
      return static_cast<mesh::ElemData *>(&m->v);
    case LogElemKind::Edge:
      return static_cast<mesh::ElemData *>(&m->e);
    case LogElemKind::Corner:
      return static_cast<mesh::ElemData *>(&m->c);
    case LogElemKind::List:
      return static_cast<mesh::ElemData *>(&m->l);
    case LogElemKind::Face:
      return static_cast<mesh::ElemData *>(&m->f);
    }
    return nullptr;
  }

  static mesh::AttrGroup &group(mesh::Mesh *m, LogElemKind kind)
  {
    return elemData(m, kind)->attrs;
  }

private:
  bool lookupId(int64_t key, int &out_id)
  {
#ifdef MESHLOG_ABSEIL_HASHMAP
    auto it = idx_to_log_id.find(key);
    int *p = nullptr;
    if (it != idx_to_log_id.end()) {
      p = &it->second;
    }
#else
    int64_t *p = idx_to_log_id.lookup_ptr(key);
#endif
    if (!p) {
      return false;
    }
    out_id = *p;
    return true;
  }

  void dropRecord(LogElem *e)
  {
    by_log_id.remove(e->log_id);
    if (e->begin_body) {
      bodies_pool.release(e->begin_body);
    }
    if (e->end_body) {
      bodies_pool.release(e->end_body);
    }
    if (!records_pool.release(e)) {
      printf("log elem double free!\n");
    }
  }
};

} // namespace sculptcore::meshlog
