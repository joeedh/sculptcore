#pragma once

#include "prop_coerce.h"
#include "prop_types.h"

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

    StructProp(StructDef *def) : Base(PROP_STRUCT), struct_def(def)
    {
    }

    StructDef *struct_def = nullptr;

    void **internal_value() override
    {
      return &internal_value_;
    }

    float lookupFloat(util::string name, float default_value)
    {
      return lookupValue<float>(name, default_value);
    }

    template <typename T> const T lookupValue(util::string name, T default_value)
    {
      Property *prop = struct_def->lookup(name);

      if (!prop) {
        return default_value;
      }

      prop->owner = owner;

      T value;
      PropError error = prop_coerce<T>(prop, &value);

      if (error != PROP_ERROR_NONE) {
        return default_value;
      }

      return value;
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

    MAKE_PROP(Int32Prop, Int32)
    MAKE_PROP(Float64Prop, Float64)
    MAKE_PROP(Float32Prop, Float32)
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

    util::Map<util::string, Property *>::value_range properties()
    {
      return members_.values();
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
