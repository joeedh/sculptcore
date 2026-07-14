/**
# Intro

Meshlog is the main undo/redo system for sculptcore. Its two principal
chunk types are:

* `LogChunkElems` — sparse, append-as-touched per-domain attribute-swap
  log for plain vertex-position / paint sculpting. The brush *Pre stage
  appends one row per element the first time it is touched in a step
  (gated by `AttrSaver`, so spatial-tree restructuring can't
  double-capture), and swaps the stored row with live on undo/redo.
  Which attributes a brush captures is declared per-brush via the sbrush
  `save` statement (defaults to vertex co/no + face no).

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

//#define MESHLOG_ABSEIL_HASHMAP

#pragma once

#include "attr_saver.h"
#include "binding/binding_constructor_builder.h"
#include "litestl/math/vector.h"
#include "litestl/util/map.h"
#include "litestl/util/pool.h"
#include "litestl/util/set.h"
#include "litestl/util/span.h"
#include "litestl/util/vector.h"
#include "mesh/attribute.h"
#include "mesh/attribute_bool.h"
#include "mesh/attribute_builtin.h"
#include "mesh/attribute_enums.h"
#include "mesh/mesh.h"
#include "mesh/mesh_callbacks.h"
#include "mesh/mesh_enums.h"
#include "mesh/mesh_path.h"
#include "mesh/ops/bevel.h"
#include "mesh/ops/extrude.h"
#include "mesh/ops/inset.h"
#include "mesh/ops/loopcut.h"
#include "mesh/ops/split.h"
#include "mesh/ops/subdivide.h"
#include "spatial/node.h"
#include "spatial/spatial.h"

#ifdef SCULPTCORE_WITH_ABSEIL
#include <../extern/abseil-cpp/absl/container/flat_hash_map.h>
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace sculptcore::meshlog {
using litestl::math::float3;
using litestl::util::string;
using litestl::util::Vector;

enum _LogChunkTypes {
  Topo = 1,
  Reorder = 2,
  Elems = 3,
  /* A foreign undo channel riding the step (e.g. the VDM tile-delta chunk,
   * source/vdm/vdm_undo.h): opaque to MeshLog beyond the undo/redo virtuals. */
  External = 4,
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

  /* Grow the store by one row, capturing element `src_i` of `src` for every
   * ref in `refs`. Columns are registered on first sight (ensureAttr is
   * idempotent), so the store is sparse / append-as-touched rather than dense
   * node-sized. Returns the new row index. */
  int appendFrom(const mesh::AttrGroup &src,
                 int src_i,
                 litestl::util::span<const mesh::AttrRef> refs)
  {
    for (const mesh::AttrRef &ref : refs) {
      ensureAttr(src, ref);
    }
    int dst_i = size_++;
    attrs_.ensure_capacity(size_);
    cpyFrom(src, src_i, dst_i);
    return dst_i;
  }

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
        printf("Warning: attribute %p %s not found in mesh\n",
               ref.name.c_str(),
               ref.name.c_str());
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

/** Sparse, append-as-touched per-domain element store for brush undo capture.
 * The brush *Pre stage appends one row per element the first time it is touched
 * in a step (gated by AttrSaver, so dyntopo tree restructuring can't
 * double-capture); undo/redo swap by origIndex restores it. One chunk per
 * domain per step. */
struct LogChunkElems : public LogChunk {
  detail::ChunkElemData data;
  mesh::ElemType domain;

  LogChunkElems(mesh::ElemType domain)
      : LogChunk(LogChunkTypes::Elems), data(0, domain), domain(domain)
  {
  }

  void undo(mesh::Mesh *m, spatial::SpatialTree *tree) override
  {
    data.undo(group(m), tree);
    update_nodes(m, tree);
  }

  void redo(mesh::Mesh *m, spatial::SpatialTree *tree) override
  {
    data.redo(group(m), tree);
    update_nodes(m, tree);
  }

  double memSize() override
  {
    return double(sizeof(*this)) + data.memSize();
  }

private:
  mesh::AttrGroup &group(mesh::Mesh *m)
  {
    switch (domain) {
    case mesh::ElemType::VERTEX:
      return m->v.attrs;
    case mesh::ElemType::EDGE:
      return m->e.attrs;
    case mesh::ElemType::CORNER:
      return m->c.attrs;
    case mesh::ElemType::LIST:
      return m->l.attrs;
    default:
      return m->f.attrs; // FACE
    }
  }

  /* Mark the node owning each touched element dirty so its bounds/GPU buffers
   * regenerate. Only vertex/face domains carry a spatial node attribute. */
  void update_nodes(mesh::Mesh *m, spatial::SpatialTree *tree)
  {
    using namespace sculptcore::spatial;
    for (int i : util::IndexRange(0, data.size())) {
      int idx = data.origIndex[i];
      int ni = 0;
      if (domain == mesh::ElemType::VERTEX) {
        ni = tree->treeMesh.v.node[idx];
      } else if (domain == mesh::ElemType::FACE) {
        ni = tree->treeMesh.f.node[idx];
      } else {
        return;
      }
      if (ni) {
        SpatialNode *node = tree->node_from_id(ni);
        // Spatial_UpdateNormals is required: undo/redo swaps co/no rows back via
        // the element store but doesn't drive add_face/remove_*, so without this
        // the node regenerates GPU buffers from stale normals (redo corruption).
        node->update(NodeFlags::Spatial_UpdateGPU | NodeFlags::Spatial_RegenBounds |
                     NodeFlags::Spatial_UpdateNormals);
      }
    }
  }
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

