#include "props/prop_struct.h"
#include "props/prop_coerce.h"

namespace sculptcore::props::struct_detail_2 {

template <typename T>
const T lookupValue(void *vstruct_def, void *owner, util::string &name, T default_value)
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

  return value;
}

/* Manually instantiate lookupValue templates here. */
template const float lookupValue<float>(void *, void *, util::string &, float);
template const int lookupValue<int>(void *, void *, util::string &, int);

} // namespace sculptcore::props::struct_detail_2
