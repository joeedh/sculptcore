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

/* Handles inheritance between parent and child. */
void resolveStruct(StructProp &parent, StructProp &child)
{
  for (string &key : parent.struct_def->keys()) {
    Property *prop = parent.struct_def->lookup(key);

    if (!child.struct_def->has(key)) {

      // child.struct_def->add(
    }
  }
}
} // namespace sculptcore::props
