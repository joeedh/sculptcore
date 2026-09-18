#pragma once

#include "enhance.h"
#include "litestl/util/map.h"
#include <algorithm>
#include <memory>

namespace sculptcore::brush {

/** Stroke-local first-contact vectors, shared only by equivalent configurations. */
class PreparedEnhanceCache {
public:
  struct Entry {
    EnhanceParams params;
    litestl::util::Map<int, float3> vectors;
  };

  void reset()
  {
    entries_.clear();
    active_.resize(0, false);
    scratch_ = EnhanceScratch();
    geometry_ = nullptr;
    topology_ = 0;
    evaluations_ = 0;
  }

  void beginGeometry(mesh::Mesh *mesh)
  {
    if (geometry_ != mesh || topology_ != mesh->topo_stamp) {
      reset();
      geometry_ = mesh;
      topology_ = mesh->topo_stamp;
    }
    active_.resize(int(mesh->v.capacity()), false);
  }

  Entry &entry(int rings, int inner)
  {
    rings = std::max(rings, 1);
    inner = std::clamp(inner, 0, rings);
    for (auto &item : entries_)
      if (item->params.rings == rings && item->params.inner == inner)
        return *item;
    auto created = std::make_unique<Entry>();
    created->params = {rings, inner};
    entries_.append(std::move(created));
    return *entries_.last();
  }

  void fill(Entry &entry, mesh::Mesh *mesh, int vertex)
  {
    if (auto *cached = entry.vectors.lookup_ptr(vertex)) {
      write(vertex, *cached);
    } else {
      const float3 value = computeEnhanceDisp(mesh, vertex, entry.params, scratch_);
      entry.vectors.add(vertex, value);
      evaluations_++;
      write(vertex, value);
    }
  }

  void write(int vertex, float3 value)
  {
    active_.materialize(vertex);
    active_[vertex] = value;
  }

  mesh::AttrData<float3> *active()
  {
    return &active_;
  }
  size_t evaluations() const
  {
    return evaluations_;
  }
  size_t configurations() const
  {
    return entries_.size();
  }

private:
  litestl::util::Vector<std::unique_ptr<Entry>> entries_;
  mesh::AttrData<float3> active_{util::string(".prepared.enhance"), 0};
  EnhanceScratch scratch_;
  mesh::Mesh *geometry_ = nullptr;
  uint64_t topology_ = 0;
  size_t evaluations_ = 0;
};

} // namespace sculptcore::brush
