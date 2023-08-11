#pragma once

#include "prop_types.h"
#include "prop_coerce.h"

#include "util/alloc.h"
#include "util/compiler_util.h"
#include "util/map.h"
#include "util/string.h"
#include "util/vector.h"

namespace sculptcore::props {

struct struct_detail {
  struct StructDef;

  struct StructProp : detail::PropBase<StructProp, void *> {
    using Base = PropBase<StructProp, void *>;

    StructProp() : Base(PROP_STRUCT)
    {
    }

    StructDef *struct_def = nullptr;

    void **internal_value() override
    {
      return &internal_value_;
    }

    template <typename T> const T lookupValue(util::string name, T default_value)
    {
      Property *prop = struct_def->lookup(name);

      if (!prop) {
        return default_value;
      }

      
    }
  private:
    void *internal_value_ = nullptr;
  };

  struct StructDef {
    using string = util::string;

    string name;

    StructDef()
    {
    }

    StructDef &Struct(string name, string uiname, int binding_offset = 0)
    {
      StructDef *def = alloc::New<StructDef>("StructDef");

      def->name = name;

      StructProp *prop = alloc::New<StructProp>("StructProp");
      prop->Name(name).UiName(uiname).BindingOffset(binding_offset);

      members_[name] = reinterpret_cast<Property *>(prop);
      return *def;
    }

#ifdef MAKE_PROP
#undef MAKE_PROP
#endif

#define MAKE_PROP(Prop, MethodName)                                                      \
  Prop &MethodName(string name, string uiname, int binding_offset = 0)                   \
  {                                                                                      \
    return make_prop<Prop>(name, uiname, binding_offset);                                \
  }

    MAKE_PROP(IntProp, Int32)
    MAKE_PROP(DoubleProp, Float64)
    MAKE_PROP(FloatProp, Float32)
    MAKE_PROP(Vec2Prop, Vec2f)
    MAKE_PROP(Vec3Prop, Vec3f)
    MAKE_PROP(Vec4Prop, Vec4f)
    MAKE_PROP(StringProp, String)

    StaticStringProp &
    StaticString(string name, string uiname, int size, int binding_offset = 0)
    {
      StaticStringProp *prop = alloc::New<StaticStringProp>("StaticStringProp");

      prop->setSize(size).BindingOffset(binding_offset).Name(name).UiName(uiname);
      members_[name] = reinterpret_cast<Property *>(prop);

      return *prop;
    }
#undef MAKE_PROP

    Property *lookup(util::string name)
    {
      Property **prop = members_.lookup_ptr(name);

      if (prop) {
        return *prop;
      }

      return nullptr;
    }

  private:
    template <typename Prop>
    Prop &make_prop(string name, string uiname, int binding_offset = 0)
    {
      Prop *prop = alloc::New<Prop>("make_prop");

      prop->Name(name).UiName(uiname).BindingOffset(binding_offset);
      members_[name] = reinterpret_cast<Property *>(prop);

      return *prop;
    }
    util::Map<util::string, Property *> members_;
  };
};

using StructProp = struct_detail::StructProp;
using StructDef = struct_detail::StructDef;

}; // namespace sculptcore::props
