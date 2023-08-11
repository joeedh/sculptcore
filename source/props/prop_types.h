#pragma once

#include "math/matrix.h"
#include "math/vector.h"
#include "prop_enums.h"
#include "util/alloc.h"
#include "util/compiler_util.h"
#include "util/function.h"
#include "util/map.h"
#include "util/set.h"
#include "util/string.h"

#include <functional>
#include <type_traits>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sculptcore::props {
namespace detail {

template <typename Child, typename ValueType> struct PropBase {
  PropType type;
  util::string name, ui_name;
  int binding_offset = -1;
  PropFlags flag = PROP_FLAG_NONE;

  std::function<ValueType *(ValueType *existing_val, void *owner)> getter;
  std::function<void(ValueType *existing_val, void *owner, ValueType &new_value)> setter;

  PropBase(PropType type_) : type(type_)
  {
  }

  PropBase(const PropBase &b)
      : type(b.type), binding_offset(b.binding_offset), flag(b.flag), name(b.name),
        ui_name(b.ui_name), getter(b.getter), setter(b.setter)
  {
  }

  PropBase(PropBase &&b)
      : type(b.type), type(b.type), binding_offset(b.binding_offset), flag(b.flag)
  {
    name = std::move(b.name);
    ui_name = std::move(b.ui_name);
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

template <typename T, typename Child> struct NumBase : public PropBase<Child, T> {
  using Base = PropBase<Child, T>;

  NumBase(PropType type) : Base(type)
  {
    internal_value_ = T(int(0));
  }

  NumBase(const NumBase &b) : Base(b)
  {
  }

  T *internal_value() override
  {
    return &internal_value_;
  }

  size_t sizeOf() const override
  {
    return sizeof(T);
  }

  Child &Min(T min_)
  {
    min = min_;
    return Base::childThis();
  }

  Child &Max(T max_)
  {
    max = max_;
    return Base::childThis();
  }

  Child &Step(T step_)
  {
    step = step_;
    return Base::childThis();
  }

  T min, max;
  T step;

private:
  T internal_value_;
};

class Empty {
};
} // namespace detail

using Property = detail::PropBase<detail::Empty, void *>;

struct DoubleProp : public detail::NumBase<double, DoubleProp> {
  using value_type = double;

  DoubleProp() : detail::NumBase<double, DoubleProp>(PROP_FLOAT64)
  {
  }
};

struct FloatProp : public detail::NumBase<float, FloatProp> {
  using value_type = float;

  FloatProp() : detail::NumBase<float, FloatProp>(PROP_FLOAT32)
  {
  }
};

struct Vec2Prop : public detail::NumBase<math::float2, Vec2Prop> {
  using value_type = math::float2;

  Vec2Prop() : detail::NumBase<math::float2, Vec2Prop>(PROP_VEC2F)
  {
  }
};
struct Vec3Prop : public detail::NumBase<math::float3, Vec3Prop> {
  using value_type = math::float3;

  Vec3Prop() : detail::NumBase<math::float3, Vec3Prop>(PROP_VEC3F)
  {
  }
};
struct Vec4Prop : public detail::NumBase<math::float4, Vec4Prop> {
  using value_type = math::float4;

  Vec4Prop() : detail::NumBase<math::float4, Vec4Prop>(PROP_VEC4F)
  {
  }
};

struct IntProp : public detail::NumBase<int, IntProp> {
  using value_type = int;

  IntProp() : detail::NumBase<int, IntProp>(PROP_INT32)
  {
  }
};

/* Property for util::strings */
struct StringProp : public detail::PropBase<StringProp, util::string> {
  using Base = detail::PropBase<StringProp, util::string>;

  using value_type = util::string;
  using string = util::string;

  StringProp() : Base(PROP_STRING)
  {
  }

  StringProp(const StringProp &b) : Base(b)
  {
    internal_value_ = b.internal_value_;
  }

  string *internal_value() override
  {
    return &internal_value_;
  }

private:
  string internal_value_;
};

/* Property for static char arrays. */
struct StaticStringProp : public detail::PropBase<StaticStringProp, char *> {
  using Base = detail::PropBase<StaticStringProp, char *>;
  using value_type = char *;
  static constexpr int static_size = 32;

  StaticStringProp() : Base(PROP_STATIC_STRING)
  {
  }

  StaticStringProp(const StaticStringProp &b) : Base(b), size_(b.size_)
  {
    if (b.size_ == 0) {
      internal_value_ = nullptr;
      return;
    }

    if (b.internal_value_ != b.static_storage_) {
      internal_value_ = static_cast<char *>(
          alloc::alloc("StaticStringProp.internal_value_", b.size_ + 1));
    } else {
      internal_value_ = static_storage_;
    }

    memcpy(static_cast<void *>(internal_value_),
           static_cast<void *>(b.internal_value_),
           size_);
  }

  StaticStringProp(StaticStringProp &&b) : Base(b), size_(b.size_)
  {
    if (b.internal_value_ == b.static_storage_) {
      memcpy(static_cast<void *>(static_storage_),
             static_cast<void *>(b.static_storage_),
             size_);
    } else {
      internal_value_ = b.internal_value_;
      b.internal_value_ = nullptr;
      b.size_ = 0;
    }
  }

  ~StaticStringProp()
  {
    if (internal_value_ && internal_value_ != static_storage_) {
      alloc::release(static_cast<void *>(internal_value_));
    }
  }

  value_type *internal_value()
  {
    return &internal_value_;
  }

  /* Size includes null terminator. */
  StaticStringProp &setSize(int size)
  {
    size_ = size;

    if (size <= static_size) {
      if (internal_value_ != static_storage_) {
        alloc::release(internal_value_);
      }

      internal_value_ = static_storage_;
    } else {
      if (internal_value_ != static_storage_) {
        alloc::release(internal_value_);
      }

      internal_value_ =
          static_cast<char *>(alloc::alloc("StaticStringProp.internal_value_", size));
    }

    if (size_ > 0) {
      internal_value_[0] = 0;
    }

    return *this;
  }

  /* Includes null terminator. */
  int size()
  {
    return size_;
  }

private:
  char *internal_value_ = nullptr;
  char static_storage_[static_size] = "";
  int size_ = 0; /* Includes null terminator. */
};

} // namespace sculptcore::props
