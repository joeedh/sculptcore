#pragma once

#include "prop_dynamics.h"
#include "prop_types.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"
#include "litestl/util/compiler_util.h"
#include "litestl/util/map.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"
#include "props/prop_types.h"

#include <span>

namespace sculptcore::props {

/** Declared metadata is independent of mutable authored values. */
struct ScalarDeclaration {
  util::string name;
  Prop type = Prop::INVALID_TYPE;
  bool hasDefault = false;
  double defaultValue = 0;
  bool hasRange = false;
  double rangeMin = 0;
  double rangeMax = 0;
  bool dynamic = false;

  bool compatible(const ScalarDeclaration &other) const;
};

struct ScalarRegistrationResult {
  PropError error = PropError::ERROR_NONE;
  util::string name;
};

PropError validateScalarDeclaration(const ScalarDeclaration &declaration);

struct ScalarDomain {
  double min = 0;
  double max = 0;
  double initial = 0;
};

/** Normalize typed bounds/defaults; preserve output on failure. */
PropError normalizeScalarDeclaration(const ScalarDeclaration &declaration, ScalarDomain &domain);

namespace struct_detail_2 {
/* very evil attempt to resolve circular reference with prop_coerce.h*/
template <typename T>
const T lookupValue(void *struct_def,
                    void *owner,
                    util::string &name,
                    T default_value,
                    DeviceInputCtx *ctx);
PropError
setScalarLocal(void *struct_def, void *owner, util::string name, Prop type, double value);
PropError readScalar(void *struct_def,
                     void *owner,
                     util::string name,
                     Prop type,
                     DeviceInputCtx *ctx,
                     double &value);
} // namespace struct_detail_2

struct struct_detail {
  struct StructDef;

  struct StructProp : detail::PropBase<StructProp, void *> {
    using Base = PropBase<StructProp, void *>;

    static litestl::binding::types::Struct<StructProp> *defineBindings()
    {
      using namespace litestl::binding;

      types::Struct<StructProp> *st = new types::Struct<StructProp>(
          "sculptcore::props::StructProp", sizeof(StructProp));
      st->inherit(detail::PropBaseType::defineBindings());

      BIND_STRUCT_METHOD(st, lookupFloat, MARGS("name", "default_value"));

      return st;
    }

    StructProp() : Base(Prop::STRUCT)
    {
    }

    StructProp(StructDef *def) : Base(Prop::STRUCT), struct_def(def)
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

    PropError setScalarLocal(util::string name, Prop type, double value)
    {
      return struct_detail_2::setScalarLocal(struct_def, owner, name, type, value);
    }

    PropError readScalar(util::string name, Prop type, double &value)
    {
      return struct_detail_2::readScalar(struct_def, owner, name, type, nullptr, value);
    }

    PropError
    evaluateScalar(util::string name, Prop type, DeviceInputCtx &ctx, double &value)
    {
      return struct_detail_2::readScalar(struct_def, owner, name, type, &ctx, value);
    }

    template <typename T>
    const T lookupValue(util::string name, T default_value, DeviceInputCtx *ctx = nullptr)
    {
      return sculptcore::props::struct_detail_2::lookupValue<T>(
          struct_def, owner, name, default_value, ctx);
    }

    void setFloat(util::string name, float value)
    {
      setValue<float>(name, value);
    }

    template <typename T> void setValue(util::string name, T value)
    {
      Property *prop = struct_def->lookup(name);
      if (prop == nullptr) {
        fprintf(stderr, "unknown property %s\n", name.c_str());
        return;
      }

      if constexpr (std::is_same_v<T, float>) {
        detail::PropBaseType *base = static_cast<detail::PropBaseType *>(prop);
        Float32Prop *p = static_cast<Float32Prop *>(base);
        p->set(value);
      } else if constexpr (std::is_same_v<T, bool>) {
        detail::PropBaseType *base = static_cast<detail::PropBaseType *>(prop);
        BoolProp *p = static_cast<BoolProp *>(base);
        p->set(value);
      } else {
        static_assert(false, "not implemented");
      }
    }

  private:
    void *internal_value_ = nullptr;
  };

  struct StructDef {
    using string = util::string;

    string name;

    // Inheritance parent (Stage 4): `lookup` falls back here for any key not
    // defined locally, so a child resolves inherited property values from a
    // category/scene default. Null = no parent. Set via props::resolveStruct.
    StructDef *parent = nullptr;

    StructDef()
    {
    }

    ~StructDef()
    {
      for (Property *prop : members_.values()) {
        alloc::Delete(prop);
      }
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

    MAKE_PROP(BoolProp, Bool)
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

    Property *lookupLocal(util::string name)
    {
      Property **prop = members_.lookup_ptr(name);
      return prop ? *prop : nullptr;
    }

    /** Check declarations against current state without publishing properties or metadata.
     * registerScalars repeats this check; a successful preflight does not reserve state. */
    ScalarRegistrationResult
    validateScalarDeclarations(std::span<const ScalarDeclaration> declarations);
    ScalarRegistrationResult
    registerScalars(std::span<const ScalarDeclaration> declarations);
    bool scalarDeclaration(util::string name, ScalarDeclaration &out);
    PropError validateScalarSchema(util::string name);

    Property *lookup(util::string name)
    {
      Property *prop = lookupLocal(name);
      if (prop) {
        return prop;
      }

      // Inheritance fallback: resolve from the parent default for any key the
      // child does not define locally.
      if (parent) {
        return parent->lookup(name);
      }

      return nullptr;
    }

    bool has(util::string name)
    {
      return lookup(name) != nullptr;
    }

    util::Map<util::string, Property *>::key_range keys()
    {
      return members_.keys();
    }

    util::Map<util::string, Property *>::value_range properties()
    {
      return members_.values();
    }

    template <typename Prop> StructDef &add(Prop *prop)
    {
      members_[name] = reinterpret_cast<Property *>(prop);
      return *this;
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
    util::Map<util::string, ScalarDeclaration> scalarDeclarations_;
  };
};

using StructProp = struct_detail::StructProp;
using StructDef = struct_detail::StructDef;

// Inheritance helpers (prop_inherit.cc). `resolveStruct` links `child` to
// `parent` so unset child properties resolve from the parent default.
Property *cloneProperty(Property *prop);
void resolveStruct(StructProp &parent, StructProp &child);

}; // namespace sculptcore::props
