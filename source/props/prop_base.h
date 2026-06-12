#pragma once

#include "litestl/binding/binding.h"
#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/compiler_util.h"
#include "litestl/util/string.h"
#include "prop_enums.h"

#include <functional>
#include <type_traits>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace litestl::binding {
template <> struct Binder<sculptcore::props::Prop> {
  static types::Enum *bind()
  {
    types::Enum *en =
        new types::Enum("sculptcore::props::Prop", sizeof(sculptcore::props::Prop));
    en->addItem("INVALID_TYPE", sculptcore::props::Prop::INVALID_TYPE);
    en->addItem("FLOAT32", sculptcore::props::Prop::FLOAT32);
    en->addItem("FLOAT64", sculptcore::props::Prop::FLOAT64);
    en->addItem("INT64", sculptcore::props::Prop::INT64);
    en->addItem("UINT64", sculptcore::props::Prop::UINT64);
    en->addItem("INT32", sculptcore::props::Prop::INT32);
    en->addItem("UINT32", sculptcore::props::Prop::UINT32);
    en->addItem("INT16", sculptcore::props::Prop::INT16);
    en->addItem("UINT16", sculptcore::props::Prop::UINT16);
    en->addItem("INT8", sculptcore::props::Prop::INT8);
    en->addItem("UINT8", sculptcore::props::Prop::UINT8);
    en->addItem("BOOL", sculptcore::props::Prop::BOOL);
    en->addItem("STRING", sculptcore::props::Prop::STRING);
    en->addItem("STATIC_STRING", sculptcore::props::Prop::STATIC_STRING);
    en->addItem("LIST", sculptcore::props::Prop::LIST);
    en->addItem("STRUCT", sculptcore::props::Prop::STRUCT);
    return en;
  }
};
} // namespace litestl::binding
using namespace litestl;
namespace sculptcore::props {

namespace detail {
using namespace litestl::binding;

// this little struct here is to avoid
// any potential weird name or virtual dispatch
// wrangling compilers might do to the more
// template heavy version below
struct PropBaseType {
  Prop type;
  util::string name, ui_name;

  static types::Struct<PropBaseType> *defineBindings()
  {
    types::Struct<PropBaseType> *st = new types::Struct<PropBaseType>(
        "sculptcore::props::detail::PropBaseType", sizeof(PropBaseType));

    BIND_STRUCT_MEMBER(st, type);
    BIND_STRUCT_MEMBER(st, name);
    BIND_STRUCT_MEMBER(st, ui_name);

    return st;
  }

  PropBaseType(Prop type, util::string name = "", util::string ui_name = "") : type(type)
  {
  }

  PropBaseType(const PropBaseType &b) : type(b.type), name(b.name), ui_name(b.ui_name)
  {
  }

  PropBaseType(PropBaseType &&b)
  {
    type = b.type;
    name = std::move(b.name);
    ui_name = std::move(b.ui_name);
  }
};

template <typename Child, typename ValueType> struct PropBase : public PropBaseType {
  int binding_offset = -1;
  PropFlag flag = PropFlag::NONE;

  std::function<ValueType *(ValueType *existing_val, void *owner)> getter;
  std::function<void(ValueType *existing_val, void *owner, ValueType &new_value)> setter;

  using value_type = ValueType;

  PropBase(Prop type_) : PropBaseType(type_)
  {
  }

  PropBase(const PropBase &b)
      : PropBaseType(b.type, b.name, b.ui_name), binding_offset(b.binding_offset),
        flag(b.flag), getter(b.getter), setter(b.setter)
  {
  }

  PropBase(PropBase &&b)
      : PropBaseType(b.type, std::move(b.name), std::move(b.ui_name)),
        binding_offset(b.binding_offset), flag(b.flag)
  {
    getter = std::move(b.getter);
    setter = std::move(b.setter);
  }

  virtual ~PropBase()
  {
  }

  virtual size_t sizeOf() const
  {
    return 0;
  }

  Child &Name(util::string s)
  {
    name = s;
    return childThis();
  }

  Child &UiName(util::string s)
  {
    ui_name = s;
    return childThis();
  }

  Child &BindingOffset(int offset)
  {
    binding_offset = offset;
    return childThis();
  }

  template <typename T> Child &Owner(T *owner_)
  {
    owner = static_cast<void *>(owner_);
    return childThis();
  }

  Child &childThis()
  {
    return *static_cast<Child *>(this);
  }

  ValueType *resolve_binding()
  {
    return static_cast<ValueType *>(pointer_offset(owner, binding_offset));
  }

  virtual const ValueType &get()
  {
    ValueType *ptr = nullptr;

    if (binding_offset != -1 && owner) {
      ptr = resolve_binding();
    } else {
      ptr = internal_value();
    }

    if (getter) {
      ptr = getter(ptr, owner);
    }

    return *ptr;
  }

  virtual const void set(ValueType &value)
  {
    ValueType *ptr = nullptr;

    if (binding_offset != -1 && owner) {
      ptr = resolve_binding();
    } else {
      ptr = internal_value();
    }

    if (setter) {
      setter(ptr, owner, value);
    } else if (ptr) {
      *ptr = value;
    }
  }

  virtual ValueType *internal_value()
  {
    return nullptr;
  }

  /* Owning struct */
  void *owner = nullptr;

private:
};

} // namespace detail
} // namespace sculptcore::props