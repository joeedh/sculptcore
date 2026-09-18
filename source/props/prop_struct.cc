#include "props/prop_struct.h"
#include "prop_base.h"
#include "props/prop_coerce.h"
#include "props/prop_types.h"
#include <type_traits>

namespace sculptcore::props::struct_detail_2 {

static bool scalarType(Prop type)
{
  return type == Prop::FLOAT32 || type == Prop::INT32 || type == Prop::BOOL;
}

PropError
setScalarLocal(void *vstruct_def, void *owner, util::string name, Prop type, double value)
{
  auto *def = static_cast<StructDef *>(vstruct_def);
  Property *prop = def ? def->lookupLocal(name) : nullptr;
  if (!prop) {
    return PropError::ERROR_NOT_EXISTS;
  }
  if (!scalarType(type) || prop->type != type) {
    return PropError::ERROR_INVALID_TYPE;
  }
  if (def->validateScalarSchema(name) != PropError::ERROR_NONE) {
    return PropError::ERROR_SCHEMA_CONFLICT;
  }
  if ((int(prop->flag) & int(PropFlag::READ_ONLY)) != 0) {
    return PropError::ERROR_READ_ONLY;
  }
  if (!std::isfinite(value)) {
    return PropError::ERROR_INVALID_VALUE;
  }
  PropError error = PropError::ERROR_INVALID_VALUE;
  numtype_dispatch(type, [&]<typename P>() {
    using T = typename P::value_type;
    auto *typed = static_cast<P *>(static_cast<detail::PropBaseType *>(prop));
    if (!std::isfinite(double(typed->min)) || !std::isfinite(double(typed->max)) ||
        typed->min > typed->max || value < double(std::numeric_limits<T>::lowest()) ||
        value > double(std::numeric_limits<T>::max()))
    {
      return;
    }
    if constexpr (std::is_integral_v<T>) {
      if (std::trunc(value) != value) {
        return;
      }
    }
    T converted = T(value);
    if (converted < typed->min || converted > typed->max) {
      return;
    }
    typed->owner = owner;
    typed->set(converted);
    error = PropError::ERROR_NONE;
  });
  return error;
}

PropError readScalar(void *vstruct_def,
                     void *owner,
                     util::string name,
                     Prop type,
                     DeviceInputCtx *ctx,
                     double &value)
{
  auto *def = static_cast<StructDef *>(vstruct_def);
  Property *prop = def ? def->lookup(name) : nullptr;
  if (!prop) {
    return PropError::ERROR_NOT_EXISTS;
  }
  if (!scalarType(type) || prop->type != type) {
    return PropError::ERROR_INVALID_TYPE;
  }
  if (def->validateScalarSchema(name) != PropError::ERROR_NONE) {
    return PropError::ERROR_SCHEMA_CONFLICT;
  }
  PropError error = PropError::ERROR_INVALID_VALUE;
  numtype_dispatch(type, [&]<typename P>() {
    using T = typename P::value_type;
    auto *typed = static_cast<P *>(static_cast<detail::PropBaseType *>(prop));
    if (def->lookupLocal(name) != prop) {
      if (typed->binding_offset != -1 || typed->getter) {
        error = PropError::ERROR_INVALID_OWNER;
        return;
      }
    } else {
      typed->owner = owner;
    }
    T base = typed->get();
    if (!std::isfinite(double(base)) || !std::isfinite(double(typed->min)) ||
        !std::isfinite(double(typed->max)) || typed->min > typed->max ||
        base < typed->min || base > typed->max)
    {
      return;
    }
    T evaluated = base;
    if (ctx) {
      if (!typed->dynamics.evaluateChecked(base, typed->min, typed->max, *ctx, evaluated)) {
        error = PropError::ERROR_INVALID_DYNAMICS;
        return;
      }
    }
    value = double(evaluated);
    error = PropError::ERROR_NONE;
  });
  return error;
}

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

  if (ctx && (prop->type == Prop::FLOAT32 || prop->type == Prop::INT32 ||
              prop->type == Prop::BOOL))
  {
    T result = default_value;
    numtype_dispatch(prop->type, [&]<typename P>() {
      auto *typed = static_cast<P *>(static_cast<detail::PropBaseType *>(prop));
      typed->dynamics.inputDeviceDatas(*ctx);
      typename P::value_type evaluated;
      if (typed->dynamics.evaluateChecked(
              typed->get(), typed->min, typed->max, evaluated))
      {
        if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
          if (double(evaluated) < double(std::numeric_limits<T>::lowest()) ||
              double(evaluated) > double(std::numeric_limits<T>::max()))
          {
            return;
          }
        }
        result = T(evaluated);
      }
    });
    return result;
  }

  T value;
  PropError error = prop_coerce<T>(prop, &value);

  if (error != PropError::ERROR_NONE) {
    return default_value;
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
