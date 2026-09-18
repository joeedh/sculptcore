#pragma once

#include "props/prop_struct.h"
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace sculptcore::brush {

inline constexpr int kNamedUniformSlotLimit = 65536;

struct NamedUniformDescriptor {
  const char *name;
  props::Prop type;
  bool dynamic;
};

const NamedUniformDescriptor *extraNamedUniformDescriptor(props::Prop type, int slot);
const NamedUniformDescriptor *extraNamedUniformDescriptor(const util::string &name,
                                                          int &slot);

namespace named_uniform_detail {

struct StoreAccess;

/** Reject subnormal bits the caller's floating environment cannot preserve. */
inline bool supportedFloat(float value)
{
  const auto magnitude = std::bit_cast<uint32_t>(value) & 0x7fffffffu;
  if (!magnitude || magnitude >= 0x00800000u)
    return true;
  // The volatile operand prevents constant folding of this caller-mode probe.
  volatile float normal = std::numeric_limits<float>::min();
  const float half = normal * 0.5f;
  volatile float input = value;
  volatile double promoted = double(input);
  return promoted != 0.0 && std::bit_cast<uint32_t>(half) != 0;
}

template <typename T> bool convert(double value, T &result)
{
  if (!std::isfinite(value) || value < double(std::numeric_limits<T>::lowest()) ||
      value > double(std::numeric_limits<T>::max()))
  {
    return false;
  }
  if constexpr (std::is_integral_v<T>) {
    if (std::trunc(value) != value) {
      return false;
    }
  }
  result = T(value);
  if constexpr (std::is_same_v<T, float>) {
    if (!supportedFloat(result) || (std::abs(value) > 0x1p-150 && double(result) == 0.0))
      return false;
  }
  return true;
}

inline props::PropError writeRegisteredProperty(props::StructProp &properties,
                                                const NamedUniformDescriptor *descriptor,
                                                props::Prop type,
                                                double value)
{
  using props::PropError;
  if (!descriptor) {
    return PropError::ERROR_NONE;
  }
  if (descriptor->type != type) {
    return PropError::ERROR_INVALID_TYPE;
  }
  if (!descriptor->dynamic || !properties.struct_def ||
      !properties.struct_def->lookup(descriptor->name))
  {
    return PropError::ERROR_NONE;
  }
  if (!properties.struct_def->lookupLocal(descriptor->name)) {
    return PropError::ERROR_INVALID_OWNER;
  }
  return properties.setScalarLocal(descriptor->name, type, value);
}

} // namespace named_uniform_detail

/** Typed working values; capacity does not imply an initialized default. */
template <typename T> class NamedUniformStore {
public:
  size_t size() const
  {
    return values_.size();
  }
  T operator[](int slot) const
  {
    return get(slot);
  }
  T get(int slot) const
  {
    return slot >= 0 && size_t(slot) < values_.size() ? values_[slot] : T{};
  }
  bool initialized(int slot) const
  {
    return slot >= 0 && size_t(slot) < initialized_.size() && initialized_[slot];
  }

private:
  friend struct Brush;
  friend class ScopedBrushWorkingValues;
  friend struct named_uniform_detail::StoreAccess;
  litestl::util::Vector<T> values_;
  litestl::util::Vector<bool> initialized_;

  void set(int slot, T value)
  {
    while (size_t(slot) >= values_.size()) {
      values_.append(T{});
      initialized_.append(false);
    }
    values_[slot] = value;
    initialized_[slot] = true;
  }

  void ensure(int slot, T value)
  {
    if (!initialized(slot)) {
      set(slot, value);
    }
  }
};

namespace named_uniform_detail {

struct StoreAccess {
  template <typename T>
  static props::PropError write(NamedUniformStore<T> &store,
                                props::StructProp &properties,
                                const NamedUniformDescriptor *descriptor,
                                props::Prop type,
                                int slot,
                                double value)
  {
    using props::Prop;
    using props::PropError;
    constexpr Prop expected = std::is_same_v<T, float>     ? Prop::FLOAT32
                              : std::is_same_v<T, int32_t> ? Prop::INT32
                                                           : Prop::BOOL;
    if (type != expected) {
      return PropError::ERROR_INVALID_TYPE;
    }
    if (slot < 0 || slot >= kNamedUniformSlotLimit) {
      return PropError::ERROR_INVALID_VALUE;
    }
    T converted;
    if (!convert(value, converted)) {
      return PropError::ERROR_INVALID_VALUE;
    }
    auto error = writeRegisteredProperty(properties, descriptor, type, value);
    if (error == PropError::ERROR_NONE) {
      store.set(slot, converted);
    }
    return error;
  }
};

} // namespace named_uniform_detail

} // namespace sculptcore::brush
