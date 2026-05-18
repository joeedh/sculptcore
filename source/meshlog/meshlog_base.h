/**
# Intro

Meshlog is the main undo/redo system for sculptcore. It has two chunk types:

* `LogChunkSimple` — per-spatial-node attribute-swap log for plain
  vertex-position sculpting. Captures a fixed set of attributes once
  per node and swaps them with live on undo/redo.

* `LogChunkTopo` — full topological log. Records every element touched
  during a step (Create / Change / Kill of verts, edges, corners,
  face-loop lists, and faces) and replays the events to either bring
  the mesh forward (redo) or backward (undo) by one step.

# LogChunkTopo: merged per-element records

Each logical element touched during a step gets at most one record in
the chunk. A record is classified by:

* **Origin**: `Existed` if the element was already alive at step-begin,
  `Created` if it was born during the step.
* **Fate**: `Live` if it survives to step-end, `Dead` if it was killed
  during the step.

A record carries up to two single-row attribute snapshots:

* **begin_body** — pre-step state, captured at first touch. Populated
  only for `Existed` records. Used for undo of `Existed && Dead`
  (alloc + writeTo) and as the swap pivot for `Existed && Live`.
* **end_body** — post-step state, captured at `finalizeStep`.
  Populated only for `Created && Live` records.

`Created && Dead` records (born and killed in the same step) are
**dropped** at kill time — the natural set-theoretic consequence of
"no net change to step-begin or step-end state", not a special
cancellation rule.

Mesh indices are reused from the freelist when an element is killed.
A log-local id generator plus an `(elem_kind, mesh_index) → log_id`
map disambiguates re-use within one step: kill of idx N unmaps it, a
subsequent create at idx N gets a fresh log_id and its own record. The
two records coexist (kill-first, create-second) and replay correctly.
*/

#pragma once

#include "binding/binding_constructor_builder.h"
#include "litestl/math/vector.h"
#include "litestl/util/map.h"
#include "litestl/util/pool.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"
#include "mesh/attribute.h"
#include "mesh/attribute_bool.h"
#include "mesh/attribute_builtin.h"
#include "mesh/attribute_enums.h"
#include "mesh/mesh.h"
#include "mesh/mesh_callbacks.h"
#include "spatial/node.h"
#include "spatial/spatial.h"

#include <cstdint>

namespace sculptcore::meshlog {
using litestl::math::float3;
using litestl::util::string;
using litestl::util::Vector;

enum _LogChunkTypes {
  Simple = 0,
  Topo = 1,
};
MAKE_ENUM_CLASS(LogChunkTypes, _LogChunkTypes, int);

enum class LogElemKind : uint8_t { Vert = 0, Edge = 1, Corner = 2, List = 3, Face = 4 };
enum class LogOrigin : uint8_t { Existed, Created };
enum class LogFate : uint8_t { Live, Dead };

struct LogChunk {
  LogChunkTypes type;
  LogChunk(LogChunkTypes type) : type(type)
  {
  }
  virtual ~LogChunk()
  {
  }
  virtual void undo(mesh::Mesh *m, spatial::SpatialTree *tree)
  {
  }
  virtual void redo(mesh::Mesh *m, spatial::SpatialTree *tree)
  {
  }
};

namespace detail {
struct ChunkElemData {
  mesh::BuiltinAttr<int, ".sculpt.origIndex"> origIndex;
  bool isSwapped = false;

  ChunkElemData(int size) : size_(size)
  {
    attrs_.ensure_capacity(size);
    origIndex.ensure(attrs_, true);
  }

  ChunkElemData &addAttr(int srcAttrIndex, mesh::AttrType type, string name)
  {
    attrs_.ensure(type, name, true);
    srcAttrMap_.append(srcAttrIndex);
    return *this;
  }