  // CLAUDENOTE: CAP-M0 ablation scaffolding (plan 2026-07-13-2046-meshlog-
  // capture-cost). SC_ABLATE_CAPTURE=1 skips capture bodies entirely,
  // =2 runs layoutFor only (no copies). Correctness-broken (undo must not be
  // replayed); timing-valid for interleaved ablation legs. Ripped in M4.
  static int ablateCapture()
  {
    static const int v = [] {
      const char *s = std::getenv("SC_ABLATE_CAPTURE");
      return s ? std::atoi(s) : 0;
    }();
    return v;
  }

  void captureFrom(mesh::AttrGroup &src, int src_idx)
  {
    // CLAUDENOTE: CAP-M0 ablation scaffolding (plan 2026-07-13-2046)
    if (ablateCapture() == 1) {
      return;
    }
    layoutFor(src);
    // CLAUDENOTE: CAP-M0 ablation scaffolding (plan 2026-07-13-2046)
    if (ablateCapture() == 2) {
      return;
    }
    for (int i = 0; i < src.attrs.size(); i++) {
      mesh::AttrRef &ref = src.attrs[i];
      uint8_t *dst = data_.data() + offsets_[i];

      // TEMP attrs (e.g. .spatial.*.node) are derived state owned by the
      // spatial tree, not authoritative undo data — skip them so incremental
      // tree updates during a logged step don't taint replay.
      if (ref.flag & mesh::AttrFlag::NOCOPY) {
        continue;
      }

      if (ref.type == mesh::AttrType::BOOL) {
        mesh::BoolAttrView *view = static_cast<mesh::BoolAttrView *>(ref.data);
        dst[0] = view->get(src_idx) ? 1 : 0;
      } else {
        const void *src = ref.data->getElemData(src_idx);
        if (!src) { // unmaterialized page (frozen-topo column?) — see warnNullPage
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
    for (int i = 0; i < n; i++) {
      mesh::AttrRef &ref = dst.attrs[i];
      if (ref.flag & mesh::AttrFlag::NOCOPY) {
        // note: since this is called on element re-creation,
        // we want to restore nocopy attrs to their default states
        // (which should have been saved in layoutFor)
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

  /* Re-read only the brush-deformable data columns (skip TOPO connectivity and
   * NOCOPY temp state) into the already-laid-out buffer, leaving the rest of
   * end_body frozen. Used to refresh a Created vert's captured position with its
   * final post-stroke value (see LogChunkTopo::refreshCreatedVertData). */
  void refreshDataColumns(mesh::AttrGroup &src, int src_idx)
  {
    int n = src.attrs.size() < count_ ? int(src.attrs.size()) : count_;
    for (int i = 0; i < n; i++) {
      mesh::AttrRef &ref = src.attrs[i];
      if ((ref.flag & mesh::AttrFlag::TOPO) || (ref.flag & mesh::AttrFlag::NOCOPY)) {
        continue;
      }
      uint8_t *dst = data_.data() + offsets_[i];
      if (ref.type == mesh::AttrType::BOOL) {
        mesh::BoolAttrView *view = static_cast<mesh::BoolAttrView *>(ref.data);
        dst[0] = view->get(src_idx) ? 1 : 0;
      } else {
        const void *s = ref.data->getElemData(src_idx);
        if (!s) {
          continue;
        }
        memcpy(static_cast<void *>(dst), s, ref.data->elemSize);
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
    int total = 0;
    for (int i = 0; i < src.attrs.size(); i++) {
      offsets_[i] = total;
      mesh::AttrRef &ref = src.attrs[i];
      total += (ref.type == mesh::AttrType::BOOL) ? 1 : int(ref.data->elemSize);
    }
    // TODO: handle non-zero attribute defaults here
    //       for now just zero initialize
    data_.resize(total);
  }

  Vector<uint8_t> data_;
  Vector<int> offsets_;
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
  /* Slab sizes are a memory/allocation trade-off: chunks are per-dab, so an
   * 8000-slot slab retained ~1.3MB per barely-used chunk (hundreds per dyntopo
   * stroke) — far past the undo budget without the accounting seeing it. */
  util::Pool<LogElem, 512> records_pool;
  util::Pool<detail::ChunkElemRow, 256> bodies_pool;

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
    // Pools own the LogElem + ChunkElemRow storage; their destructors
    // tear everything down.
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
    e->begin_body->captureFrom(group(m, kind), idx);

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
    // CLAUDENOTE: CAP-M0 ablation scaffolding (plan 2026-07-13-2046):
    // SC_ABLATE_KILL_CAPTURE=1 skips only the kill-time row copies.
    static const bool ablateKill = [] {
      const char *s = std::getenv("SC_ABLATE_KILL_CAPTURE");
      return s && s[0] && s[0] != '0';
    }();
    if (!ablateKill) {
      e->begin_body->captureFrom(group(m, kind), idx);
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
        e.end_body->captureFrom(group(m, e.kind), e.end_mesh_index);
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
         released or rewired, and of verts about to be released — else their
         leaf keeps a dangling unique_verts ref that an index-reusing recreate
         would resurrect into a double-owned vert. */
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

  // returns double (not size_t) because the prototype is inferred from
  // the TS binding side
  double memSize() override
  {
    double tot = double(sizeof(*this));
    // Slab pools retain full-slab capacity, not just live objects — count it,
    // or a stroke's chunks blow past the undo budget invisibly.
    tot += double(records_pool.capacity()) * double(sizeof(LogElem));
    tot += double(bodies_pool.capacity()) * double(sizeof(detail::ChunkElemRow));
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

/**
 * Reorder undo/redo chunk — records the five element permutations
 * (map[old] = new). A reorder is a pure bijection, so undo replays the inverse
 * permutation and redo replays the forward one, both via
 * SpatialTree::applyReorderIncremental: it relabels the existing node set in
 * place (no rebuild), so the node set + ids that existed before the reorder are
 * reproduced EXACTLY across undo/redo — simple chunks recorded in earlier steps
 * still resolve their node ids after undoing back across this chunk. (The
 * forward apply that recorded this chunk must likewise be incremental.)
 */
struct LogChunkReorder : public LogChunk {
  Vector<int> vmap, emap, cmap, lmap, fmap;
  /* Scoped (partial) compaction: the per-domain moved (live) slot sets. A scoped
   * reorder is a closed permutation over these slots, so the set is invariant
   * under the permutation AND its inverse — undo/redo replay scoped with the same
   * sets. Empty ⇒ full reorder (whole-mesh map; replay via the full path). */
  Vector<int> mv, me, mc, ml, mf;
  /* Scoped chunk: the target slots of the moved sets (vval[i] = map[mv[i]]). The
   * full map is NOT stored (it is mostly identity) — reconstructed transiently on
   * undo/redo. This makes the chunk O(moved) instead of O(capacity). */
  Vector<int> vval, eval, cval, lval, fval;
  bool scoped = false;

  LogChunkReorder() : LogChunk(LogChunkTypes::Reorder)
  {
  }

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
    /* Reconstruct the full forward bijection (from sparse moves for a scoped chunk,
     * or pad the stored full map otherwise), then replay the INVERSE via the full
     * applyReorderIncremental. Undo/redo are rare, so the full O(mesh) replay is
     * fine; the scoped forward already paid only O(region). The scoped forward
     * leaves exactly the state a full apply would, so the full inverse reverts it. */
    Vector<int> v, e, c, l, f;
    forwardMaps(m, v, e, c, l, f);

    Vector<int> iv, ie, ic, il, iff;
    invert(v, iv);
    invert(e, ie);
    invert(c, ic);
    invert(l, il);
    invert(f, iff);
    tree->applyReorderIncremental(iv, ie, ic, il, iff);
  }

  void redo(mesh::Mesh *m, spatial::SpatialTree *tree) override
  {
    Vector<int> v, e, c, l, f;
    forwardMaps(m, v, e, c, l, f);
    tree->applyReorderIncremental(v, e, c, l, f);
  }

  double memSize() override
  {
    double n =
        double(vmap.size() + emap.size() + cmap.size() + lmap.size() + fmap.size());
    n += double(mv.size() + me.size() + mc.size() + ml.size() + mf.size());
    n += double(vval.size() + eval.size() + cval.size() + lval.size() + fval.size());
    return double(sizeof(*this)) + n * sizeof(int);
  }

private:
  /* Materialize the full forward bijection per domain at the current capacity —
   * reconstructed from the sparse moves for a scoped chunk, or padded from the
   * stored full map otherwise. */
  void forwardMaps(mesh::Mesh *m,
                   Vector<int> &v,
                   Vector<int> &e,
                   Vector<int> &c,
                   Vector<int> &l,
                   Vector<int> &f)
  {
    if (scoped) {
      reconstruct(mv, vval, int(m->v.capacity()), v);
      reconstruct(me, eval, int(m->e.capacity()), e);
      reconstruct(mc, cval, int(m->c.capacity()), c);
      reconstruct(ml, lval, int(m->l.capacity()), l);
      reconstruct(mf, fval, int(m->f.capacity()), f);
    } else {
      padToCapacity(vmap, int(m->v.capacity()), v);
      padToCapacity(emap, int(m->e.capacity()), e);
      padToCapacity(cmap, int(m->c.capacity()), c);
      padToCapacity(lmap, int(m->l.capacity()), l);
      padToCapacity(fmap, int(m->f.capacity()), f);
    }
  }

  /* Build a full bijection at @p cap from the sparse move list: identity, then
   * out[from[i]] = to[i]. (from,to)=(mv,vval) gives the forward map. Slots created
   * after the reorder are free now and map to themselves, so identity-by-default
   * is the correct extension. */
  static void reconstruct(const Vector<int> &from,
                          const Vector<int> &to,
                          int cap,
                          Vector<int> &out)
  {
    out.resize(cap);
    for (int i = 0; i < cap; i++) {
      out[i] = i;
    }
    for (int i = 0; i < int(from.size()); i++) {
      out[from[i]] = to[i];
    }
  }

  /* The map was recorded at the capacity that existed when the reorder ran. Later
   * steps grow the element arrays and undo doesn't shrink them, so by the time
   * this chunk is replayed the domain capacity can EXCEED the map. The reorder
   * only permuted [0, recorded-capacity); the slots created afterward are free now
   * (their creators are already undone) and map to themselves. Extend with
   * identity so the permutation is a full bijection over the current capacity. */
  static void padToCapacity(const Vector<int> &map, int cap, Vector<int> &out)
  {
    int n = int(map.size());
    out.resize(cap);
    for (int i = 0; i < n && i < cap; i++) {
      out[i] = map[i];
    }
    for (int i = n; i < cap; i++) {
      out[i] = i;
    }
  }

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
    /** Set once the step's topo chunks have captured their end-state (see
     * finalizeStroke). Guards against a second capture from a mutated mesh —
     * stroke-end compaction finalizes early, then endStep must not re-capture. */
    bool finalized = false;

    Vector<LogChunk *> chunks;
    /** Monotonic step id assigned by beginStep; stable across history trims. */
    int id = -1;

    /* Active element per domain (vert/edge/face) captured at beginStep, swapped
     * with the live MeshLog active_* on undo/redo so box-modeling's "active
     * vertex" rides undo (the draft's "active vertex stored in meshlog"). */
    int snapActiveVert = -1;
    int snapActiveEdge = -1;
    int snapActiveFace = -1;

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
    // Step/chunk sequencing (beginStep/endStep/pushTopoChunk/setActiveMesh) is
    // intentionally NOT bound: the brush CommandExecutor owns the dab sequence
    // and meshlog boundaries; JS clients drive undo/redo + memory accounting only.
    BIND_STRUCT_METHOD(st, undo, MARGS("m", "tree"));
    BIND_STRUCT_METHOD(st, redo, MARGS("m", "tree"));
    BIND_STRUCT_METHOD(st, curStrokeId, MARGS());
    BIND_STRUCT_METHOD(st, lastStepId, MARGS());
    BIND_STRUCT_METHOD(st, stepMemSize, MARGS("id"));
    BIND_STRUCT_METHOD(st, totalMemSize, MARGS());
    BIND_STRUCT_METHOD(st, entryCount, MARGS());
    BIND_STRUCT_METHOD(st, freeStep, MARGS("id"));
    BIND_STRUCT_METHOD(st, hasTopoChunk, MARGS());
    BIND_STRUCT_METHOD(st, reorderForLocality, MARGS("tree"));
    BIND_STRUCT_METHOD(st, compactIfFragmented, MARGS("tree", "vertRatioThreshold"));

    // Box-modeling topology macro-ops.
    BIND_STRUCT_MEMBER(st, selectFlushPreferOpDomain);
    BIND_STRUCT_METHOD(st, extrudeRegion, MARGS("m", "outNormal"));
    BIND_STRUCT_METHOD(st, extrudeIndividual, MARGS("m", "outNormal"));
    BIND_STRUCT_METHOD(st, extrudeWireVerts, MARGS("m", "outNormal"));
    BIND_STRUCT_METHOD(st, splitFacesOff, MARGS("m", "outNormal"));
    BIND_STRUCT_METHOD(st, subdivideEdges, MARGS("m", "numCuts", "outVerts"));
    BIND_STRUCT_METHOD(st, loopCut, MARGS("m", "seedEdge", "outVerts"));
    BIND_STRUCT_METHOD(st, loopCutAtRay, MARGS("m", "tree", "origin", "dir", "outVerts"));
    BIND_STRUCT_METHOD(st, insetRegion, MARGS("m", "insetVerts", "baseCo", "tangent"));
    BIND_STRUCT_METHOD(st, bevelVerts, MARGS("m", "verts", "baseCo", "tangent"));

    // Box-modeling selection (undoable).
    BIND_STRUCT_METHOD(st, selectionBeginStep, MARGS());
    BIND_STRUCT_METHOD(st, selectionEndStep, MARGS());
    BIND_STRUCT_METHOD(st, selectOne, MARGS("m", "domain", "idx", "state"));
    BIND_STRUCT_METHOD(st, selectIndices, MARGS("m", "domain", "indices", "state"));
    BIND_STRUCT_METHOD(st, selectAllElems, MARGS("m", "domain", "state"));
    BIND_STRUCT_METHOD(st, selectShortestPath, MARGS("m", "vEnd", "state"));
    BIND_STRUCT_METHOD(st, selectLoop, MARGS("m", "seedEdge", "kind", "state"));
    BIND_STRUCT_METHOD(
        st, selectScreenCircle, MARGS("m", "tree", "co", "ray", "r1", "r2", "domain", "state"));
    BIND_STRUCT_METHOD(st,
                       selectScreenRect,
                       MARGS("m",
                             "tree",
                             "near0",
                             "near1",
                             "near2",
                             "near3",
                             "far0",
                             "far1",
                             "far2",
                             "far3",
                             "domain",
                             "state"));
    BIND_STRUCT_METHOD(st, setActiveElem, MARGS("domain", "idx"));
    BIND_STRUCT_METHOD(st, activeVert, MARGS());
    BIND_STRUCT_METHOD(st, activeEdge, MARGS());
    BIND_STRUCT_METHOD(st, activeFace, MARGS());

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
    if (m != active_mesh_) {
      /* Stamps index the active mesh's element ids; a mesh switch makes them
       * meaningless — invalidate wholesale. */
      bumpChunkStampGen();
    }
    active_mesh_ = m;
    // Bind the brush's save-gate columns up front (before any dab op fires a
    // callback) so stampUndoGate never allocs mid-stroke. See stampUndoGate.
    if (m) {
      vertGate_.ensure(*m);
      faceGate_.ensure(*m);
    }
  }

  void beginStep(bool hasDyntopo)
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

  /** Current stroke id, masked to 16 bits (see AttrSaver stamp packing). Starts
   * at 1 so a freshly-stamped 0 element always reads as "not saved yet". */
  int curStrokeId() const
  {
    return strokeId_ & 0xffff;
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

  /** Capture every topo chunk's end-state from the CURRENT mesh: the active
   * chunk's Created&&Live end_body, plus a data-column refresh of every chunk's
   * Created verts/faces. Idempotent per step (the `finalized` guard) so the
   * stroke-end auto-compaction can finalize early — it MUST, because it permutes
   * the live mesh afterwards: end_body holds TOPO connectivity by index, and
   * redo replays the topo chunks into the pre-reorder layout, so a post-reorder
   * capture corrupts the corner cycles (infinite loop in add_face). */
  void finalizeStroke()
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

  void endStep()
  {
    finalizeStroke();
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

  /** Lazily allocates a topo chunk in the current entry. */
  LogChunkTopo *getTopoChunk()
  {
    if (curEntry().topo_chunk_) {
      return curEntry().topo_chunk_;
    }
    pushTopoChunk();
    return curEntry().topo_chunk_;
  }

  /** Find-or-create the current step's per-domain element store (the
   * append-as-touched undo capture for AttrSaver-gated brush deformation). */
  LogChunkElems *elemStore(mesh::ElemType domain)
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

  /** Append a caller-built chunk (LogChunkTypes::External subclasses) to the
   * open step; MeshLog takes ownership and drives it purely through the
   * undo/redo virtuals. Aborts when no step is open (mirrors pushReorderChunk). */
  void appendChunk(LogChunk *chunk)
  {
    if (curStep_ < 0 || curStep_ >= entries.size()) {
      fprintf(stderr, "Error: appendChunk called with no current undo entry\n");
      abort();
    }
    curEntry().chunks.append(chunk);
  }

  /** Atomic reorder step: open a step, record the five permutations, close it.
   * The one sanctioned way for non-brush code (Scene locality reorder) to push
   * an undo step without driving beginStep/endStep itself. */
  LogChunkReorder *pushReorderStep(Vector<int> vmap,
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

  /** TS-app entry point for the "optimize mesh layout" button: compute locality
   * permutations from the tree, record an undoable reorder step, then apply it.
   * Mirrors debug Scene::reorderForLocality so the app path is fully
   * meshlog-aware. Pushes the maps (copied) before applying, matching the debug
   * ordering. No-op without a tree. */
  void reorderForLocality(spatial::SpatialTree *tree)
  {
    if (!tree) {
      return;
    }
    Vector<int> vmap, emap, cmap, lmap, fmap;
    tree->computeLocalityMaps(vmap, emap, cmap, lmap, fmap);
    pushReorderStep(vmap, emap, cmap, lmap, fmap);
    tree->applyReorderIncremental(vmap, emap, cmap, lmap, fmap);
    /* Element ids just moved; the recorded-in-chunk stamps are id-keyed. */
    bumpChunkStampGen();
  }

  /** Stroke-boundary auto-compaction (mechanism B). MUST be called with the
   * stroke's undo step still OPEN (before endStep): if the tree's vert page-
   * spread exceeds @p vertRatioThreshold (1.0 = perfectly compact), append an
   * incremental reorder CHUNK to the current step and apply it. Folding the
   * reorder into the stroke's step means one undo reverts stroke + compaction
   * together AND keeps the stroke's id-based chunks valid (the reorder chunk,
   * being last, is inverted FIRST on undo, restoring the pre-compaction ids the
   * earlier chunks expect). Returns true if it compacted. Cheap to call every
   * stroke — fragmentationStats is O(elements) and the gate skips the work until
   * churn has actually scattered the layout. */
  bool compactIfFragmented(spatial::SpatialTree *tree, double vertRatioThreshold = 3.0)
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
    Vector<int> *vals[5] = {&chunk->vval, &chunk->eval, &chunk->cval, &chunk->lval,
                            &chunk->fval};
    for (int k = 0; k < 5; k++) {
      *mvs[k] = moved[k];  // moved slots (from)
      vals[k]->resize(int(moved[k].size()));
      for (int i = 0; i < int(moved[k].size()); i++) {
        (*vals[k])[i] = (*maps[k])[moved[k][i]];  // target slots (to)
      }
    }
    curEntry().chunks.append(chunk);

    tree->applyReorderIncremental(vmap, emap, cmap, lmap, fmap, moved[0], moved[1],
                                  moved[2], moved[3], moved[4]);
    /* Element ids just moved; the recorded-in-chunk stamps are id-keyed. */
    bumpChunkStampGen();
    return true;
  }

  /* -------------------- Box-modeling topology macro-ops --------------------
   * Each brackets one MeshLog step (so it's one undo press), sets the active
   * mesh so the create/change/kill callbacks fire into the topo chunk, runs the
   * Euler-op composition (mesh/ops/*), and leaves the new movable region
   * selected. `outNormal` receives the op's averaged normal (3 floats) for the
   * chained transform's default constraint axis. The spatial tree is rebuilt by
   * the TS op afterward (topology changed wholesale). */

  /** selectFlush op-domain preference (mesh/utils/select_derive.h). Bound so the
   * TS ops mirror the sculptcore.select_flush_prefer_op_domain feature flag into
   * it before each macro-op (the auto_defrag pattern: flag read TS-side). */
  bool selectFlushPreferOpDomain = true;

  void extrudeRegion(mesh::Mesh *m, util::Vector<float> &outNormal)
  {
    if (!m) {
      return;
    }
    setActiveMesh(m);
    beginStep(false);
    mesh::ops::ExtrudeResult res;
    mesh::ops::extrudeRegion(*m, callbacks(), res, selectFlushPreferOpDomain);
    endStep();
    outNormal.append(res.normal[0]);
    outNormal.append(res.normal[1]);
    outNormal.append(res.normal[2]);
  }

  void extrudeIndividual(mesh::Mesh *m, util::Vector<float> &outNormal)
  {
    if (!m) {
      return;
    }
    setActiveMesh(m);
    beginStep(false);
    mesh::ops::ExtrudeResult res;
    mesh::ops::extrudeIndividual(*m, callbacks(), res, selectFlushPreferOpDomain);
    endStep();
    outNormal.append(res.normal[0]);
    outNormal.append(res.normal[1]);
    outNormal.append(res.normal[2]);
  }

  void extrudeWireVerts(mesh::Mesh *m, util::Vector<float> &outNormal)
  {
    if (!m) {
      return;
    }
    setActiveMesh(m);
    beginStep(false);
    mesh::ops::ExtrudeResult res;
    mesh::ops::extrudeWireVerts(*m, callbacks(), res, selectFlushPreferOpDomain);
    endStep();
    outNormal.append(res.normal[0]);
    outNormal.append(res.normal[1]);
    outNormal.append(res.normal[2]);
  }

  void splitFacesOff(mesh::Mesh *m, util::Vector<float> &outNormal)
  {
    if (!m) {
      return;
    }
    setActiveMesh(m);
    beginStep(false);
    mesh::ops::ExtrudeResult res;
    mesh::ops::splitFacesOff(*m, callbacks(), res, selectFlushPreferOpDomain);
    endStep();
    outNormal.append(res.normal[0]);
    outNormal.append(res.normal[1]);
    outNormal.append(res.normal[2]);
  }

  /* Subdivide the selected edges (or the selected faces' edges) with `numCuts`
   * cuts each (immediate; self-brackets a step). Outputs the created cut verts. */
  void subdivideEdges(mesh::Mesh *m, int numCuts, util::Vector<int> &outVerts)
  {
    if (!m) {
      return;
    }
    setActiveMesh(m);
    beginStep(false);
    mesh::ops::subdivideEdges(*m, callbacks(), numCuts, outVerts, selectFlushPreferOpDomain);
    endStep();
  }

  /* Loop-cut the quad strip through `seedEdge` (immediate; self-brackets). Outputs
   * the new loop's midpoint verts (left selected). */
  void loopCut(mesh::Mesh *m, int seedEdge, util::Vector<int> &outVerts)
  {
    if (!m) {
      return;
    }
    setActiveMesh(m);
    beginStep(false);
    mesh::ops::loopCut(*m, callbacks(), seedEdge, outVerts);
    endStep();
  }

  /* Loop-cut at a cursor ray: cast against `tree`, seed from the hit face's edge
   * nearest the hit point, then cut. Outputs the new loop's verts (selected). */
  void loopCutAtRay(mesh::Mesh *m,
                    spatial::SpatialTree *tree,
                    const math::float3 &origin,
                    const math::float3 &dir,
                    util::Vector<int> &outVerts)
  {
    if (!m || !tree) {
      return;
    }
    spatial::CastRayIsect isect;
    if (!tree->castRay(origin, dir, isect) || isect.faceIndex == ELEM_NONE) {
      return;
    }
    int seed = mesh::faceEdgeNearestPoint(*m, isect.faceIndex, isect.p);
    if (seed == ELEM_NONE) {
      return;
    }
    setActiveMesh(m);
    beginStep(false);
    mesh::ops::loopCut(*m, callbacks(), seed, outVerts);
    endStep();
  }

  /* Build the inset ring (parametric modal). Unlike the extrude wrappers this
   * does NOT bracket the step — the modal op holds one step open across the drag
   * (selectionBeginStep -> insetRegion -> drag setVertCo -> selectionEndStep), so
   * the created inset verts capture their final dragged positions at finalizeStep.
   * Outputs the inset vert indices + base coords + inward tangents (flat). */
  void insetRegion(mesh::Mesh *m,
                   util::Vector<int> &insetVerts,
                   util::Vector<float> &baseCo,
                   util::Vector<float> &tangent)
  {
    if (!m) {
      return;
    }
    setActiveMesh(m);
    mesh::ops::insetRegion(*m, callbacks(), insetVerts, baseCo, tangent,
                          selectFlushPreferOpDomain);
  }

  /* Bevel the selected verts (parametric modal; does NOT self-bracket, like
   * insetRegion). Outputs the offset verts + base coords + edge tangents. */
  void bevelVerts(mesh::Mesh *m,
                  util::Vector<int> &verts,
                  util::Vector<float> &baseCo,
                  util::Vector<float> &tangent)
  {
    if (!m) {
      return;
    }
    setActiveMesh(m);
    mesh::ops::bevelVerts(*m, callbacks(), verts, baseCo, tangent,
                          selectFlushPreferOpDomain);
  }

  /* -------------------- Box-modeling selection (undoable) --------------------
   * The per-element `select` bool is a normal (non-TOPO, non-NOCOPY) data column,
   * so snapshotting a changed element into the step's topo chunk via onChange (a
   * full-row capture) makes undo/redo swap `select` back exactly like positions.
   * onChange snapshots an Existed element only on first touch, so repeated writes
   * across a modal drag accumulate into a single undo step. The step bracketing is
   * split (begin/end) so a circle-brush drag owns one step; `domain` is 0 = vertex,
   * 1 = edge, 2 = face (a code, not an ElemType/SelMask flag — those disagree on
   * FACE). These are the sanctioned non-brush selection entry, like
   * reorderForLocality is for the locality reorder. */

  /** Open a selection step. Snapshots active elements for undo (see beginStep). */
  void selectionBeginStep()
  {
    beginStep(false);
  }

  /** Close the current selection step. */
  void selectionEndStep()
  {
    endStep();
  }

  static LogElemKind selectDomainKind(int domain)
  {
    switch (domain) {
      case 0:
        return LogElemKind::Vert;
      case 1:
        return LogElemKind::Edge;
      default:
        return LogElemKind::Face;
    }
  }

  /** Snapshot then set one element's select bool. Caller is inside a step. */
  void selectOne(mesh::Mesh *m, int domain, int idx, bool state)
  {
    if (!m || idx < 0) {
      return;
    }
    getTopoChunk()->onChange(selectDomainKind(domain), m, idx);
    switch (domain) {
      case 0:
        m->v.select.set(idx, state);
        break;
      case 1:
        m->e.select.set(idx, state);
        break;
      case 2:
        m->f.select.set(idx, state);
        break;
    }
  }

  /** Snapshot + set select for a list of element indices (reuses a spatial
   * query's bound out-vector as input). Caller is inside a step. */
  void selectIndices(mesh::Mesh *m, int domain, util::Vector<int> &indices, int state)
  {
    if (!m) {
      return;
    }
    if (m->topo_frozen) {
      m->thawTopo();
    }
    bool s = state != 0;
    for (int idx : indices) {
      selectOne(m, domain, idx, s);
    }
  }

  /** Snapshot + set select for every live element in `domain`. */
  void selectAllElems(mesh::Mesh *m, int domain, int state)
  {
    if (!m) {
      return;
    }
    if (m->topo_frozen) {
      m->thawTopo();
    }
    bool s = state != 0;
    switch (domain) {
      case 0:
        for (int i : m->v) {
          selectOne(m, 0, i, s);
        }
        break;
      case 1:
        for (int i : m->e) {
          selectOne(m, 1, i, s);
        }
        break;
      case 2:
        for (int i : m->f) {
          selectOne(m, 2, i, s);
        }
        break;
    }
  }

  /** Select the shortest edge-path from the active vertex to `vEnd`; `vEnd`
   * becomes the new active vertex (the draft's path-select). Returns the number
   * of path verts (0 if unreachable, but active still advances). Inside a step. */
  int selectShortestPath(mesh::Mesh *m, int vEnd, int state)
  {
    if (!m || active_vert_ < 0 || vEnd < 0) {
      if (m && vEnd >= 0) {
        active_vert_ = vEnd;
      }
      return 0;
    }
    util::Vector<int> path;
    if (!mesh::shortestEdgePath(m, active_vert_, vEnd, path) || path.size() < 2) {
      active_vert_ = vEnd;
      return 0;
    }
    if (m->topo_frozen) {
      m->thawTopo();
    }
    bool s = state != 0;
    for (int v : path) {
      selectOne(m, 0, v, s);
    }
    for (int i = 0; i + 1 < int(path.size()); i++) {
      int e = m->find_edge(path[i], path[i + 1]);
      if (e != ELEM_NONE) {
        selectOne(m, 1, e, s);
      }
    }
    active_vert_ = vEnd;
    return int(path.size());
  }

  /** Select the edge loop (kind 0), edge ring (kind 1), or face loop (kind 2)
   * seeded at `seedEdge` (the ctrl / ctrl-shift click select). A select of an
   * already fully-selected loop DESELECTS it instead (loop toggle). Pure
   * selection; caller brackets the step. Returns the element count walked,
   * negated when the toggle deselected. */
  int selectLoop(mesh::Mesh *m, int seedEdge, int kind, int state)
  {
    if (!m || seedEdge < 0) {
      return 0;
    }
    if (m->topo_frozen) {
      m->thawTopo();
    }
    util::Vector<int> elems;
    int domain = kind == 2 ? 2 : 1;
    if (kind == 2) {
      mesh::walkFaceLoop(*m, seedEdge, elems);
    } else if (kind == 0) {
      mesh::walkEdgeLoop(*m, seedEdge, elems);
    } else {
      mesh::walkEdgeRing(*m, seedEdge, elems);
    }
    bool s = state != 0;
    if (s && elems.size() > 0) {
      bool all = true;
      for (int el : elems) {
        if (!m->elemSelected(domain, el)) {
          all = false;
          break;
        }
      }
      if (all) {
        s = false;
      }
    }
    for (int el : elems) {
      selectOne(m, domain, el, s);
    }
    return s ? int(elems.size()) : -int(elems.size());
  }

  /* Select elements from a spatial query's collected face/vert sets, by domain.
   * vert → the collected verts; face → the collected faces; edge → edges whose
   * BOTH endpoints were collected (the natural "edge inside the region" rule).
   * Caller is inside a step. */
  void selectFromSets(mesh::Mesh *m,
                      int domain,
                      util::Vector<int> &faces,
                      util::Vector<int> &verts,
                      bool state)
  {
    if (!m) {
      return;
    }
    switch (domain) {
      case 0:
        for (int v : verts) {
          selectOne(m, 0, v, state);
        }
        break;
      case 2:
        for (int f : faces) {
          selectOne(m, 2, f, state);
        }
        break;
      case 1: {
        util::Set<int> vset;
        for (int v : verts) {
          vset.add(v);
        }
        for (int v : verts) {
          for (int e : m->e_of_v(v)) {
            int other = m->e.vs[e][0] == v ? m->e.vs[e][1] : m->e.vs[e][0];
            if (vset.contains(other)) {
              selectOne(m, 1, e, state);
            }
          }
        }
        break;
      }
    }
  }

  /* Cone (circle/brush) select: run the spatial cone query and select the hits
   * in `domain`. Pick + select happen entirely in C++ so no index array crosses
   * the binding. Caller brackets the step (one step per drag for the brush). */
  void selectScreenCircle(mesh::Mesh *m,
                          spatial::SpatialTree *tree,
                          const math::float3 &co,
                          const math::float3 &ray,
                          float r1,
                          float r2,
                          int domain,
                          int state)
  {
    if (!m || !tree) {
      return;
    }
    if (m->topo_frozen) {
      m->thawTopo();
    }
    util::Vector<int> faces, verts;
    tree->castScreenCircle(co, ray, r1, r2, faces, verts);
    selectFromSets(m, domain, faces, verts, state != 0);
  }

  /* Box select: run the spatial frustum query (8 object-local corners, like
   * SpatialTree::castScreenRect) and select the hits in `domain`. */
  void selectScreenRect(mesh::Mesh *m,
                        spatial::SpatialTree *tree,
                        const math::float3 &near0,
                        const math::float3 &near1,
                        const math::float3 &near2,
                        const math::float3 &near3,
                        const math::float3 &far0,
                        const math::float3 &far1,
                        const math::float3 &far2,
                        const math::float3 &far3,
                        int domain,
                        int state)
  {
    if (!m || !tree) {
      return;
    }
    if (m->topo_frozen) {
      m->thawTopo();
    }
    util::Vector<int> faces, verts;
    tree->castScreenRect(near0, near1, near2, near3, far0, far1, far2, far3, faces, verts);
    selectFromSets(m, domain, faces, verts, state != 0);
  }

  /** Set the active element for a domain (0/1/2). Inside a step so it rides undo. */
  void setActiveElem(int domain, int idx)
  {
    switch (domain) {
      case 0:
        active_vert_ = idx;
        break;
      case 1:
        active_edge_ = idx;
        break;
      case 2:
        active_face_ = idx;
        break;
    }
  }

  int activeVert() const
  {
    return active_vert_;
  }
  int activeEdge() const
  {
    return active_edge_;
  }
  int activeFace() const
  {
    return active_face_;
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
  }

  void redo(mesh::Mesh *m, spatial::SpatialTree *tree)
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
    curStep_++;
  }

private:
  /* Swap the live active elements with this step's snapshot. Symmetric: undo
   * swaps live(post-step)↔snap(pre-step) → live becomes pre-step; redo swaps
   * again → live becomes post-step. */
  void swapActiveElems(LogEntry &e)
  {
    std::swap(active_vert_, e.snapActiveVert);
    std::swap(active_edge_, e.snapActiveEdge);
    std::swap(active_face_, e.snapActiveFace);
  }


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

  /* Stamp the brush's per-element save-gate (`.strokeid.<domain>`) so the brush
   * deform — which runs AFTER dyntopo each dab — treats this element as already
   * saved and skips appending it to the per-step element store. The topo chunk
   * captured this element's true pre-step body on first touch, so its restore is
   * authoritative; an element-store row would hold a stale post-dyntopo value
   * and, being older than later dabs' topo chunks, would win the newest-first
   * undo and re-corrupt the element. Only the brush-gated domains (vert co/no,
   * face no) need stamping. Stamp the full flag set so any brush save mask is
   * covered. */
  void stampUndoGate(LogElemKind kind, int idx)
  {
    if (kind == LogElemKind::Vert) {
      vertGate_.updateSaved(idx, curStrokeId(), 0xffff);
    } else if (kind == LogElemKind::Face) {
      faceGate_.updateSaved(idx, curStrokeId(), 0xffff);
    }
  }

  /* Per-domain "already recorded in the current topo chunk" stamps — the
   * no-op fast path (plan 2026-07-12-2141 M1). 65% of dab events re-touch an
   * element the active chunk already recorded; a dense generation-stamp read
   * answers that without the makeKey/hash lookup or the redundant undo-gate
   * write. Stamps are set only by the slow path below (right after the chunk
   * records the element), cleared by onKill (index reuse must re-record), and
   * invalidated wholesale by a generation bump per new chunk / active-mesh
   * switch — so a stale hit is impossible by construction. */
  litestl::util::Vector<uint32_t> chunk_stamp_[5];
  uint32_t chunk_stamp_gen_ = 1;

  void bumpChunkStampGen()
  {
    if (++chunk_stamp_gen_ == 0) {
      for (int k = 0; k < 5; k++) {
        chunk_stamp_[k].clear();
      }
      chunk_stamp_gen_ = 1;
    }
  }

  bool chunkStampHit(LogElemKind kind, int idx) const
  {
    const litestl::util::Vector<uint32_t> &s = chunk_stamp_[int(kind)];
    return uint32_t(idx) < uint32_t(s.size()) && s[idx] == chunk_stamp_gen_;
  }

  void setChunkStamp(LogElemKind kind, int idx)
  {
    litestl::util::Vector<uint32_t> &s = chunk_stamp_[int(kind)];
    if (uint32_t(idx) >= uint32_t(s.size())) {
      litestl::alloc::PermanentGuard guard; /* persistent buffer: not a leak */
      int old = int(s.size());
      s.resize(idx + 1);
      for (int i = old; i <= idx; i++) {
        s[i] = 0;
      }
    }
    s[idx] = chunk_stamp_gen_;
  }

  void clearChunkStamp(LogElemKind kind, int idx)
  {
    litestl::util::Vector<uint32_t> &s = chunk_stamp_[int(kind)];
    if (uint32_t(idx) < uint32_t(s.size())) {
      s[idx] = 0;
    }
  }

  void installCallbacks()
  {
    auto fwd = [this](LogElemKind kind) {
      return [this, kind](int idx) {
        if (!active_mesh_) {
          return;
        }
        /* Fast path: element already recorded by the active chunk — nothing
         * to capture, and its undo gate was stamped on first touch. */
        if (chunkStampHit(kind, idx)) {
          return;
        }
        getTopoChunk()->onChange(kind, active_mesh_, idx);
        setChunkStamp(kind, idx);
        stampUndoGate(kind, idx);
      };
    };
    auto fwdCreate = [this](LogElemKind kind) {
      return [this, kind](int idx) {
        if (!active_mesh_) {
          return;
        }
        getTopoChunk()->onCreate(kind, active_mesh_, idx);
        setChunkStamp(kind, idx);
        stampUndoGate(kind, idx);
      };
    };
    auto fwdKill = [this](LogElemKind kind) {
      return [this, kind](int idx) {
        if (!active_mesh_) {
          return;
        }
        getTopoChunk()->onKill(kind, active_mesh_, idx);
        clearChunkStamp(kind, idx);
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
  int strokeId_ = 0; // bumped to 1 on the first beginStep (see curStrokeId)
  /* Box-modeling active element per domain (vert/edge/face index, -1 = none).
   * Snapshotted per step in LogEntry; see swapActiveElems / setActiveElem. */
  int active_vert_ = -1;
  int active_edge_ = -1;
  int active_face_ = -1;
  /* Brush save-gate stampers, shared (by attribute name) with the brush kernels'
   * own AttrSavers — see stampUndoGate. */
  AttrSaver<mesh::ElemType::VERTEX> vertGate_;
  AttrSaver<mesh::ElemType::FACE> faceGate_;
};

} // namespace sculptcore::meshlog
