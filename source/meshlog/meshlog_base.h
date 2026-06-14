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
#include "mesh/mesh_enums.h"
#include "spatial/node.h"
#include "spatial/spatial.h"

#include <cstdint>
#include <utility>

namespace sculptcore::meshlog {
using litestl::math::float3;
using litestl::util::string;
using litestl::util::Vector;

enum _LogChunkTypes {
  Simple = 0,
  Topo = 1,
  Reorder = 2,
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
  /** Estimated heap bytes retained by this chunk (undo memory accounting). */
  virtual double memSize()
  {
    return double(sizeof(LogChunk));
  }
};

namespace detail {
struct ChunkElemData {
  mesh::BuiltinAttr<int, ".sculpt.undo.origIndex"> origIndex;
  bool isSwapped = false;

  ChunkElemData(int size, mesh::ElemType domain) : size_(size), domain_(domain)
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
  template <typename ITER> void cpyFrom(const mesh::AttrGroup &src, ITER &elements)
  {
    int index = 0;
    for (int i : elements) {
      cpyFrom(src, i, index);
      index++;
    }
  }

  void cpyFrom(const mesh::AttrGroup &src, int src_i, int dst_i)
  {
    using namespace sculptcore::mesh;

    origIndex[dst_i] = src_i;

    for (int i = 0; i < srcAttrMap_.size(); i++) {
      int srcAttrIndex = srcAttrMap_[i];
      if (srcAttrIndex == -1) {
        // attribute disappeared
        continue;
      }

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

  void swapWith(const mesh::AttrGroup &src, int src_i, int dst_i)
  {
    using namespace sculptcore::mesh;
    char buf[64];

    for (int i = 0; i < srcAttrMap_.size(); i++) {
      int srcAttrIndex = srcAttrMap_[i];
      if (srcAttrIndex == -1) {
        // attribute disappeared
        continue;
      }

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

  void swap(mesh::AttrGroup &src, spatial::SpatialTree *tree)
  {
    updateSrcAttrMap(tree->m);

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

  double memSize()
  {
    double tot = double(sizeof(*this)) + double(srcAttrMap_.size()) * sizeof(int);
    tot += double(attrs_.bool_attrs.blocksize()) * double(size_);
    for (auto &ref : attrs_.attrs) {
      if (ref.type == mesh::AttrType::BOOL) {
        continue;
      }
      tot += double(ref.data->elemSize) * double(size_);
    }
    return tot;
  }


  int size() const
  {
    return size_;
  }

  void updateSrcAttrMap(Mesh *m)
  {
    mesh::AttrGroup *meshAttrs = nullptr;

    switch (domain_) {
    case mesh::ElemType::VERTEX:
      meshAttrs = &m->v.attrs;
      break;
    case mesh::ElemType::EDGE:
      meshAttrs = &m->e.attrs;
      break;
    case mesh::ElemType::CORNER:
      meshAttrs = &m->c.attrs;
      break;
    case mesh::ElemType::LIST:
      meshAttrs = &m->l.attrs;
      break;
    case mesh::ElemType::FACE:
      meshAttrs = &m->f.attrs;
      break;
    }

    if (meshAttrs == nullptr) {
      fprintf(stderr, "Error: updateSrcAttrMap called with invalid domain\n");
      return;
    }

    auto oldSrcMap = srcAttrMap_;
    srcAttrMap_.clear();

    // skip origIndex which doesn't map to any real attribute in the mesh
    for (int i = 1; i < attrs_.attrs.size(); i++) {
      auto &ref = attrs_.attrs[i];
      bool ok = false;

      for (int j = 0; j < meshAttrs->attrs.size(); j++) {
        if (ref.name == meshAttrs->attrs[j].name) {
          srcAttrMap_.append(j);
          ok = true;
          break;
        }
      }

      if (!ok) {
        printf("Warning: attribute %p %s not found in mesh\n", ref.name.c_str(), ref.name.c_str());
        srcAttrMap_.append(-1);
      }
      if (srcAttrMap_[i] != oldSrcMap[i]) {
        printf("Info: attribute %s changed index in mesh\n", ref.name.c_str());
      }
    }
  }

private:
  mesh::AttrGroup attrs_;
  mesh::ElemType domain_;
  Vector<int> srcAttrMap_; // one-to-one mapping to attributes in attrs_
  int size_;
};
} // namespace detail

/** Per-node undo data. */
struct LogChunkSimple : public LogChunk {
  detail::ChunkElemData v, e, c, f;
  /** note: nodeId cannot be read after initial creation of chunks,
      since by the time this chunk is undo/redo-afied it's not guaranteed
      to be correct */
  int nodeId;

  LogChunkSimple(int nodeId, int vcount, int ecount, int ccount, int fcount)
      : LogChunk(LogChunkTypes::Simple), nodeId(nodeId),
        v(vcount, mesh::ElemType::VERTEX), e(ecount, mesh::ElemType::EDGE),
        c(ccount, mesh::ElemType::CORNER), f(fcount, mesh::ElemType::FACE)
  {
    //
  }

  void undo(mesh::Mesh *m, spatial::SpatialTree *tree) override
  {
    using namespace sculptcore::spatial;

    v.undo(m->v.attrs, tree);
    e.undo(m->e.attrs, tree);
    c.undo(m->c.attrs, tree);
    f.undo(m->f.attrs, tree);

    // cannot use nodeId here it's not guaranteed to be correct
    update_node(m, tree);
  }

  void redo(mesh::Mesh *m, spatial::SpatialTree *tree) override
  {
    v.redo(m->v.attrs, tree);
    e.redo(m->e.attrs, tree);
    c.redo(m->c.attrs, tree);
    f.redo(m->f.attrs, tree);

    // cannot use nodeId here it's not guaranteed to be correct
    update_node(m, tree);
  }

  double memSize() override
  {
    return double(sizeof(*this)) + v.memSize() + e.memSize() + c.memSize() + f.memSize();
  }

private:
  void update_node(mesh::Mesh *m, spatial::SpatialTree *tree)
  {
    using namespace sculptcore::spatial;

    // cannot use nodeId here it's not guaranteed to be correct
    for (int i : util::IndexRange(0, f.size())) {
      int fi = f.origIndex[i];
      int ni = tree->treeMesh.f.node[fi];
      if (ni) {
        SpatialNode *node = tree->node_from_id(ni);
        node->update(NodeFlags::Spatial_UpdateGPU | NodeFlags::Spatial_RegenBounds);
      }
    }
  }

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
 * computed from the source group on capture; writeTo / swapWith only
 * ever touch the first count_ attrs (the prefix that existed at capture
 * time), so an AttrGroup that has attrs *appended* between capture and
 * replay stays safe — the new trailing columns are simply not restored.
 * Reordering is still unsupported.
 */
struct ChunkElemRow {
  ChunkElemRow() = default;

  void captureFrom(mesh::AttrGroup &src, int src_idx)
  {
    layoutFor(src);
    for (int i = 0; i < src.attrs.size(); i++) {
      mesh::AttrRef &ref = src.attrs[i];
      uint8_t *dst = data_.data() + offsets_[i];

      /* TEMP attrs (e.g. .spatial.*.node) are derived state owned by the
       * spatial tree, not authoritative undo data — skip them so incremental
       * tree updates during a logged step don't taint replay. */
      if (ref.flag & mesh::AttrFlag::NOCOPY) {
        continue;
      }

      if (ref.type == mesh::AttrType::BOOL) {
        mesh::BoolAttrView *view = static_cast<mesh::BoolAttrView *>(ref.data);
        dst[0] = view->get(src_idx) ? 1 : 0;
      } else {
        const void *src = ref.data->getElemData(src_idx);
        if (!src) { /* unmaterialized page (frozen-topo column?) — see warnNullPage */
          warnNullPage("captureFrom", ref);
          continue;
        }
        memcpy(static_cast<void *>(dst), src, ref.data->elemSize);
      }
    }
  }

  void writeTo(mesh::AttrGroup &dst, int dst_idx)
  {
    int n = dst.attrs.size() < count_ ? int(dst.attrs.size()) : count_;
    diagCheck("writeTo", dst, n);
    for (int i = 0; i < n; i++) {
      mesh::AttrRef &ref = dst.attrs[i];
      if (ref.flag & mesh::AttrFlag::NOCOPY) {
        // note: since this is called on element re-creation,
        // we want to restore nocopy attrs to their default states
        // (which should should have been saved in layoutFor)
      }
      uint8_t *src = data_.data() + offsets_[i];

      if (ref.type == mesh::AttrType::BOOL) {
        mesh::BoolAttrView *view = static_cast<mesh::BoolAttrView *>(ref.data);
        view->set(dst_idx, src[0] != 0);
      } else {
        void *dst = ref.data->getElemData(dst_idx);
        if (!dst) {
          warnNullPage("writeTo", ref);
          continue;
        }
        memcpy(dst, static_cast<const void *>(src), ref.data->elemSize);
      }
    }
  }

  double memSize()
  {
    return double(sizeof(*this)) + double(data_.size()) +
           double(offsets_.size()) * sizeof(int);
  }

  void swapWith(mesh::AttrGroup &live, int live_idx)
  {
    uint8_t buf[64];
    int n = live.attrs.size() < count_ ? int(live.attrs.size()) : count_;
    diagCheck("swapWith", live, n);
    for (int i = 0; i < n; i++) {
      mesh::AttrRef &ref = live.attrs[i];
      if (ref.flag & mesh::AttrFlag::NOCOPY) {
        continue;
      }
      uint8_t *slot = data_.data() + offsets_[i];

      if (ref.type == mesh::AttrType::BOOL) {
        mesh::BoolAttrView *view = static_cast<mesh::BoolAttrView *>(ref.data);
        bool tmp = view->get(live_idx);
        view->set(live_idx, slot[0] != 0);
        slot[0] = tmp ? 1 : 0;
      } else {
        void *live_p = ref.data->getElemData(live_idx);
        if (!live_p) {
          warnNullPage("swapWith", ref);
          continue;
        }
        size_t n = ref.data->elemSize;
        memcpy(static_cast<void *>(buf), live_p, n);
        memcpy(live_p, static_cast<const void *>(slot), n);
        memcpy(static_cast<void *>(slot), static_cast<const void *>(buf), n);
      }
    }
  }

private:
  /* A null getElemData means an unmaterialized page — e.g. a frozen-topo TOPO
   * column. MeshLog::undo/redo thaw before replaying topo chunks, so hitting
   * this is a bug; warn loudly instead of memcpy'ing through null. */
  static void warnNullPage(const char *where, const mesh::AttrRef &ref)
  {
    fprintf(stderr,
            "meshlog: ChunkElemRow::%s: attr '%s' has an unmaterialized page; "
            "skipping (mesh frozen during undo?)\n",
            where,
            ref.name.c_str());
  }

  void layoutFor(mesh::AttrGroup &src)
  {
    count_ = int(src.attrs.size());
    offsets_.resize(src.attrs.size());
    /* CLAUDENOTE: diag — record captured per-attr identity to detect layout drift */
    capType_.resize(src.attrs.size());
    capSize_.resize(src.attrs.size());
    capName_.resize(src.attrs.size());
    int total = 0;
    for (int i = 0; i < src.attrs.size(); i++) {
      offsets_[i] = total;
      mesh::AttrRef &ref = src.attrs[i];
      capType_[i] = int(ref.type);
      capSize_[i] = (ref.type == mesh::AttrType::BOOL) ? 1 : int(ref.data->elemSize);
      capName_[i] = ref.name;
      total += (ref.type == mesh::AttrType::BOOL) ? 1 : int(ref.data->elemSize);
    }
    // TODO: handle non-zero attribute defaults here
    //       for now just zero initialize
    data_.resize(total);
  }

  /* CLAUDENOTE: diag — verify replay-time attr i still matches capture-time attr i */
  void diagCheck(const char *where, mesh::AttrGroup &dst, int n)
  {
    static int reported = 0;
    for (int i = 0; i < n && i < int(capType_.size()); i++) {
      mesh::AttrRef &ref = dst.attrs[i];
      int liveSize = (ref.type == mesh::AttrType::BOOL) ? 1 : int(ref.data->elemSize);
      if (int(ref.type) != capType_[i] || liveSize != capSize_[i] ||
          ref.name != capName_[i])
      {
        if (reported++ < 40) {
          fprintf(stderr,
                  "MESHLOG-DIAG %s: attr[%d] DRIFT cap=(%s t%d sz%d) live=(%s t%d sz%d) "
                  "count_=%d dst.size=%d\n",
                  where,
                  i,
                  capName_[i].c_str(),
                  capType_[i],
                  capSize_[i],
                  ref.name.c_str(),
                  int(ref.type),
                  liveSize,
                  count_,
                  int(dst.attrs.size()));
        }
      }
    }
  }

  Vector<uint8_t> data_;
  Vector<int> offsets_;
  /* CLAUDENOTE: diag arrays */
  Vector<int> capType_;
  Vector<int> capSize_;
  Vector<string> capName_;
  /* Number of attrs present at capture time. Restore loops bound to this so a
   * mid-step attr append (e.g. boundary EDGE_DIRTY) can't drive offsets_[i] OOB. */
  int count_ = 0;
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
  util::Pool<LogElem, 512> records_pool;
  util::Pool<detail::ChunkElemRow, 512> bodies_pool;

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

    idx_to_log_id.insert(int64_t(key), int(e->log_id));
    by_log_id.insert(int(e->log_id), e);
  }

  void onChange(LogElemKind kind, mesh::Mesh *m, int idx)
  {
    int64_t key = makeKey(kind, idx);

    int existing_id;
    if (lookupId(key, existing_id)) {
      /* Already a record for this element; nothing to do. Created
       * z snapshot at finalizeStep; Existed records already
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

    by_log_id.insert(int(e->log_id), e);
    /* Do NOT map idx_to_log_id — element is dead. */
  }

  /** Capture end-state for Created && Live records. Called from MeshLog::endStep. */
  void finalizeStep(mesh::Mesh *m)
  {
    for (LogElem &e : records_pool) {
      if (e.origin == LogOrigin::Created && e.fate == LogFate::Live) {
        if (!e.end_body) {
          e.end_body = bodies_pool.alloc();
        }
        e.end_body->captureFrom(group(m, e.kind), e.end_mesh_index);
      }
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

  void undo(mesh::Mesh *m, spatial::SpatialTree *tree) override
  {
    Vector<LogElem *> records = getSortedRecords();

    /* The raw alloc/release below bypass make_face/kill_face, so the spatial
     * tree's incremental face ownership (`.spatial.f.node`, a TEMP attr that
     * ChunkElemRow does NOT log) is never updated by the restore itself. Drive
     * the tree's add_face/remove_face here so its leaf face-sets + GPU buffers
     * track the restored mesh; without it undo leaves a stale tree (nothing
     * redrawn). No-op when undoing with no tree (the isolated operator tests). */
    if (tree) {
      /* Pre-pass (mesh still in post-step state, so connectivity is valid):
       * drop ownership of faces about to be released or rewired, and of verts
       * about to be released (else their leaf keeps a dangling unique_verts ref
       * — the forward kill path never owned-removed them via callbacks). */

      for (LogElem *e : records) {
        if (e->origin == LogOrigin::Created && e->fate == LogFate::Live) {
          if (e->kind == LogElemKind::Face)
            tree->remove_face(e->end_mesh_index);
          else if (e->kind == LogElemKind::Vert)
            tree->remove_vert(e->end_mesh_index);
        } else if (e->kind == LogElemKind::Face && e->origin == LogOrigin::Existed &&
                   e->fate == LogFate::Live)
        {
          tree->remove_face(e->begin_mesh_index);
        }
      }
    }

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

    if (tree) {
      /* Post-pass (mesh now fully in pre-step state): re-own faces that came
       * back or were rewired. add_face re-derives the leaf and flags it for
       * tris/bounds/GPU regen. */
      for (LogElem *e : records) {
        if (e->kind != LogElemKind::Face)
          continue;
        if (e->origin == LogOrigin::Existed && e->fate == LogFate::Dead) {
          tree->add_face(e->begin_mesh_index);
        } else if (e->origin == LogOrigin::Existed && e->fate == LogFate::Live) {
          // double check face is in tree
          if (tree->treeMesh.f.node[e->begin_mesh_index] == 0) {
            tree->add_face(e->begin_mesh_index);
          }
        }
      }
    }
  }

  void redo(mesh::Mesh *m, spatial::SpatialTree *tree) override
  {
    Vector<LogElem *> records = getSortedRecords();

    if (tree) {
      /* Pre-pass (mesh in pre-step state): drop ownership of faces about to be
       * released or rewired, and of verts about to be released — else their
       * leaf keeps a dangling unique_verts ref that an index-reusing recreate
       * would resurrect into a double-owned vert. */
      for (LogElem *e : records) {
        if (e->origin != LogOrigin::Existed)
          continue;
        if (e->kind == LogElemKind::Face && e->fate == LogFate::Dead) {
          tree->remove_face(e->begin_mesh_index);
        } else if (e->kind == LogElemKind::Face && e->fate == LogFate::Live) {
          if (tree->treeMesh.f.node[e->begin_mesh_index] != 0) {
            tree->remove_face(e->begin_mesh_index);
          }
        } else if (e->kind == LogElemKind::Vert && e->fate == LogFate::Dead) {
          tree->remove_vert(e->begin_mesh_index);
        }
      }
    }

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

    if (tree) {
      /* Post-pass (mesh in post-step state): re-own recreated/rewired faces. */
      for (LogElem *e : records) {
        if (e->kind != LogElemKind::Face)
          continue;
        if (e->origin == LogOrigin::Created && e->fate == LogFate::Live) {
          if (tree->treeMesh.f.node[e->end_mesh_index] == 0) {
            tree->add_face(e->end_mesh_index);
          }
        } else if (e->origin == LogOrigin::Existed && e->fate == LogFate::Live) {
          if (tree->treeMesh.f.node[e->begin_mesh_index] == 0) {
            tree->add_face(e->begin_mesh_index);
          }
        }
      }
    }
  }

  double memSize() override
  {
    /* Map/pool bookkeeping is estimated as a flat per-record constant. */
    constexpr double kRecordOverhead = sizeof(LogElem *) + 48.0;
    double tot = double(sizeof(*this));
    for (LogElem &e : records_pool) {
      tot += double(sizeof(LogElem)) + kRecordOverhead;
      if (e.begin_body) {
        tot += e.begin_body->memSize();
      }
      if (e.end_body) {
        tot += e.end_body->memSize();
      }
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
    int *p = idx_to_log_id.lookup_ptr(key);
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

/**
 * Reorder undo/redo chunk — records the five element permutations
 * (map[old] = new) applied by SpatialTree::applyReorder. A reorder is a pure
 * bijection, so undo replays the inverse permutation and redo replays the
 * forward one; applyReorder rebuilds the tree each way. Because buildAll is
 * deterministic in the mesh's geometry+topology, an inverse reorder reproduces
 * the exact node set (and node ids) that existed before the reorder, so simple
 * chunks recorded in earlier steps still resolve their node ids after undoing
 * back across this chunk.
 */
struct LogChunkReorder : public LogChunk {
  Vector<int> vmap, emap, cmap, lmap, fmap;

  LogChunkReorder(Vector<int> vmap_,
                  Vector<int> emap_,
                  Vector<int> cmap_,
                  Vector<int> lmap_,
                  Vector<int> fmap_)
      : LogChunk(LogChunkTypes::Reorder), vmap(std::move(vmap_)), emap(std::move(emap_)),
        cmap(std::move(cmap_)), lmap(std::move(lmap_)), fmap(std::move(fmap_))
  {
  }

  void undo(mesh::Mesh *m, spatial::SpatialTree *tree) override
  {
    Vector<int> iv, ie, ic, il, iff;
    invert(vmap, iv);
    invert(emap, ie);
    invert(cmap, ic);
    invert(lmap, il);
    invert(fmap, iff);
    tree->applyReorder(iv, ie, ic, il, iff);
  }

  void redo(mesh::Mesh *m, spatial::SpatialTree *tree) override
  {
    tree->applyReorder(vmap, emap, cmap, lmap, fmap);
  }

  double memSize() override
  {
    double n =
        double(vmap.size() + emap.size() + cmap.size() + lmap.size() + fmap.size());
    return double(sizeof(*this)) + n * sizeof(int);
  }

private:
  static void invert(const Vector<int> &map, Vector<int> &out)
  {
    out.resize(map.size());
    for (int i = 0; i < int(map.size()); i++) {
      out[map[i]] = i;
    }
  }
};

struct MeshLog {
  /** Each field in LogEntry is processed in reverse
   * order (for undo) and order (for redo).  Undo
   * swaps with current data.
   */
  struct LogEntry {
    LogChunkTopo *topo_chunk_ = nullptr;
    bool hasTopoChunk = false;

    Vector<LogChunk *> chunks;
    /** Monotonic step id assigned by beginStep; stable across history trims. */
    int id = -1;

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

    double memSize()
    {
      double tot = double(sizeof(*this));
      for (LogChunk *chunk : chunks) {
        tot += chunk->memSize();
      }
      return tot;
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
    BIND_STRUCT_METHOD(st, beginStep, MARGS("hasDyntopo"));
    BIND_STRUCT_METHOD(st, endStep, MARGS());
    BIND_STRUCT_METHOD(st, lastStepId, MARGS());
    BIND_STRUCT_METHOD(st, stepMemSize, MARGS("id"));
    BIND_STRUCT_METHOD(st, totalMemSize, MARGS());
    BIND_STRUCT_METHOD(st, entryCount, MARGS());
    BIND_STRUCT_METHOD(st, freeStep, MARGS("id"));
    BIND_STRUCT_METHOD(st, pushTopoChunk, MARGS());
    BIND_STRUCT_METHOD(st, hasTopoChunk, MARGS());

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

  void beginStep(bool hasDyntopo)
  {
    if (curStep_ != entries.size()) {
      /** thoeretically this should call all the right destructors */
      entries.resize(curStep_);
    }
    entries.grow_one();
    entries.last().id = nextStepId_++;
    if (hasDyntopo) {
      pushTopoChunk();
    }
  }

  /** Id of the most recently begun step (-1 if none). Call right after
   * beginStep to key this step for stepMemSize/freeStep. */
  int lastStepId()
  {
    return entries.size() > 0 ? entries.last().id : -1;
  }

  /** Estimated heap bytes retained by the step with @p id (0 if freed). */
  double stepMemSize(int id)
  {
    for (auto &entry : entries) {
      if (entry.id == id) {
        return entry.memSize();
      }
    }
    return 0.0;
  }

  double totalMemSize()
  {
    double tot = 0.0;
    for (auto &entry : entries) {
      tot += entry.memSize();
    }
    return tot;
  }

  int entryCount()
  {
    return int(entries.size());
  }

  /** Free the committed step with @p id (undo-memory eviction from the app's
   * tool stack). Only steps strictly behind the cursor are freeable — the
   * current/redo entries stay. Returns 1 if a step was freed. */
  int freeStep(int id)
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
      /* Move the dropped entry out so its dtor frees the chunks, then shift
       * the tail left (move-assign; raw chunk pointers transfer ownership). */
      LogEntry dropped = std::move(entries[idx]);
      for (int i = idx; i < int(entries.size()) - 1; i++) {
        entries[i] = std::move(entries[i + 1]);
      }
      entries.pop_back();
    }
    curStep_--;
    return 1;
  }

  void endStep()
  {
    /* Only the still-active (last) topo chunk needs finalizing here; earlier
     * chunks were finalized at deactivation by pushTopoChunk. */
    if (curEntry().topo_chunk_ && active_mesh_) {
      curEntry().topo_chunk_->finalizeStep(active_mesh_);
    }
    curEntry().topo_chunk_ = nullptr;
    curStep_++;
    trimHistory();
  }

  /** Cap the retained undo history to @p n committed steps (-1 = unbounded).
   * Trims immediately so lowering the cap at runtime frees old steps now. */
  void setMaxUndoSteps(int n)
  {
    maxUndoSteps_ = n;
    trimHistory();
  }
  int maxUndoSteps() const
  {
    return maxUndoSteps_;
  }

  bool hasTopoChunk() const
  {
    return curEntry().hasTopoChunk;
  }

  void pushTopoChunk()
  {
    if (curStep_ < 0 || curStep_ >= entries.size()) {
      fprintf(stderr, "Error: getTopoChunk called with no current undo entry\n");
      abort();
    }
    /* Finalize the outgoing chunk NOW (end of its dab), not at endStep: each
     * chunk must capture its Created records' end-state while it is still the
     * active chunk. Capturing at end-of-step instead would snapshot connectivity
     * a LATER dab rewired (referencing verts a later chunk creates), so per-chunk
     * redo would re-own a face before its verts exist. */
    if (curEntry().topo_chunk_ && active_mesh_) {
      curEntry().topo_chunk_->finalizeStep(active_mesh_);
    }
    curEntry().topo_chunk_ = litestl::alloc::New<LogChunkTopo>("LogChunkTopo");
    curEntry().hasTopoChunk = true;
    curEntry().chunks.append(curEntry().topo_chunk_);
  }

  /** Lazily allocates a topo chunk in the current entry. */
  LogChunkTopo *getTopoChunk()
  {
    if (curEntry().topo_chunk_) {
      return curEntry().topo_chunk_;
    }
    pushTopoChunk();
    return curEntry().topo_chunk_;
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

  /** Append a reorder chunk capturing the five permutations to the current
   * step. Caller applies the reorder itself (via SpatialTree::applyReorder);
   * the chunk only stores the maps for later undo/redo. */
  LogChunkReorder *pushReorderChunk(Vector<int> vmap,
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

  LogEntry &curEntry()
  {
    return entries[curStep_];
  }

  const LogEntry &curEntry() const
  {
    return entries[curStep_];
  }

  void undo(mesh::Mesh *m, spatial::SpatialTree *tree)
  {
    if (curStep_ <= 0) {
      return;
    }

    curStep_--;

    if (curStep_ < 0 || curStep_ >= entries.size()) {
      return;
    }
    thawForTopoChunks(m);
    /* Undo chunks in REVERSE creation order. A folded sculpt step (TS sculpt op)
     * holds the dyntopo topo chunk (created first) followed by the brush's
     * per-node position LogChunkSimple chunks (created during the deform that ran
     * after dyntopo). They overlap on the verts dyntopo moved: the topo chunk
     * holds the true pre-step position, the simple chunk a mid-stroke
     * (post-dyntopo) one. Replaying newest-first lets the topo chunk's pre-step
     * value win (and keeps the simple swap operating on the still-post-step
     * topology, where its captured indices are all live). */
    auto &chunks = curEntry().chunks;
    for (int i = int(chunks.size()) - 1; i >= 0; i--) {
      chunks[i]->undo(m, tree);
    }
  }

  void redo(mesh::Mesh *m, spatial::SpatialTree *tree)
  {
    if (curStep_ < 0 || curStep_ >= entries.size()) {
      return;
    }
    thawForTopoChunks(m);
    /* Forward creation order, one chunk fully applied before the next: each topo
     * chunk reproduces its own dab's end-state (captured at deactivation, not
     * end-of-step), so a face rewired across dabs is re-owned only after its
     * dab's chunk recreates the verts it now references. */
    for (LogChunk *chunk : curEntry().chunks) {
      chunk->redo(m, tree);
    }
    curStep_++;
  }

private:
  /* Topo chunks restore elements with raw alloc/release + attr memcpys,
   * bypassing the auto-thawing topology mutators. On a frozen mesh the live
   * TOPO link pages are freed (getElemData == null), so thaw first. */
  void thawForTopoChunks(mesh::Mesh *m)
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

  /** Drop oldest committed steps until at most maxUndoSteps_ remain. The popped
   * LogEntry is destroyed by value, so ~LogEntry frees its chunks. Stops at
   * curStep_ == 0 so it never discards the current step or pending redo. */
  void trimHistory()
  {
    if (maxUndoSteps_ < 0) {
      return;
    }
    while (int(entries.size()) > maxUndoSteps_ && curStep_ > 0) {
      entries.pop_front();
      curStep_--;
    }
  }

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
  int maxUndoSteps_ = -1; // -1 = unbounded
  int nextStepId_ = 0;
};

} // namespace sculptcore::meshlog