  ChunkElemData &ensureAttr(int srcAttrIndex, mesh::AttrType type, string name)
  {
    for (auto &attr : attrs_.attrs) {
      if (attr.name == name && attr.type == type) {
        return *this;
      }
    }
    addAttr(srcAttrIndex, type, name);
    return *this;
  }
  ChunkElemData &ensureAttr(const mesh::AttrGroup &src, const mesh::AttrRef &ref)
  {
    int index = -1;
    int i = 0;
    for (auto &ref2 : src.attrs) {
      if (ref2.name == ref.name && ref2.type == ref.type) {
        index = i;
        break;
      }
      i++;
    }
    if (index == -1) {
      fprintf(stderr,
              "attribute %s (type %d) is not in mesh\n",
              ref.name.c_str(),
              int(ref.type));
      abort();
    }
    return ensureAttr(index, ref.type, ref.name);
  }

  // TODO: figure out concept for iterator<int>
  template <typename ITER>
  ATTR_NO_OPT void cpyFrom(const mesh::AttrGroup &src, ITER &elements)
  {
    int index = 0;
    for (int i : elements) {
      cpyFrom(src, i, index);
      index++;
    }
  }

  ATTR_NO_OPT void cpyFrom(const mesh::AttrGroup &src, int src_i, int dst_i)
  {
    using namespace sculptcore::mesh;

    origIndex[dst_i] = src_i;

    for (int i = 0; i < srcAttrMap_.size(); i++) {
      int srcAttrIndex = srcAttrMap_[i];
      const auto &ref = src.attrs[srcAttrIndex];
      auto &dstData = attrs_.attrs[i + 1].data;
      const auto &srcData = ref.data;

      if (ref.type == AttrType::BOOL) {
        BoolAttrView *view = static_cast<BoolAttrView *>(ref.data);
        BoolAttrView *dstView = static_cast<BoolAttrView *>(dstData);

        dstView->set(dst_i, view->get(src_i));
        continue;
      }

      mesh::AttrData<float3> *dstdst = static_cast<mesh::AttrData<float3> *>(dstData);
      memcpy(dstData->getElemData(dst_i), srcData->getElemData(src_i), dstData->elemSize);
    }
  };

  ATTR_NO_OPT
  void swapWith(const mesh::AttrGroup &src, int src_i, int dst_i)
  {
    using namespace sculptcore::mesh;
    char buf[64];

    for (int i = 0; i < srcAttrMap_.size(); i++) {
      int srcAttrIndex = srcAttrMap_[i];
      const auto &ref = src.attrs[srcAttrIndex];
      auto &dstData = attrs_.attrs[i + 1].data;

      if (ref.type == AttrType::BOOL) {
        BoolAttrView *view = static_cast<BoolAttrView *>(ref.data);
        BoolAttrView *dstView = static_cast<BoolAttrView *>(dstData);

        bool tmp = dstView->get(dst_i);
        dstView->set(dst_i, view->get(src_i));
        view->set(src_i, tmp);
        continue;
      }

      const auto &srcData = ref.data;

      memcpy(static_cast<void *>(buf), dstData->getElemData(dst_i), dstData->elemSize);
      memcpy(dstData->getElemData(dst_i), srcData->getElemData(src_i), dstData->elemSize);
      memcpy(srcData->getElemData(src_i), static_cast<void *>(buf), dstData->elemSize);
    }
  };

  ATTR_NO_OPT
  void swap(mesh::AttrGroup &src, spatial::SpatialTree *tree)
  {
    for (int i : util::IndexRange(0, size_)) {
      this->swapWith(src, origIndex[i], i);
    }
    isSwapped ^= true;
  }
  virtual void undo(mesh::AttrGroup &src, spatial::SpatialTree *tree)
  {
    swap(src, tree);
  }
  virtual void redo(mesh::AttrGroup &src, spatial::SpatialTree *tree)
  {
    swap(src, tree);
  }

private:
  mesh::AttrGroup attrs_;
  Vector<int> srcAttrMap_; // one-to-one mapping to attributes in attrs_
  int size_;
};
} // namespace detail

/** Per-node undo data. */
struct LogChunkSimple : public LogChunk {
  detail::ChunkElemData v, e, c, f;
  int nodeId;

  LogChunkSimple(int nodeId, int vcount, int ecount, int ccount, int fcount)
      : LogChunk(LogChunkTypes::Simple), nodeId(nodeId), v(vcount), e(ecount), c(ccount),
        f(fcount)
  {
    //
  }

