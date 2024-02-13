#include "math/lut.h"
#include "math/mix.h"
#include "props/prop_curve.h"
#include "util/string.h"
#include "util/vector.h"

namespace sculptcore::props {
enum class DynamicFlags {
  NONE = 0, //
  DISABLED = 1 << 1,
  NO_INHERIT = 1 << 2
};
FlagOperators(DynamicFlags);

struct DynamicDevice {
  util::string name;
  util::string uiName;
  util::string description;

  CurveGenProp *curve;
  math::BasicMix mixMode;
  float mixFactor;

  DynamicFlags flag = DynamicFlags::NONE;
};

struct Dynamics {
  util::Vector<DynamicDevice> devices;
}; //
} // namespace sculptcore::props
