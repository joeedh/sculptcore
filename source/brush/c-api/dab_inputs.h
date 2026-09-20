#pragma once

#include "brush/brush.h"
#include <climits>
#include <cmath>

namespace sculptcore::brush {

/** Reflect the prepared image's geometric frame, then restore the primary frame. */
class DabFrameOverlay {
  Brush &brush_;
  float3 from_, to_, view_;

public:
  explicit DabFrameOverlay(Brush &brush, const float *signs)
      : brush_(brush), from_(brush.grabFrom), to_(brush.grabTo), view_(brush.viewDir)
  {
    if (signs)
      for (int axis = 0; axis < 3; axis++) {
        brush_.grabFrom[axis] *= signs[axis];
        brush_.grabTo[axis] *= signs[axis];
        brush_.viewDir[axis] *= signs[axis];
      }
  }
  ~DabFrameOverlay()
  {
    brush_.grabFrom = from_;
    brush_.grabTo = to_;
    brush_.viewDir = view_;
  }
};

/** Declare one symmetry image to the executor for the dab about to run
 * (`setImageSign`: the primary is the identity, a mirror carries its signs),
 * and put the executor back on the primary afterwards so a host that never
 * calls setImageSign itself is not left on a mirror. Both executors expose
 * the same setter. */
template <class Executor> class DabImageScope {
  Executor &exec_;

public:
  DabImageScope(Executor &exec, const float *signs) : exec_(exec)
  {
    if (signs)
      exec_.setImageSign(signs[0], signs[1], signs[2], true);
    else
      exec_.setImageSign(1.0f, 1.0f, 1.0f, false);
  }
  ~DabImageScope()
  {
    exec_.setImageSign(1.0f, 1.0f, 1.0f, false);
  }
};

inline int reportStrokeInputFailure(const props::ScalarRegistrationResult &result)
{
  fprintf(stderr,
          "SculptCore: rejected %s (property status %d)\n",
          result.name.c_str(),
          int(result.error));
  return -1;
}

/** Restore the failing dab's temporary inputs; earlier accepted dabs stay committed. */
class DabInputOverlay {
  Brush &brush_;
  float radius_, strength_, authoredRadius_ = 0, authoredStrength_ = 0;
  bool invert_, authoredInvert_ = false, committed_ = false;
  props::Float32Prop *radiusProp_ = nullptr, *strengthProp_ = nullptr;
  props::BoolProp *invertProp_ = nullptr;
  props::DeviceInputCtx inputs_;

  template <typename P> P *property(const char *name, props::Prop type)
  {
    if (!brush_.props.struct_def)
      return nullptr;
    auto *p = brush_.props.struct_def->lookupLocal(name);
    if (!p || p->name != util::string(name) || p->type != type)
      return nullptr;
    auto *typed = static_cast<P *>(static_cast<props::detail::PropBaseType *>(p));
    if (typed->getter || typed->setter ||
        (typed->binding_offset != -1 && (typed->owner || brush_.props.owner)) ||
        (typed->flag & props::PropFlag::READ_ONLY) != props::PropFlag::NONE)
      return nullptr;
    return typed;
  }

public:
  explicit DabInputOverlay(Brush &brush)
      : brush_(brush), radius_(brush.radius), strength_(brush.strength),
        invert_(brush.invert), inputs_(brush.deviceInputCtx)
  {
    radiusProp_ = property<props::Float32Prop>("radius", props::Prop::FLOAT32);
    strengthProp_ = property<props::Float32Prop>("strength", props::Prop::FLOAT32);
    invertProp_ = property<props::BoolProp>("invert", props::Prop::BOOL);
    if (valid()) {
      authoredRadius_ = *radiusProp_->internal_value();
      authoredStrength_ = *strengthProp_->internal_value();
      authoredInvert_ = *invertProp_->internal_value();
    }
  }
  bool valid() const
  {
    return radiusProp_ && strengthProp_ && invertProp_;
  }
  void commit()
  {
    committed_ = true;
  }
  ~DabInputOverlay()
  {
    if (!valid() || committed_)
      return;
    brush_.radius = radius_;
    brush_.strength = strength_;
    brush_.invert = invert_;
    *radiusProp_->internal_value() = authoredRadius_;
    *strengthProp_->internal_value() = authoredStrength_;
    *invertProp_->internal_value() = authoredInvert_;
    brush_.deviceInputCtx = std::move(inputs_);
  }
};

/** Inputs rows contain pressure, tilt X/Y, speed, presence mask and invert.
 * All arrays are borrowed for the call. Each count describes complete rows. */
inline bool validDabInputs(int n,
                           const float *dabs,
                           float strength,
                           const float *inputs,
                           const float *signs,
                           int mirrors)
{
  if (n < 0 || n > INT_MAX / 7 || mirrors < 0 || mirrors > INT_MAX / 3 ||
      (n && (!dabs || !inputs)) || (mirrors && !signs) || !std::isfinite(strength))
    return false;
  for (int i = 0; i < n; i++) {
    const float *d = dabs + i * 7, *s = inputs + i * 6;
    for (int j = 0; j < 7; j++)
      if (!std::isfinite(d[j]))
        return false;
    if (d[6] < 0 || (d[6] > 0 && !std::isfinite(1.0f / d[6])))
      return false;
    for (int j = 0; j < 4; j++)
      if (!std::isfinite(s[j]) || s[j] < 0 || s[j] > 1)
        return false;
    if (!std::isfinite(s[4]) || s[4] < 0 || s[4] > 15 || std::floor(s[4]) != s[4] ||
        (s[5] != 0 && s[5] != 1))
      return false;
  }
  for (int i = 0; i < mirrors * 3; i++)
    if (signs[i] != -1 && signs[i] != 1)
      return false;
  return true;
}

/** Replace the host-owned dab values without copying evaluated working caches. */
inline void setDabInputs(Brush &brush, float radius, float strength, const float *inputs)
{
  brush.radius = radius;
  brush.strength = strength;
  brush.invert = inputs[5] != 0;
  brush.props.setValue<float>("radius", radius);
  brush.props.setValue<float>("strength", strength);
  brush.props.setValue<bool>("invert", brush.invert);
  brush.clearDeviceInputs();
  for (int i = 0; i < 4; i++)
    if (int(inputs[4]) & (1 << i))
      brush.pushDeviceInput(i, inputs[i]);
}
} // namespace sculptcore::brush