  void undo(mesh::Mesh *m, spatial::SpatialTree *tree)
  {
    using namespace sculptcore::spatial;

    v.undo(m->v.attrs, tree);
    e.undo(m->e.attrs, tree);
    c.undo(m->c.attrs, tree);
    f.undo(m->f.attrs, tree);

    SpatialNode *node = tree->node_from_id(nodeId);
    node->update(NodeFlags::Spatial_UpdateGPU | NodeFlags::Spatial_RegenBounds);
  }

  void redo(mesh::Mesh *m, spatial::SpatialTree *tree)
  {
    using namespace sculptcore::spatial;

    v.redo(m->v.attrs, tree);
    e.redo(m->e.attrs, tree);
    c.redo(m->c.attrs, tree);
    f.redo(m->f.attrs, tree);

    SpatialNode *node = tree->node_from_id(nodeId);
    node->update(NodeFlags::Spatial_UpdateGPU | NodeFlags::Spatial_RegenBounds);
  }

private:
  mesh::AttrGroup attrs_;
  Vector<int> srcAttrMap_; // one-to-one mapping to attributes in attrs_
  int size_;
};

namespace detail {

/**
 * Single-row attribute snapshot for one element in an AttrGroup.
 *
 * Captures every attribute (typed + bool, including TOPO-flagged
 * attrs) at a given index into a flat byte buffer. The byte layout is
 * computed from the source group on capture and assumed identical on
 * subsequent writeTo / swapWith calls (i.e. the AttrGroup must not
 * have had attrs added or reordered in between).
 */
struct ChunkElemRow {
  ChunkElemRow() = default;

  void captureFrom(mesh::AttrGroup &src, int src_idx)
  {
    layoutFor(src);
    for (int i = 0; i < src.attrs.size(); i++) {
      mesh::AttrRef &ref = src.attrs[i];
      uint8_t *dst = data_.data() + offsets_[i];

      if (ref.type == mesh::AttrType::BOOL) {
        mesh::BoolAttrView *view = static_cast<mesh::BoolAttrView *>(ref.data);
        dst[0] = view->get(src_idx) ? 1 : 0;
      } else {
        memcpy(static_cast<void *>(dst),
               ref.data->getElemData(src_idx),
               ref.data->elemSize);
      }
    }
  }

  void writeTo(mesh::AttrGroup &dst, int dst_idx)
  {
    for (int i = 0; i < dst.attrs.size(); i++) {
      mesh::AttrRef &ref = dst.attrs[i];
      uint8_t *src = data_.data() + offsets_[i];

      if (ref.type == mesh::AttrType::BOOL) {
        mesh::BoolAttrView *view = static_cast<mesh::BoolAttrView *>(ref.data);
        view->set(dst_idx, src[0] != 0);
      } else {
        memcpy(ref.data->getElemData(dst_idx),
               static_cast<const void *>(src),
               ref.data->elemSize);
      }
    }
  }

  void swapWith(mesh::AttrGroup &live, int live_idx)
  {
    uint8_t buf[64];
    for (int i = 0; i < live.attrs.size(); i++) {
      mesh::AttrRef &ref = live.attrs[i];
      uint8_t *slot = data_.data() + offsets_[i];

      if (ref.type == mesh::AttrType::BOOL) {
        mesh::BoolAttrView *view = static_cast<mesh::BoolAttrView *>(ref.data);
        bool tmp = view->get(live_idx);
        view->set(live_idx, slot[0] != 0);
        slot[0] = tmp ? 1 : 0;
      } else {
        void *live_p = ref.data->getElemData(live_idx);
        size_t n = ref.data->elemSize;
        memcpy(static_cast<void *>(buf), live_p, n);
        memcpy(live_p, static_cast<const void *>(slot), n);
        memcpy(static_cast<void *>(slot), static_cast<const void *>(buf), n);
      }
    }
  }

private:
  void layoutFor(mesh::AttrGroup &src)
  {
    offsets_.resize(src.attrs.size());
    int total = 0;
    for (int i = 0; i < src.attrs.size(); i++) {
      offsets_[i] = total;
      mesh::AttrRef &ref = src.attrs[i];
      total += (ref.type == mesh::AttrType::BOOL) ? 1 : int(ref.data->elemSize);
    }
    data_.resize(total);
  }

