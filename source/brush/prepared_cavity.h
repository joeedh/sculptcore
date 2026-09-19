#pragma once

#include "automask.h"
#include "litestl/util/map.h"

#include <array>
#include <memory>

namespace sculptcore::brush {

/** Each stage owns its contact region even when the program selects all leaves. */
inline bool preparedCavityContact(const Brush &brush,
                                  bool unbounded,
                                  const float3 &point,
                                  const float3 &center,
                                  const float3 &normal)
{
  if (!unbounded)
    return brush.insideFalloff(point - center, normal);
  if (brush.unboundedExtent <= 0)
    return true;
  const float cutoff = brush.radius * brush.unboundedExtent;
  return std::hypot(double(point[0]) - double(center[0]),
                    double(point[1]) - double(center[1]),
                    double(point[2]) - double(center[2])) < double(cutoff);
}

/** First-contact cavity factors owned by one prepared stroke transaction. */
class PreparedCavityCache {
public:
  struct Key {
    int blur;
    float factor;
    bool inverted, useCurve;
    std::array<float, Brush::kCavityCurveLutSize> curve;

    bool operator==(const Key &b) const
    {
      return blur == b.blur && factor == b.factor && inverted == b.inverted &&
             useCurve == b.useCurve && curve == b.curve;
    }
  };
  struct Entry {
    Key key;
    litestl::util::Map<int, float> factors;
  };

  void reset()
  {
    entries_.clear();
    active_.resize(0, false);
    scratch_ = CavityScratch();
    geometry_ = nullptr;
    topology_ = 0;
    evaluations_ = 0;
  }

  void beginGeometry(const void *geometry, uint64_t topology, int capacity)
  {
    if (geometry_ != geometry || topology_ != topology) {
      reset();
      geometry_ = geometry;
      topology_ = topology;
    }
    active_.resize(capacity, false);
  }

  Entry &entry(const Brush &brush)
  {
    const Key key{brush.cavity_blur_steps,
                  brush.cavity_factor,
                  brush.cavity_inverted,
                  brush.cavity_use_curve,
                  brush.cavity_curve};
    for (auto &item : entries_) {
      if (item->key == key)
        return *item;
    }
    auto created = std::make_unique<Entry>();
    created->key = key;
    entries_.append(std::move(created));
    return *entries_.last();
  }

  template <class Source> void fill(Entry &entry, const Source &source, int vertex)
  {
    float *cached = entry.factors.lookup_ptr(vertex);
    if (!cached) {
      CavityParams params;
      params.enabled = true;
      params.blur_steps = entry.key.blur;
      params.factor = entry.key.factor;
      params.inverted = entry.key.inverted;
      params.use_curve = entry.key.useCurve;
      params.curve_lut = entry.key.curve.data();
      const float value =
          cavityRemap(params, cavityRawT(source, vertex, params.blur_steps, scratch_));
      entry.factors.add(vertex, value);
      evaluations_++;
      write(vertex, value);
    } else {
      write(vertex, *cached);
    }
  }

  void write(int vertex, float value)
  {
    active_.materialize(vertex);
    active_[vertex] = value;
  }

  mesh::AttrData<float> *active()
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
  size_t storedFactors() const
  {
    size_t count = 0;
    for (const auto &item : entries_)
      count += item->factors.size();
    return count;
  }

private:
  litestl::util::Vector<std::unique_ptr<Entry>> entries_;
  mesh::AttrData<float> active_{util::string(".prepared.cavity"), 0};
  CavityScratch scratch_;
  const void *geometry_ = nullptr;
  uint64_t topology_ = 0;
  size_t evaluations_ = 0;
};

/** Select prepared cache behavior only while an accepted prepared stage executes. */
struct ScopedPreparedCavity {
  bool &flag;
  bool previous;
  explicit ScopedPreparedCavity(bool &flag) : flag(flag), previous(flag)
  {
    flag = true;
  }
  ~ScopedPreparedCavity()
  {
    flag = previous;
  }
};

} // namespace sculptcore::brush
