#pragma once

#include "props/prop_coerce.h"
#include "props/prop_struct.h"

using namespace litestl::util;

namespace sculptcore::props {
Property *cloneProperty(Property *prop)
{
  Property *result = nullptr;

  proptype_dispatch(prop->type, [&]<typename PropType>() {
    PropType *existing = reinterpret_cast<PropType *>(prop);
    PropType *clone = alloc::New<PropType>("cloned property", *existing);

    result = reinterpret_cast<Property *>(clone);
  });

  return result;
}

/* Link `child` to `parent` so StructDef::lookup falls back to the parent's
 * properties for any key the child does not define locally. This is the
 * "category default → instance" layer; deeper chains (scene/user) are formed by
 * linking parents to their own parents. */
void resolveStruct(StructProp &parent, StructProp &child)
{
  if (child.struct_def && parent.struct_def) {
    child.struct_def->parent = parent.struct_def;
  }
}
} // namespace sculptcore::props