  Vector<uint8_t> data_;
  Vector<int> offsets_;
};

} // namespace detail

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
  Vector<LogElem *> records;
  util::Pool<LogElem> records_pool;
  util::Pool<detail::ChunkElemRow> bodies_pool;

  /** key: (uint8_t kind << 32) | uint32_t(mesh_index)  →  log_id */
  util::Map<int64_t, int> idx_to_log_id;
  util::Map<int, LogElem *> by_log_id;
  int next_log_id = 0;

  LogChunkTopo() : LogChunk(LogChunkTypes::Topo)
  {
  }

  ~LogChunkTopo() override
  {
    /* Pools own the LogElem + ChunkElemRow storage; their destructors
     * tear everything down. */
  }

  void onCreate(LogElemKind kind, mesh::Mesh *m, int idx)
  {
    int64_t key = makeKey(kind, idx);

    /* Defensive: stale mapping from a malformed prior sequence. */
    int existing_id;
    if (lookupId(key, existing_id)) {
      idx_to_log_id.remove(key);
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

    records.append(e);
    idx_to_log_id.insert(int64_t(key), int(e->log_id));
    by_log_id.insert(int(e->log_id), e);
  }

  void onChange(LogElemKind kind, mesh::Mesh *m, int idx)
  {
    int64_t key = makeKey(kind, idx);

    int existing_id;
    if (lookupId(key, existing_id)) {
      /* Already a record for this element; nothing to do. Created
       * records snapshot at finalizeStep; Existed records already
       * snapshotted on first touch. */
      return;
    }

    /* First touch of a previously-existing element — take begin-snapshot. */
    LogElem *e = records_pool.alloc();
    e->log_id = next_log_id++;
    e->kind = kind;
    e->origin = LogOrigin::Existed;
    e->fate = LogFate::Live;
    e->begin_mesh_index = idx;
    e->end_mesh_index = idx;
    e->begin_body = bodies_pool.alloc();
    e->end_body = nullptr;
    e->begin_body->captureFrom(group(m, kind), idx);

    records.append(e);
    idx_to_log_id.insert(int64_t(key), int(e->log_id));
    by_log_id.insert(int(e->log_id), e);
  }

  void onKill(LogElemKind kind, mesh::Mesh *m, int idx)
  {
    int64_t key = makeKey(kind, idx);

    int existing_id;
    if (lookupId(key, existing_id)) {
      LogElem *e = by_log_id.lookup(existing_id);

      if (e->origin == LogOrigin::Created) {
        /* Create+kill within step: net no-op. Drop the record. */
        dropRecord(e);
        idx_to_log_id.remove(key);
        return;
      }

      /* Existed && now Dead — begin_body already captured. */
      e->fate = LogFate::Dead;
      idx_to_log_id.remove(key);
      return;
    }

    /* Killed without a prior change — snapshot now. */
    LogElem *e = records_pool.alloc();
    e->log_id = next_log_id++;
    e->kind = kind;
    e->origin = LogOrigin::Existed;
    e->fate = LogFate::Dead;
    e->begin_mesh_index = idx;
    e->end_mesh_index = idx;
    e->begin_body = bodies_pool.alloc();
    e->end_body = nullptr;
    e->begin_body->captureFrom(group(m, kind), idx);

    records.append(e);
    by_log_id.insert(int(e->log_id), e);
    /* Do NOT map idx_to_log_id — element is dead. */
  }

  /** Capture end-state for Created && Live records. Called from MeshLog::endStep. */
  void finalizeStep(mesh::Mesh *m)
  {
    for (LogElem *e : records) {
      if (e->origin == LogOrigin::Created && e->fate == LogFate::Live) {
        if (!e->end_body) {
          e->end_body = bodies_pool.alloc();
        }
        e->end_body->captureFrom(group(m, e->kind), e->end_mesh_index);
      }
    }
  }

  void undo(mesh::Mesh *m, spatial::SpatialTree *tree) override
  {
    /* Reverse order: undo dependents before underlying elements. */
    for (int i = records.size() - 1; i >= 0; i--) {
      LogElem *e = records[i];
      mesh::ElemData *ed = elemData(m, e->kind);
      mesh::AttrGroup &grp = ed->attrs;

      if (e->origin == LogOrigin::Created && e->fate == LogFate::Live) {
        /* It exists post-step; release it. */
        ed->release(e->end_mesh_index);
      } else if (e->origin == LogOrigin::Existed && e->fate == LogFate::Live) {
        /* Swap with live to revert to pre-step state. */
        e->begin_body->swapWith(grp, e->begin_mesh_index);
      } else if (e->origin == LogOrigin::Existed && e->fate == LogFate::Dead) {
        /* It was killed; bring it back at its original index. */
        ed->alloc(e->begin_mesh_index);
        e->begin_body->writeTo(grp, e->begin_mesh_index);
      }
      /* (Created && Dead) records were dropped at kill time. */
    }
  }

  void redo(mesh::Mesh *m, spatial::SpatialTree *tree) override
  {
    /* Forward order: allocate underlying before dependents reference them. */
    for (int i = 0; i < records.size(); i++) {
      LogElem *e = records[i];
      mesh::ElemData *ed = elemData(m, e->kind);
      mesh::AttrGroup &grp = ed->attrs;

      if (e->origin == LogOrigin::Created && e->fate == LogFate::Live) {
        /* Recreate at the recorded mesh index. */
        ed->alloc(e->end_mesh_index);
        e->end_body->writeTo(grp, e->end_mesh_index);
      } else if (e->origin == LogOrigin::Existed && e->fate == LogFate::Live) {
        /* Swap toggle — the body now holds the pre-step state, the live
         * mesh gets the post-step state back. */
        e->begin_body->swapWith(grp, e->begin_mesh_index);
      } else if (e->origin == LogOrigin::Existed && e->fate == LogFate::Dead) {
        /* It was killed during the step. */
        ed->release(e->begin_mesh_index);
      }
    }
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
    int *p = idx_to_log_id.lookup_ptr(key);
    if (!p) {
      return false;
    }
    out_id = *p;
    return true;
  }

  void dropRecord(LogElem *e)
  {
    for (int i = 0; i < int(records.size()); i++) {
      if (records[i] == e) {
        records.remove_at(i);
        break;
      }
    }
    by_log_id.remove(e->log_id);
    if (e->begin_body) {
      bodies_pool.release(e->begin_body);
    }
    if (e->end_body) {
      bodies_pool.release(e->end_body);
    }
    records_pool.release(e);
  }
};

struct MeshLog {
  /** Each field in LogEntry is processed in reverse
   * order (for undo) and order (for redo).  Undo
   * swaps with current data.
   */
  struct LogEntry {
    Vector<LogChunk *> chunks;

