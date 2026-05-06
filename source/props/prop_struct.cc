#include "props/prop_struct.h"
#include "prop_base.h"
#include "props/prop_coerce.h"
#include "props/prop_types.h"
#include <type_traits>

namespace sculptcore::props::struct_detail_2 {

template <typename T>
const T lookupValue(void *vstruct_def,
                    void *owner,
                    util::string &name,
                    T default_value,
                    DeviceInputCtx *ctx)
{
  StructDef *struct_def = static_cast<StructDef *>(vstruct_def);
  Property *prop = struct_def->lookup(name);

  if (!prop) {
    return default_value;
  }

  prop->owner = owner;

  T value;
  PropError error = prop_coerce<T>(prop, &value);

  if (error != PropError::ERROR_NONE) {
    return default_value;
  }

  if constexpr (std::is_floating_point_v<T>) {
    Dynamics *dyn = nullptr;
    detail::PropBaseType *base = static_cast<detail::PropBaseType *>(prop);

    if (prop->type == Prop::FLOAT32) {
      dyn = &static_cast<Float32Prop *>(base)->dynamics;
    } else if (prop->type == Prop::FLOAT64) {
      dyn = &static_cast<Float64Prop *>(base)->dynamics;
    }

    if (ctx && dyn) {
      // TODO: we could input device data to all properties in a brush at once at
      // the beginning of the frame.
      dyn->inputDeviceDatas(*ctx);
      value = dyn->evaluate(value);
    }
  }

  return value;
}

/* Manually instantiate lookupValue templates here. */
template const float
lookupValue<float>(void *, void *, util::string &, float, DeviceInputCtx *);
template const double
lookupValue<double>(void *, void *, util::string &, double, DeviceInputCtx *);
template const int
lookupValue<int>(void *, void *, util::string &, int, DeviceInputCtx *);
template const bool
lookupValue<bool>(void *, void *, util::string &, bool, DeviceInputCtx *);
template const short
lookupValue<short>(void *, void *, util::string &, short, DeviceInputCtx *);

} // namespace sculptcore::props::struct_detail_2
