#pragma once

#include "litestl/math/mix.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"
#include "props/prop_curve.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

using namespace litestl;
namespace sculptcore::props {
inline constexpr int kDeviceCurveSampleLimit = 65536;
enum class DynamicFlags {
  NONE = 0, //
  DISABLED = 1 << 1,
  NO_INHERIT = 1 << 2
};
FlagOperators(DynamicFlags);

enum class DeviceType {
  PRESSURE = 0,
  TILTX = 1,
  TILTY = 2,
  SPEED = 3,
  ANGLE = 4,
  CURVATURE = 5,
  TWIST = 6
};

struct DeviceInput {
  DeviceType type;
  float value;
};
struct DeviceInputCtx {
  util::Vector<DeviceInput, 8> inputs;

  // Per-dab device samples â€” refilled each dab by the bridge before loadProps.
  void push(int type, float value)
  {
    for (size_t i = inputs.size(); i > 0; i--) {
      auto &input = inputs[i - 1];
      if (int(input.type) == type) {
        input.value = value;
        return;
      }
    }
    inputs.append(DeviceInput{static_cast<DeviceType>(type), value});
  }
  void clear()
  {
    inputs.clear();
  }
};

struct DynamicDevice {
  DeviceType type = DeviceType::PRESSURE;
  util::string name;
  util::string uiName;
  util::string description;

  float curDeviceValue = 0.0f;
  bool hasDeviceValue = false;
  // Zero samples means identity; a response table requires at least two samples.
  util::Vector<float> curveTable;
  // 0 table/identity, 1 constant, 2 two-step. Parameters stay double even for float
  // inputs.
  int responseKind = 0;
  double responseThreshold = 0, responseLow = 0, responseHigh = 0;
  math::BasicMix mixMode = math::BasicMix::MULTIPLY;
  float mixFactor = 1.0f;

  DynamicFlags flag = DynamicFlags::NONE;

  DynamicDevice() = default;

  bool valid(bool allowPending = false) const
  {
    if (int(type) < int(DeviceType::PRESSURE) || int(type) > int(DeviceType::SPEED) ||
        !std::isfinite(mixFactor) || mixFactor < 0.0f || mixFactor > 1.0f ||
        curveTable.size() == 1 || curveTable.size() > kDeviceCurveSampleLimit ||
        (int(flag) & ~int(DynamicFlags::DISABLED | DynamicFlags::NO_INHERIT)) != 0 ||
        (!allowPending && hasPendingCurve()))
    {
      return false;
    }
    if (responseKind < 0 || responseKind > 2 || !std::isfinite(responseThreshold) ||
        !std::isfinite(responseLow) || !std::isfinite(responseHigh) ||
        responseThreshold < 0 || responseThreshold > 1 ||
        (responseKind == 0 &&
         (responseThreshold != 0 || responseLow != 0 || responseHigh != 0)) ||
        (responseKind == 1 && (responseThreshold != 0 || responseHigh != 0)) ||
        (responseKind != 0 && curveTable.size() != 0))
    {
      return false;
    }
    switch (mixMode) {
    case math::BasicMix::MULTIPLY:
    case math::BasicMix::ADD:
    case math::BasicMix::SUBTRACT:
    case math::BasicMix::DIFFERENCE:
    case math::BasicMix::LINEAR:
      break;
    default:
      return false;
    }
    for (float sample : curveTable) {
      if (!std::isfinite(sample)) {
        return false;
      }
    }
    return true;
  }

  bool hasPendingCurve() const
  {
    return pendingSamples_.size() != 0;
  }

  bool setCurveTable(const util::Vector<float> &samples)
  {
    if (samples.size() == 1 || samples.size() > kDeviceCurveSampleLimit) {
      return false;
    }
    for (float sample : samples) {
      if (!std::isfinite(sample)) {
        return false;
      }
    }
    curveTable = samples;
    responseKind = 0;
    responseThreshold = responseLow = responseHigh = 0;
    pendingSamples_.clear();
    pendingPresent_.clear();
    pendingCount_ = 0;
    return true;
  }