    LogEntry() = default;
    LogEntry(const LogEntry &b) = default;
    LogEntry(LogEntry &&b) = default;
    LogEntry &operator=(LogEntry &&b) = default;
    LogEntry &operator=(const LogEntry &b) = default;

    ~LogEntry()
    {
      for (LogChunk *chunk : chunks) {
        litestl::alloc::Delete(chunk);
      }
    }
  };

  static litestl::binding::types::Struct<MeshLog> *defineBindings()
  {
    using namespace litestl::binding;
    using binding::types::Struct;
    Struct<MeshLog> *st =
        new Struct<MeshLog>("sculptcore::meshlog::MeshLog", sizeof(MeshLog));

    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_METHOD(st, undo, MARGS("m", "tree"));
    BIND_STRUCT_METHOD(st, redo, MARGS("m", "tree"));
    BIND_STRUCT_METHOD(st, beginStep, MARGS());
    BIND_STRUCT_METHOD(st, endStep, MARGS());

    return st;
  }
  Vector<LogEntry> entries;

  MeshLog()
  {
    entries.grow_one();
    curStep_ = 0;
    installCallbacks();
  }
  ~MeshLog() = default;

  /** Pass to mesh topology ops so they fire into the current topo chunk. */
  mesh::MeshCallbacks *callbacks()
  {
    return &cb_;
  }

