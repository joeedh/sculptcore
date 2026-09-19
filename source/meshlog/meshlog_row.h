/**
# RowLayout / ChunkElemRow

Shared row layout plus single-row attribute snapshots for topological undo
(`LogChunkTopo`). Split out of `meshlog_base.h` to keep it from growing into
one monster header.
*/

#pragma once

#include "litestl/util/vector.h"
#include "mesh/attribute.h"
#include "mesh/attribute_bool.h"
#include "mesh/attribute_enums.h"
#include "mesh/mesh.h"
#include "meshlog_types.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace sculptcore::meshlog {
using litestl::util::Vector;

namespace detail {

/** Shared per-domain row layout: byte offsets/sizes for every attr of one
 * AttrGroup, computed once per (chunk, domain, attr-count) instead of per
 * captured row — the per-row recompute measured ~24% of a dyntopo dab's
 * wall. `count` doubles as the append-guard prefix: rows built on a layout
 * restore only the attrs that existed when it was built, so an AttrGroup
 * that has attrs *appended* between capture and replay stays safe (the new
 * trailing columns are simply not restored). Reordering is unsupported.
 * Owned by the LogChunkTopo whose rows reference it. */
struct RowLayout {
  int count = 0;
  int total = 0;
  bool skipTopo = false;
  Vector<int> offsets;
  Vector<int> sizes; /* bytes per attr cell (BOOL = 1) */

  /* Byte offsets of the AttrType::WEIGHTS cells, so a row can retain/release
   * them without re-walking a live AttrGroup — which it has none of at
   * destruction time. Empty for the overwhelmingly common weightless mesh. */
  Vector<int, 2> weight_cells;
  /* Keeps that pool addressable for as long as any row built on this layout;
   * ~LogChunkTopo clears the row pools before deleting its layouts. */
  mesh::DeformPoolUser pool;

  void build(mesh::AttrGroup &src, bool skipTopology = false)
  {
    skipTopo = skipTopology;
    count = int(src.attrs.size());
    offsets.resize(count);
    sizes.resize(count);
    weight_cells.clear();
    total = 0;
    for (int i = 0; i < count; i++) {
      mesh::AttrRef &ref = src.attrs[i];
      int sz = (ref.type == mesh::AttrType::BOOL) ? 1 : int(ref.data->elemSize);
      offsets[i] = total;
      sizes[i] = sz;
      if (ref.type == mesh::AttrType::WEIGHTS && !(ref.flag & mesh::AttrFlag::NOCOPY)) {
        weight_cells.append(total);
      }
      total += sz;
    }
    if (weight_cells.size() > 0) {
      pool.reset(src.deform_pool);
    }
  }

  double memSize() const
  {
    return double(sizeof(*this)) + double(offsets.size() + sizes.size()) * sizeof(int);
  }
};

/**
 * Single-row attribute snapshot for one element in an AttrGroup, laid out
 * by a shared RowLayout (which must outlive the row — both are owned by the
 * same LogChunkTopo).
 */
struct ChunkElemRow {
  ChunkElemRow() = default;

  /* Rows are pooled (LogChunkTopo::bodies_pool), so both release-and-reuse and
   * pool teardown land here — that is what settles a captured run's reference. */
  ~ChunkElemRow()
  {
    releaseWeights();
  }

  void captureFrom(const RowLayout *plan, mesh::AttrGroup &src, int src_idx)
  {
    releaseWeights(); // a reused row still names the previous element's runs
    layout_ = plan;
    data_.resize(plan->total);
    /* NOCOPY cells are skipped below and must restore as zeros (their
     * "default state" contract in writeTo) — pooled rows reuse buffers. */
    memset(data_.data(), 0, size_t(plan->total));
    for (int i = 0; i < plan->count; i++) {
      mesh::AttrRef &ref = src.attrs[i];

      // TEMP attrs (e.g. .spatial.*.node) are derived state owned by the
      // spatial tree, not authoritative undo data — skip them so incremental
      // tree updates during a logged step don't taint replay.
      if ((ref.flag & mesh::AttrFlag::NOCOPY) ||
          (plan->skipTopo && (ref.flag & mesh::AttrFlag::TOPO)))
      {
        continue;
      }
      uint8_t *dst = data_.data() + plan->offsets[i];

      if (ref.type == mesh::AttrType::BOOL) {
        mesh::BoolAttrView *view = static_cast<mesh::BoolAttrView *>(ref.data);
        dst[0] = view->get(src_idx) ? 1 : 0;
      } else {
        const void *src = ref.data->getElemData(src_idx);
        if (!src) { // unmaterialized page (frozen-topo column?) — see warnNullPage
          warnNullPage("captureFrom", ref);
          continue;
        }
        memcpy(static_cast<void *>(dst), src, size_t(plan->sizes[i]));
      }
    }
    retainWeights();
  }

