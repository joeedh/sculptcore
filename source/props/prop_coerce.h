#include "prop_enums.h"
#include "prop_types.h"

#include "math/matrix.h"
#include "math/vector.h"
#include "util/compiler_util.h"
#include "util/string.h"

#include <type_traits>

namespace sculptcore::props {
template <typename Callback> static void numtype_dispatch(PropType type, Callback cb)
{
  switch (type) {
  case PROP_INT32:
    cb.operator()<IntProp>();
    break;
  case PROP_FLOAT32:
    cb.operator()<FloatProp>();
    break;
  }
}

template <typename Callback> static void proptype_dispatch(PropType type, Callback cb)
{
  switch (type) {
  case PROP_INT32:
    cb.operator()<IntProp>();
    break;
  case PROP_FLOAT32:
    cb.operator()<FloatProp>();
    break;
  }
}

template <typename T> static constexpr PropType type_to_proptype()
{
  if constexpr (std::is_same_v<T, float>) {
    return PROP_FLOAT32;
  } else if constexpr (std::is_same_v<T, int>) {
    return PROP_INT32;
  } else if constexpr (std::is_same_v<T, double>) {
    return PROP_FLOAT64;
  } else if constexpr (std::is_same_v<T, char *>) {
    return PROP_STATIC_STRING;
  } else if constexpr (std::is_same_v<T, util::string>) {
    return PROP_STRING;
  }

  return PROP_INVALID_TYPE;
}

static constexpr bool is_num_type(PropType type)
{
  switch (type) {
  case PROP_BOOL:
  case PROP_INT8:
  case PROP_INT16:
  case PROP_INT32:
  case PROP_INT64:
  case PROP_FLOAT32:
  case PROP_FLOAT64:
    return true;
  }

  return false;
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

/* Does not check type of prop. */
template <typename T> T prop_value_get(Property *prop)
{
  proptype_dispatch(prop->type, [prop]<typename PropType>() {
    PropType *real_prop = reinterpret_cast<PropType *>(prop);

    return real_prop->get();
  });

  return T();
}

/* Checks type of prop. */
template <typename T> PropError prop_coerce(Property *prop, T *value_out)
{
  PropType type1 = type_to_proptype<T>();
  PropType type2 = prop->type;

  if (type1 == type2) {
    *value_out = prop_value_get<T>(prop);
    return PROP_ERROR_NONE;
  }

  if (is_num_type(type1)) {
    if (!is_num_type(type2)) {
      return PROP_ERROR_INVALID_TYPE;
    }

    *value_out = T(num_prop_to_double(prop));

    return PROP_ERROR_NONE;
  } else {
      
  }

  return PROP_ERROR_INVALID_TYPE;
}

} // namespace sculptcore::props
