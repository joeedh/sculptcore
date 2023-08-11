#pragma once

#include "prop_types.h"
#include "util/alloc.h"
#include "util/compiler_util.h"
#include "util/map.h"
#include "util/string.h"
#include "util/vector.h"

namespace sculptcore::props {
struct StructDef;

struct StructProp : detail::PropBase<StructProp> {
  using Base = PropBase<StructProp>;

  StructProp() : Base(PROP_STRUCT)
  {
  }

  StructDef *struct_def = nullptr;
};

struct StructDef {
  using string = util::string;

  string name;

  StructDef()
  {
  }

  StructDef &Struct(string name, string uiname, int binding_offset=0)
  {
    StructDef *def = alloc::New<StructDef>("StructDef");

    def->name = name;

    StructProp *prop = alloc::New<StructProp>("StructProp");
    prop->Name(name).UiName(uiname).BindingOffset(binding_offset);

    members_[name] = reinterpret_cast<Property*>(prop);
    return *def;
  }

  FloatProp &Float32(string name, string uiname, int binding_offset = 0)
  {
    FloatProp *prop = alloc::New<FloatProp>("FloatProp");

    prop->Name(name).UiName(uiname).BindingOffset(binding_offset);
    return *prop;
  }

private:
  util::Map<util::string, Property *> members_;
};

}; // namespace sculptcore::props