  /** Resized uploads publish only when every sample has been provided. */
  bool setCurveSample(int index, int count, float value)
  {
    if (count < 2 || count > kDeviceCurveSampleLimit || index < 0 || index >= count ||
        !std::isfinite(value))
    {
      return false;
    }
    if (!hasPendingCurve() && int(curveTable.size()) == count) {
      curveTable[index] = value;
      return true;
    }
    if (int(pendingSamples_.size()) != count) {
      pendingSamples_.resize(count);
      pendingPresent_.resize(count);
      for (int i = 0; i < count; i++) {
        pendingSamples_[i] = 0;
        pendingPresent_[i] = false;
      }
      pendingCount_ = 0;
    }
    pendingSamples_[index] = value;
    if (!pendingPresent_[index]) {
      pendingPresent_[index] = true;
      pendingCount_++;
    }
    if (pendingCount_ == count) {
      curveTable = std::move(pendingSamples_);
      responseKind = 0;
      responseThreshold = responseLow = responseHigh = 0;
      pendingSamples_.clear();
      pendingPresent_.clear();
      pendingCount_ = 0;
    }
    return true;
  }

  bool sameConfiguration(const DynamicDevice &other) const
  {
    auto equal = [](const auto &a, const auto &b) {
      if (a.size() != b.size()) {
        return false;
      }
      for (size_t i = 0; i < a.size(); i++) {
        if (a[i] != b[i]) {
          return false;
        }
      }
      return true;
    };
    return responseKind == other.responseKind &&
           responseThreshold == other.responseThreshold &&
           responseLow == other.responseLow && responseHigh == other.responseHigh &&
           type == other.type && name == other.name && uiName == other.uiName &&
           description == other.description && mixMode == other.mixMode &&
           mixFactor == other.mixFactor && flag == other.flag &&
           equal(curveTable, other.curveTable) &&
           equal(pendingSamples_, other.pendingSamples_) &&
           equal(pendingPresent_, other.pendingPresent_) &&
           pendingCount_ == other.pendingCount_;
  }

private:
  util::Vector<float> pendingSamples_;
  util::Vector<bool> pendingPresent_;
  int pendingCount_ = 0;

public:
  template <typename Real = float> Real deviceFactor() const
  {
    return deviceFactor<Real>(curDeviceValue);
  }

  template <typename Real = float> Real deviceFactor(float input) const
  {
    if (responseKind == 1) {
      return Real(responseLow);
    }
    if (responseKind == 2) {
      // Do not round the threshold onto the input before choosing the branch.
      return Real(std::clamp(double(input), 0.0, 1.0) < responseThreshold ? responseLow
                                                                          : responseHigh);
    }
    int n = int(curveTable.size());
    Real x = std::clamp(Real(input), Real(0), Real(1));
    if (n < 2) {
      return x;
    }
    x *= Real(n - 1);
    int i = int(x);
    if (i >= n - 1) {
      return curveTable[n - 1];
    }
    Real t = x - Real(i);
    return Real(curveTable[i]) * (Real(1) - t) + Real(curveTable[i + 1]) * t;
  }

  template <typename Real> Real apply(Real value) const
  {
    return apply(value, curDeviceValue);
  }

  template <typename Real> Real apply(Real value, float input) const
  {
    Real factor = deviceFactor<Real>(input);
    if (!std::isfinite(factor)) {
      return value;
    }
    Real combined;
    switch (mixMode) {
    case math::BasicMix::MULTIPLY:
      combined = value * factor;
      break;
    case math::BasicMix::ADD:
      combined = value + factor;
      break;
    case math::BasicMix::SUBTRACT:
      combined = value - factor;
      break;
    case math::BasicMix::DIFFERENCE:
      combined = value > factor ? value - factor : factor - value;
      break;
    case math::BasicMix::LINEAR:
    default:
      combined = factor;
      break;
    }
    if (!std::isfinite(combined)) {
      return value;
    }
    Real result = value + (combined - value) * Real(mixFactor);
    return std::isfinite(result) ? result : value;
  }
};

struct Dynamics {
  util::Vector<DynamicDevice> devices;

  Dynamics() = default;
  Dynamics(const Dynamics &other) : devices(other.devices)
  {
    resetInputs();
  }
  Dynamics &operator=(const Dynamics &other)
  {
    devices = other.devices;
    resetInputs();
    return *this;
  }

