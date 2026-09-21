/**
# LogChunkElems

Sparse, append-as-touched per-domain attribute-swap log for plain vertex-
position / paint sculpting. See the file-level intro in `meshlog_base.h`.
Split out of that file to keep it from growing into one monster header.
*/

#pragma once

#include "attr_saver.h"
#include "litestl/util/map.h"
#include "litestl/util/set.h"
#include "litestl/util/span.h"
#include "litestl/util/vector.h"
#include "mesh/attribute.h"
#include "mesh/attribute_bool.h"
#include "mesh/attribute_builtin.h"
#include "mesh/attribute_enums.h"
#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "meshlog_types.h"
#include "spatial/node.h"
#include "spatial/spatial.h"
#include <cstdio>
#include <cstring>

namespace sculptcore::meshlog {
using litestl::util::string;
using litestl::util::Vector;

namespace detail {
struct ChunkElemData {
  /** Declared first so it destructs LAST — after attrs_, whose ~AttrGroup
   * releases this store's weight slots back into the pool. The mesh is routinely
   * deleted before the log that logged it, so the reference is what keeps the
   * pool addressable that long. */
  mesh::DeformPoolUser pool_user;

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
    bindPool(src);
    return ensureAttr(index, ref.type, ref.name);
  }

  /** Point this store's group at the same DeformPool the mesh column uses, and
   * take a user. Both halves are required before a WEIGHTS column can be
   * created: AttrGroup::ensure asserts on the pool, and the rows this store
   * copies are raw slot indices, only resolvable against that one pool. */
  void bindPool(const mesh::AttrGroup &src)
  {
    // Bound once. Re-pointing at a second pool would strand the references the
    // already-captured rows hold in the first one.
    if (!src.deform_pool || attrs_.deform_pool) {
      return;
    }
    pool_user.reset(src.deform_pool);
    attrs_.deform_pool = src.deform_pool;
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

      /* srcData's page for src_i may be lazily unmaterialized — most captures
       * only ever touch co/no (eagerly materialized), but capturePreviewRegion
       * sweeps every non-NOCOPY attribute, including sparse ones (e.g. mask,
       * cavity) that a never-touched vertex has no backing page for yet. */
      srcData->materializeElem(src_i);

      if (ref.type == AttrType::WEIGHTS && attrs_.deform_pool) {
        // A plain memcpy would duplicate the slot index without a reference, and
        // the pool would then reclaim a run this row still names.
        dstData->materializeElem(dst_i);
        attrs_.deform_pool->reassign(
            *static_cast<WeightSlot *>(dstData->getElemData(dst_i)),
            *static_cast<const WeightSlot *>(srcData->getElemData(src_i)));
        continue;
      }

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

  /* Grow the store by `n` unfilled rows (columns pre-registered via ensureAttr
   * and materialized here), returning the first new row index. The parallel
   * capture (parallel_capture.h) then fills disjoint row ranges with cpyFrom
   * from multiple threads. */
  int appendRows(int n)
  {
    int base = size_;
    size_ += n;
    attrs_.ensure_capacity(size_);
    return base;
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

      /* See cpyFrom(): srcData's page for src_i may be lazily unmaterialized. */
      srcData->materializeElem(src_i);

      // A WEIGHTS column needs nothing special here: the two sides exchange slot
      // indices, so the reference each already holds simply moves with it.
      memcpy(static_cast<void *>(buf), dstData->getElemData(dst_i), dstData->elemSize);
      memcpy(dstData->getElemData(dst_i), srcData->getElemData(src_i), dstData->elemSize);
      memcpy(srcData->getElemData(src_i), static_cast<void *>(buf), dstData->elemSize);
    }
  };

  // A row is appended the first time an element is touched in a step, so an
  // element hit again later in the same step is normally gated out by
  // AttrSaver — but a rolled-back preview dab's own capture can leave a stale
  // row behind (its needsData() gate resets on rollback, so a later real
  // touch appends a second row for the same origIndex). Two rows for one
  // origIndex are captured oldest-first; undoing must therefore unwind
  // newest-first (reverse) and redoing must replay oldest-first (forward) —
  // rows are chained (mesh <-> row[N] <-> row[N-1] <-> ... ), and visiting
  // them out of order strands the element on an intermediate value instead
  // of its true endpoint.
  void swap(mesh::AttrGroup &src, spatial::SpatialTree *tree)
  {
    updateSrcAttrMap(tree->m);

    if (isSwapped) {
      for (int i = 0; i < size_; i++) {
        this->swapWith(src, origIndex[i], i);
      }
    } else {
      for (int i = size_ - 1; i >= 0; i--) {
        this->swapWith(src, origIndex[i], i);
      }
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
   * regenerate. Vertex/face domains carry a spatial node attribute; a corner
   * maps to its face's owner (via live topo links — thaw first). Without the
   * corner branch, an undo that swaps corner UV rows back (the reprojection
   * capture) leaves the viewport's attribute streams showing the undone UVs. */
  void update_nodes(mesh::Mesh *m, spatial::SpatialTree *tree)
  {
    using namespace sculptcore::spatial;
    if ((domain == mesh::ElemType::CORNER || domain == mesh::ElemType::FACE) && m->topo_frozen) {
      m->thawTopo();
    }
    for (int i : util::IndexRange(0, data.size())) {
      int idx = data.origIndex[i];
      int ni = 0;
      if (domain == mesh::ElemType::VERTEX) {
        ni = tree->treeMesh.v.node[idx];
      } else if (domain == mesh::ElemType::FACE) {
        ni = tree->treeMesh.f.node[idx];
        // A face row carries the poly-group id and the row swap bypasses the
        // boundary mutators: dirty the face (walks live corner links, hence
        // the thaw above) so the lazy recompute re-derives its border edges.
        if (idx >= 0 && idx < int(m->f.capacity()) && !m->f.freemap[idx]) {
          mesh::boundary::markFaceDirty(m, idx);
        }
      } else if (domain == mesh::ElemType::CORNER) {
        if (idx < 0 || idx >= int(m->c.capacity()) || m->c.freemap[idx]) {
          continue;
        }
        // Attribute streams only — geometry/normals/bounds are untouched by a
        // corner-row swap.
        ni = tree->treeMesh.f.node[m->l.f[m->c.l[idx]]];
        if (ni) {
          tree->node_from_id(ni)->update(NodeFlags::Spatial_UpdateGPU);
        }
        continue;
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

} // namespace sculptcore::meshlog
