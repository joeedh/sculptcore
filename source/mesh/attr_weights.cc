#include "attr_weights.h"

#include "mesh.h"

#include <algorithm>

namespace sculptcore::mesh {

namespace detail {

void weightsReleaseElem(AttrDataBase *data, DeformPool *pool, int elem)
{
  if (!data || !pool) {
    return;
  }

  auto *col = static_cast<AttrData<WeightSlot> *>(data);
  if (elem < 0 || elem >= col->size()) {
    return;
  }

  auto &page = col->pages[elem >> ATTR_PAGESHIFT];
  if (!page.exists) {
    return; // unmaterialized: every element in it still reads the page default
  }

  WeightSlot &slot = page.data[elem & ATTR_PAGEMASK];
  pool->release(slot);
  slot = WeightSlot();
}

void weightsReleaseFrom(AttrDataBase *data, DeformPool *pool, int start)
{
  if (!data || !pool) {
    return;
  }

  auto *col = static_cast<AttrData<WeightSlot> *>(data);
  start = std::max(start, 0);

  const int npages = int(col->pages.size());
  for (int p = start >> ATTR_PAGESHIFT; p < npages; p++) {
    auto &page = col->pages[p];
    if (!page.exists) {
      continue;
    }

    // Whole page past the first: entries beyond size_ are slot 0, and release()
    // short-circuits on that before it takes any lock.
    const int lo = std::max(start - (p << ATTR_PAGESHIFT), 0);
    for (int i = lo; i < ATTR_PAGESIZE; i++) {
      pool->release(page.data[i]);
      page.data[i] = WeightSlot();
    }
  }
}

} // namespace detail

void WeightsRef::collectRoots(util::Vector<WeightSlot> &out) const
{
  if (!data_) {
    return;
  }

  for (auto &page : data_->pages) {
    if (!page.exists) {
      continue;
    }
    for (int i = 0; i < ATTR_PAGESIZE; i++) {
      if (page.data[i].index != 0) {
        out.append(page.data[i]);
      }
    }
  }
}

WeightsRef ensureVertWeights(Mesh &mesh, const string &name)
{
  mesh.deformPool();
  return WeightsRef(mesh.v.attrs, mesh.v.attrs.ensure(AttrType::WEIGHTS, name));
}

WeightsRef findVertWeights(Mesh &mesh, const string &name)
{
  for (AttrRef &attr : mesh.v.attrs.attrs) {
    if (attr.type == AttrType::WEIGHTS && attr.name == name) {
      return WeightsRef(mesh.v.attrs, attr);
    }
  }
  return WeightsRef();
}

} // namespace sculptcore::mesh