  /** Current Mesh* — must be set by the caller before issuing logged ops
   *  so the topo chunk can snapshot pre-kill attributes by index. */
  void setActiveMesh(mesh::Mesh *m)
  {
    active_mesh_ = m;
  }

  void beginStep()
  {
    if (curStep_ != entries.size()) {
      /** thoeretically this should call all the right destructors */
      entries.resize(curStep_);
    }
    entries.grow_one();
    topo_chunk_ = nullptr;
  }

  void endStep()
  {
    if (topo_chunk_ && active_mesh_) {
      topo_chunk_->finalizeStep(active_mesh_);
    }
    topo_chunk_ = nullptr;
    curStep_++;
  }

  /** Lazily allocates a topo chunk in the current entry. */
  LogChunkTopo *getTopoChunk()
  {
    if (topo_chunk_) {
      return topo_chunk_;
    }
    if (curStep_ < 0 || curStep_ >= entries.size()) {
      fprintf(stderr, "Error: getTopoChunk called with no current undo entry\n");
      abort();
    }
    topo_chunk_ = litestl::alloc::New<LogChunkTopo>("LogChunkTopo");
    curEntry().chunks.append(topo_chunk_);
    return topo_chunk_;
  }

  LogChunkSimple *hasSimpleChunk(int nodeId)
  {
    if (curStep_ < 0 || curStep_ >= entries.size()) {
      fprintf(stderr, "error: call to hasSimpleChunk with no entry\n");
      abort();
    }

    for (LogChunk *chunk : curEntry().chunks) {
      if (chunk->type != LogChunkTypes::Simple) {
        continue;
      }
      LogChunkSimple *simple = static_cast<LogChunkSimple *>(chunk);
      if (simple->nodeId == nodeId) {
        return simple;
      }
    }

    return nullptr;
  }

  LogChunkSimple *
  getSimpleChunk(int nodeId, int vcount, int ecount, int ccount, int fcount)
  {
    if (curStep_ < 0 || curStep_ >= entries.size()) {
      fprintf(stderr, "Error: getSimpleChunk called with no current undo entry\n");
      abort();
    }

    LogChunkSimple *simple = hasSimpleChunk(nodeId);
    if (!simple) {
      simple = litestl::alloc::New<LogChunkSimple>(
          "LogChunkSimple", nodeId, vcount, ecount, ccount, fcount);
      curEntry().chunks.append(simple);
    }
    return simple;
  }

  LogEntry &curEntry()
  {
    return entries[curStep_];
  }

  ATTR_NO_OPT
  void undo(mesh::Mesh *m, spatial::SpatialTree *tree)
  {
    if (curStep_ <= 0) {
      return;
    }

    curStep_--;

    if (curStep_ < 0 || curStep_ >= entries.size()) {
      return;
    }
    for (LogChunk *chunk : curEntry().chunks) {
      chunk->undo(m, tree);
    }
  }

  void redo(mesh::Mesh *m, spatial::SpatialTree *tree)
  {
    if (curStep_ < 0 || curStep_ >= entries.size()) {
      return;
    }
    for (LogChunk *chunk : curEntry().chunks) {
      chunk->redo(m, tree);
    }
    curStep_++;
  }

private:
  void installCallbacks()
  {
    auto fwd = [this](LogElemKind kind) {
      return [this, kind](int idx) {
        if (!active_mesh_) {
          return;
        }
        getTopoChunk()->onChange(kind, active_mesh_, idx);
      };
    };
    auto fwdCreate = [this](LogElemKind kind) {
      return [this, kind](int idx) {
        if (!active_mesh_) {
          return;
        }
        getTopoChunk()->onCreate(kind, active_mesh_, idx);
      };
    };
    auto fwdKill = [this](LogElemKind kind) {
      return [this, kind](int idx) {
        if (!active_mesh_) {
          return;
        }
        getTopoChunk()->onKill(kind, active_mesh_, idx);
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

  int curStep_;
  mesh::MeshCallbacks cb_;
  mesh::Mesh *active_mesh_ = nullptr;
  LogChunkTopo *topo_chunk_ = nullptr;
};

} // namespace sculptcore::meshlog