  void writeTo(mesh::AttrGroup &dst, int dst_idx)
  {
    if (!layout_) {
      return; /* never captured */
    }
    int n = dst.attrs.size() < layout_->count ? int(dst.attrs.size()) : layout_->count;
    for (int i = 0; i < n; i++) {
      mesh::AttrRef &ref = dst.attrs[i];
      if (layout_->skipTopo && (ref.flag & mesh::AttrFlag::TOPO))
        continue;
      if (ref.flag & mesh::AttrFlag::NOCOPY) {
        // note: since this is called on element re-creation,
        // we want to restore nocopy attrs to their default states
        // (captured cells stay zeroed for NOCOPY)
      }
      uint8_t *src = data_.data() + layout_->offsets[i];

      if (ref.type == mesh::AttrType::BOOL) {
        mesh::BoolAttrView *view = static_cast<mesh::BoolAttrView *>(ref.data);
        view->set(dst_idx, src[0] != 0);
      } else {
        void *dst = ref.data->getElemData(dst_idx);
        if (!dst) {
          warnNullPage("writeTo", ref);
          continue;
        }
        if (ref.type == mesh::AttrType::WEIGHTS && layout_->pool.ptr) {
          // The element is being re-created, so its cell is a fresh default —
          // but reassign is right either way, and the row keeps its own copy.
          layout_->pool.ptr->reassign(*static_cast<mesh::WeightSlot *>(dst),
                                      *reinterpret_cast<const mesh::WeightSlot *>(src));
          continue;
        }
        memcpy(dst, static_cast<const void *>(src), size_t(layout_->sizes[i]));
      }
    }
  }

  /* Re-read only the brush-deformable data columns (skip TOPO connectivity and
   * NOCOPY temp state) into the already-laid-out buffer, leaving the rest of
   * end_body frozen. Used to refresh a Created vert's captured position with its
   * final post-stroke value (see LogChunkTopo::refreshCreatedVertData). */
  void refreshDataColumns(mesh::AttrGroup &src, int src_idx)
  {
    if (!layout_) {
      return; /* never captured */
    }
    int n = src.attrs.size() < layout_->count ? int(src.attrs.size()) : layout_->count;
    for (int i = 0; i < n; i++) {
      mesh::AttrRef &ref = src.attrs[i];
      if ((ref.flag & mesh::AttrFlag::TOPO) || (ref.flag & mesh::AttrFlag::NOCOPY)) {
        continue;
      }
      uint8_t *dst = data_.data() + layout_->offsets[i];
      if (ref.type == mesh::AttrType::BOOL) {
        mesh::BoolAttrView *view = static_cast<mesh::BoolAttrView *>(ref.data);
        dst[0] = view->get(src_idx) ? 1 : 0;
      } else {
        const void *s = ref.data->getElemData(src_idx);
        if (!s) {
          continue;
        }
        if (ref.type == mesh::AttrType::WEIGHTS && layout_->pool.ptr) {
          layout_->pool.ptr->reassign(*reinterpret_cast<mesh::WeightSlot *>(dst),
                                      *static_cast<const mesh::WeightSlot *>(s));
          continue;
        }
        memcpy(static_cast<void *>(dst), s, size_t(layout_->sizes[i]));
      }
    }
  }

  double memSize()
  {
    /* The shared RowLayout is counted once by the owning chunk. */
    return double(sizeof(*this)) + double(data_.size());
  }

  void swapWith(mesh::AttrGroup &live, int live_idx)
  {
    if (!layout_) {
      return; /* never captured */
    }
    uint8_t buf[64];
    int n = live.attrs.size() < layout_->count ? int(live.attrs.size()) : layout_->count;
    for (int i = 0; i < n; i++) {
      mesh::AttrRef &ref = live.attrs[i];
      if ((ref.flag & mesh::AttrFlag::NOCOPY) ||
          (layout_->skipTopo && (ref.flag & mesh::AttrFlag::TOPO)))
      {
        continue;
      }
      uint8_t *slot = data_.data() + layout_->offsets[i];

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
        // WEIGHTS needs no special case: the row and the mesh exchange slot
        // indices, so the reference each holds moves with it.
        size_t n = size_t(layout_->sizes[i]);
        memcpy(static_cast<void *>(buf), live_p, n);
        memcpy(live_p, static_cast<const void *>(slot), n);
        memcpy(static_cast<void *>(slot), static_cast<const void *>(buf), n);
      }
    }
  }

private:
  /** The row's own WEIGHTS cells, or null when it has none / was never
   * captured. Both loops below are no-ops on a weightless mesh. */
  mesh::WeightSlot *weightCell(int offset)
  {
    return reinterpret_cast<mesh::WeightSlot *>(data_.data() + offset);
  }

  void retainWeights()
  {
    if (!layout_ || !layout_->pool.ptr) {
      return;
    }
    for (int offset : layout_->weight_cells) {
      layout_->pool.ptr->retain(*weightCell(offset));
    }
  }

  void releaseWeights()
  {
    if (!layout_ || !layout_->pool.ptr || int(data_.size()) < layout_->total) {
      return;
    }
    for (int offset : layout_->weight_cells) {
      mesh::WeightSlot *cell = weightCell(offset);
      layout_->pool.ptr->release(*cell);
      *cell = mesh::WeightSlot();
    }
  }

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

  Vector<uint8_t> data_;
  /* Shared layout this row was captured with; owned by the same chunk. */
  const RowLayout *layout_ = nullptr;
};

} // namespace detail

} // namespace sculptcore::meshlog
