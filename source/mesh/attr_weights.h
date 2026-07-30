#pragma once

/**
 * Typed access to an AttrType::WEIGHTS column.
 *
 * The column is an ordinary AttrData<WeightSlot>: a flat, 4-byte-per-element
 * paged column, so reorder, swap, resize, serialization and the meshlog's raw
 * byte copies all keep working on it untouched. What the column does not carry
 * is the reference discipline — a slot index is only meaningful while the
 * DeformPool still holds a reference for it. WeightsRef is that discipline:
 * every store goes through DeformPool::reassign (retain the new, release the
 * old), and every run read copies out, because a pointer into a shard's arena
 * dangles across the next intern().
 *
 * Write weights through WeightsRef, never through AttrRef::get_data<WeightSlot>().
 */

#include "attribute.h"
#include "deform_pool.h"

namespace sculptcore::mesh {

struct Mesh;

/** The vertex-group layer the c-api's bulk accessors and the addon bridge speak
 * for. Blender carries exactly one MDeformVert table per mesh, so a layer-name
 * argument on those accessors would only ever take this value. */
inline constexpr const char *VERT_WEIGHTS = ".vertex_groups";

struct WeightsRef {
  WeightsRef() = default;

  WeightsRef(AttrGroup &group, AttrRef &attr)
  {
    if (attr.type == AttrType::WEIGHTS && attr.data && group.deform_pool) {
      data_ = static_cast<AttrData<WeightSlot> *>(attr.data);
      pool_ = group.deform_pool;
    }
  }

  bool exists() const
  {
    return data_ != nullptr;
  }

  DeformPool *pool() const
  {
    return pool_;
  }

  /** The element's slot. Safe on an unmaterialized page (reads its default). */
  WeightSlot slot(int elem) const
  {
    return data_->safe_get(elem);
  }

  void setSlot(int elem, WeightSlot slot)
  {
    data_->materialize(elem);
    pool_->reassign((*data_)[elem], slot);
  }

  /** Intern `run` and store it. `run` need not be sorted or deduplicated. */
  void setRun(int elem, util::span<const DeformWeight> run)
  {
    WeightSlot next = pool_->intern(run);
    setSlot(elem, next);
    // intern() handed us a reference and setSlot took its own.
    pool_->release(next);
  }

  void clear(int elem)
  {
    setSlot(elem, WeightSlot(0));
  }

  int runSize(int elem) const
  {
    return pool_->runSize(slot(elem));
  }

  int getRun(int elem, DeformWeight *out, int max) const
  {
    return pool_->copyRun(slot(elem), out, max);
  }

  float weight(int elem, int group) const
  {
    return pool_->weight(slot(elem), group);
  }

  /** Append one entry per reference this column holds — every element of every
   * materialized page, free elements included, since nothing releases a slot
   * when its element is freed. Feeds DeformPool::auditRefcounts. */
  void collectRoots(util::Vector<WeightSlot> &out) const;

private:
  AttrData<WeightSlot> *data_ = nullptr;
  DeformPool *pool_ = nullptr;
};

/** Find or create a WEIGHTS layer on `mesh`'s vertex domain, creating the
 * mesh's DeformPool on first use. The entry point for every weights writer —
 * AttrGroup::ensure alone cannot do it, since it has no way to reach the pool. */
WeightsRef ensureVertWeights(Mesh &mesh, const string &name);

/** The existing WEIGHTS layer, or a WeightsRef whose exists() is false. */
WeightsRef findVertWeights(Mesh &mesh, const string &name);

} // namespace sculptcore::mesh
