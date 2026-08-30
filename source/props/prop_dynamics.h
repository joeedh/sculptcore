#pragma once

#include "litestl/math/mix.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"
#include "props/prop_curve.h"

using namespace litestl;
namespace sculptcore::props {
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

  // Per-dab device samples — refilled each dab by the bridge before loadProps.
  void push(int type, float value)
  {
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
  // Baked response curve: device value (0..1) → factor, sampled from the TS
  // channel's Curve1D. <2 entries means identity (factor == device value).
  util::Vector<float> curveTable;
  math::BasicMix mixMode = math::BasicMix::MULTIPLY;
  float mixFactor = 1.0f;

  DynamicFlags flag = DynamicFlags::NONE;

  DynamicDevice() = default;

  // Map the current device value through the baked curve to a response factor.
  float deviceFactor() const
  {
    int n = int(curveTable.size());
    if (n < 2) {
      return curDeviceValue; // identity
    }
    float x = curDeviceValue;
    x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    x *= float(n - 1);
    int i = int(x);
    if (i >= n - 1) {
      return curveTable[n - 1];
    }
    float t = x - float(i);
    return curveTable[i] * (1.0f - t) + curveTable[i + 1] * t;
  }

  // Combine a property value with the curved device factor per mixMode, then
  // blend toward that by mixFactor. MULTIPLY at mixFactor=1 reproduces the TS
  // `value * curve(pressure)` behavior the bridge replaces.
  float apply(float value) const
  {
    float factor = deviceFactor();
    float combined;
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
    return value + (combined - value) * mixFactor;
  }
};

struct Dynamics {
  util::Vector<DynamicDevice> devices;

  void inputDeviceDatas(const DeviceInputCtx &ctx)
  {
    for (auto &device : ctx.inputs) {
      for (auto &layer : devices) {
        if (layer.type == device.type) {
          layer.curDeviceValue = device.value;
        }
      }
    }
  }

  float evaluate(float value)
  {
    float f = value;
    for (auto &device : devices) {
      if (((int)device.flag & (int)DynamicFlags::DISABLED) != 0) {
        continue;
      }
      f = device.apply(f);
    }
    return f;
  }
}; //
} // namespace sculptcore::props
