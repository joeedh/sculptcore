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
};

struct DynamicDevice {
  DeviceType type;
  util::string name;
  util::string uiName;
  util::string description;

  float curDeviceValue;
  CurveGenProp curve;
  math::BasicMix mixMode;
  float mixFactor;

  DynamicFlags flag = DynamicFlags::NONE;

  DynamicDevice() : curve()
  {
  }

  float evaluate(float f)
  {
    return curve.evaluate(f);
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
      float f2 = device.evaluate(f);
      float t = device.mixFactor;
      f = f + (f2 - f) * t;
    }
    return f;
  }
}; //
} // namespace sculptcore::props