  void resetInputs()
  {
    for (auto &layer : devices) {
      layer.hasDeviceValue = false;
      layer.curDeviceValue = 0.0f;
    }
  }

  static bool validStack(const util::Vector<DynamicDevice> &layers,
                         bool allowPending = false)
  {
    unsigned seen = 0;
    for (const auto &layer : layers) {
      if (!layer.valid(allowPending)) {
        return false;
      }
      unsigned bit = 1u << unsigned(layer.type);
      if (seen & bit) {
        return false;
      }
      seen |= bit;
    }
    return true;
  }

  bool replaceStack(const util::Vector<DynamicDevice> &layers)
  {
    if (!validStack(layers)) {
      return false;
    }
    devices = layers;
    resetInputs();
    return true;
  }

  bool configure(DeviceType type, math::BasicMix mode, float factor)
  {
    DynamicDevice candidate;
    candidate.type = type;
    candidate.mixMode = mode;
    candidate.mixFactor = factor;
    if (!candidate.valid() || !validStack(devices, true)) {
      return false;
    }
    for (auto &layer : devices) {
      if (layer.type == type) {
        layer.mixMode = mode;
        layer.mixFactor = factor;
        return true;
      }
    }
    devices.append(candidate);
    return true;
  }

  void inputDeviceDatas(const DeviceInputCtx &ctx)
  {
    resetInputs();
    for (auto &device : ctx.inputs) {
      for (auto &layer : devices) {
        if (layer.type == device.type) {
          layer.curDeviceValue = device.value;
          layer.hasDeviceValue = std::isfinite(device.value);
        }
      }
    }
  }

  template <typename T>
  bool evaluateChecked(T value, T minimum, T maximum, T &result) const
  {
    return evaluateCheckedImpl(value, minimum, maximum, nullptr, result);
  }

  // Preparation reads the current samples without changing cached inputs or
  // copying response tables. The cached compatibility path shares the math.
  template <typename T>
  bool evaluateChecked(
      T value, T minimum, T maximum, const DeviceInputCtx &ctx, T &result) const
  {
    return evaluateCheckedImpl(value, minimum, maximum, &ctx, result);
  }

private:
  template <typename T>
  bool evaluateCheckedImpl(
      T value, T minimum, T maximum, const DeviceInputCtx *ctx, T &result) const
  {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, int32_t> ||
                  std::is_same_v<T, bool>);
    using Real = std::conditional_t<std::is_same_v<T, float>, float, double>;
    if (!std::isfinite(Real(value)) || !std::isfinite(Real(minimum)) ||
        !std::isfinite(Real(maximum)) || minimum > maximum || !validStack(devices))
    {
      return false;
    }
    Real f = Real(value);
    bool applied = false;
    for (const auto &device : devices) {
      float input = device.curDeviceValue;
      bool present = device.hasDeviceValue;
      if (ctx) {
        present = false;
        for (size_t i = ctx->inputs.size(); i > 0; i--) {
          if (ctx->inputs[i - 1].type == device.type) {
            input = ctx->inputs[i - 1].value;
            present = std::isfinite(input);
            break;
          }
        }
      }
      if ((int(device.flag) & int(DynamicFlags::DISABLED)) != 0 || !present ||
          !std::isfinite(input))
      {
        continue;
      }
      f = device.apply(f, input);
      applied = true;
    }
    if (!applied && value >= minimum && value <= maximum) {
      result = value;
      return true;
    }
    f = std::clamp(f, Real(minimum), Real(maximum));
    if constexpr (std::is_same_v<T, int32_t>) {
      f = std::clamp(std::round(f),
                     double(std::numeric_limits<int32_t>::lowest()),
                     double(std::numeric_limits<int32_t>::max()));
      result = int32_t(f);
    } else if constexpr (std::is_same_v<T, bool>) {
      result = f >= 0.5;
    } else {
      result = f;
    }
    return true;
  }

public:
  float evaluate(float value) const
  {
    float result = value;
    evaluateChecked(value,
                    std::numeric_limits<float>::lowest(),
                    std::numeric_limits<float>::max(),
                    result);
    return result;
  }
}; //
} // namespace sculptcore::props
