#include "prop_enums.h"
#include "prop_struct.h"
#include "prop_types.h"

#include "litestl/math/matrix.h"
#include "litestl/math/vector.h"
#include "litestl/util/compiler_util.h"
#include "litestl/util/string.h"

#include <type_traits>

using namespace litestl;

namespace sculptcore::props {
template <typename Callback> static void numtype_dispatch(Prop type, Callback cb)
{
  switch (type) {
  case Prop::INT32:
    cb.template operator()<Int32Prop>();
    break;
  case Prop::FLOAT32:
    cb.template operator()<Float32Prop>();
    break;
  }
}

template <typename Callback> static void proptype_dispatch(Prop type, Callback cb)
{
  switch (type) {
  case Prop::INT32:
    cb.template operator()<Int32Prop>();
    break;
  case Prop::FLOAT32:
    cb.template operator()<Float32Prop>();
    break;
  case Prop::STRUCT:
    cb.template operator()<StructProp>();
    break;
  }
}

template <typename T> static constexpr Prop type_to_proptype()
{
  if constexpr (std::is_same_v<T, float>) {
    return Prop::FLOAT32;
  } else if constexpr (std::is_same_v<T, int>) {
    return Prop::INT32;
  } else if constexpr (std::is_same_v<T, double>) {
    return Prop::FLOAT64;
  } else if constexpr (std::is_same_v<T, char *>) {
    return Prop::STATIC_STRING;
  } else if constexpr (std::is_same_v<T, util::string>) {
    return Prop::STRING;
  }

  return Prop::INVALID_TYPE;
}

static constexpr bool is_num_type(Prop type)
{
  switch (type) {
  case Prop::BOOL:
  case Prop::INT8:
  case Prop::INT16:
  case Prop::INT32:
  case Prop::INT64:
  case Prop::FLOAT32:
  case Prop::FLOAT64:
    return true;
  default:
    return false;
  }
}

double num_prop_to_double(Property *prop)
{
  double retval = 0;

  numtype_dispatch(prop->type, [prop, &retval]<typename T>() {
    T *real_prop = reinterpret_cast<T *>(prop);

    retval = double(real_prop->get());
  });

  return retval;
}

void num_prop_from_double(Property *prop, double d)
{
  numtype_dispatch(prop->type, [&]<typename PropType>() {
    PropType *real_prop = reinterpret_cast<PropType *>(prop);

    typename PropType::value_type value = typename PropType::value_type(d);
    real_prop->set(value);
  });
}

#define IS_NUM_TYPE(Type)                                                                \
  ((std::is_integral_v<Type> || std::is_floating_point_v<Type>)&&!std::is_pointer_v<Type>)

/* Does not check type of prop. */
template <typename T> T prop_value_get(Property *prop)
{
  T result = {};

  proptype_dispatch(prop->type, [prop, &result]<typename PropType>() {
    PropType *real_prop = reinterpret_cast<PropType *>(prop);

    if constexpr (std::is_same_v<typename PropType::value_type, T>) {
      result = real_prop->get();
    }
  });

  return result;
}

/* Checks type of prop. */
template <typename Type> PropError prop_coerce(Property *prop, Type *value_out)
{
  Prop type1 = type_to_proptype<Type>();
  Prop type2 = prop->type;

  if (type1 == type2) {
    *value_out = prop_value_get<Type>(prop);
    return PropError::ERROR_NONE;
  }

  if (is_num_type(type1)) {
    if (!is_num_type(type2)) {
      return PropError::ERROR_INVALID_TYPE;
    }

    *value_out = Type(num_prop_to_double(prop));

    return PropError::ERROR_NONE;
  } else {
    PropError error = PropError::ERROR_INVALID_TYPE;

    proptype_dispatch(prop->type, [&]<typename PropType>() {
      PropType *real_prop = reinterpret_cast<PropType *>(prop);

      if constexpr (!std::is_same_v<decltype(real_prop->get()), Type>) {
        error = PropError::ERROR_INVALID_TYPE;
      } else {
        *value_out = real_prop->get();
        error = PropError::ERROR_NONE;
      }
    });

    return error;
  }

  return PropError::ERROR_INVALID_TYPE;
}

} // namespace sculptcore::props
