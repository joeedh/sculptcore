#pragma once

#include "binding/binding_constructor_builder.h"
#include "litestl/math/vector.h"
#include "litestl/util/map.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"
#include "mesh/attribute.h"
#include "mesh/attribute_bool.h"
#include "mesh/attribute_builtin.h"
#include "mesh/attribute_enums.h"
#include "mesh/mesh.h"
#include "spatial/node.h"
#include "spatial/spatial.h"

namespace sculptcore::meshlog {
using litestl::math::float3;
using litestl::util::string;
using litestl::util::Vector;

enum _LogChunkTypes {
  Simple = 0,
  // Topo = 1,
};
MAKE_ENUM_CLASS(LogChunkTypes, _LogChunkTypes, int);

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
    node->update(NodeFlags::Spatial_UpdateGPU || NodeFlags::Spatial_RegenBounds);
  }

  void redo(mesh::Mesh *m, spatial::SpatialTree *tree)
  {
    using namespace sculptcore::spatial;

    v.redo(m->v.attrs, tree);
    e.redo(m->e.attrs, tree);
    c.redo(m->c.attrs, tree);
    f.redo(m->f.attrs, tree);

    SpatialNode *node = tree->node_from_id(nodeId);
    node->update(NodeFlags::Spatial_UpdateGPU || NodeFlags::Spatial_RegenBounds);
  }

private:
  mesh::AttrGroup attrs_;
  Vector<int> srcAttrMap_; // one-to-one mapping to attributes in attrs_
  int size_;
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
  }
  ~MeshLog() = default;

  void beginStep()
  {
    if (curStep_ != entries.size()) {
      /** thoeretically this should call all the right destructors */
      entries.resize(curStep_);
    }
    entries.grow_one();
    printf("beginStep called curStep_=%d\n", curStep_);
  }

  void endStep()
  {
    curStep_++;
    printf("endStep called curStep_=%d\n", curStep_);
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

    printf("undo: this: %p curStep_: %d, entries: %d\n", this, curStep_, int(entries.size()));
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
    printf("redo: this: %p curStep_: %d, entries: %d\n", this, curStep_, int(entries.size()));
    for (LogChunk *chunk : curEntry().chunks) {
      chunk->redo(m, tree);
    }
    curStep_++;
  }

private:
  int curStep_;
};

} // namespace sculptcore::meshlog
