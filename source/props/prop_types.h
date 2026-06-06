#pragma once

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/compiler_util.h"
#include "litestl/util/string.h"

#include "prop_base.h"
#include "prop_dynamics.h"
#include "prop_enums.h"

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace litestl;
namespace sculptcore::props {
struct Dynamics;

namespace detail {

template <typename T, typename Child> struct NumBase : public PropBase<Child, T> {
  using Base = PropBase<Child, T>;
  Dynamics dynamics;

  NumBase(Prop type) : Base(type)
  {
    internal_value_ = T(double(0));
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

  Child &Default(T value)
  {
    internal_value_ = value;
    return Base::childThis();
  }

  T min, max;
  T step;

private:
  T internal_value_;
};

class Empty {};
} // namespace detail

using Property = detail::PropBase<detail::Empty, void *>;

#ifdef NUMPROP_DECL
#undeef NUMPROP_DECL
#endif

#define NUMPROP_DECL(PropName, type, proptype)                                           \
  struct PropName : public detail::NumBase<type, PropName> {                             \
    using value_type = type;                                                             \
    PropName() : detail::NumBase<type, PropName>(proptype)                               \
    {                                                                                    \
    }                                                                                    \
  }

NUMPROP_DECL(Float64Prop, double, Prop::FLOAT64);
NUMPROP_DECL(Float32Prop, float, Prop::FLOAT32);
NUMPROP_DECL(Vec2Prop, math::float2, Prop::VEC2F);
NUMPROP_DECL(Vec3Prop, math::float3, Prop::VEC3F);
NUMPROP_DECL(Vec4Prop, math::float4, Prop::VEC4F);
NUMPROP_DECL(Int64Prop, int64_t, Prop::INT64);
NUMPROP_DECL(Uint64Prop, uint64_t, Prop::UINT64);
NUMPROP_DECL(Int32Prop, int32_t, Prop::INT32);
NUMPROP_DECL(Uint32Prop, uint32_t, Prop::UINT32);
NUMPROP_DECL(Int16Prop, int16_t, Prop::INT16);
NUMPROP_DECL(Uint16Prop, uint16_t, Prop::UINT16);
NUMPROP_DECL(Int8Prop, int8_t, Prop::INT8);
NUMPROP_DECL(Uint8Prop, uint8_t, Prop::UINT8);
NUMPROP_DECL(BoolProp, bool, Prop::BOOL);

#undef NUMPROP_DECL

template <std::derived_from<detail::PropBaseType> T, typename Func>
static void getNumProp(T *prop, Func &func)
{
  detail::PropBaseType *base = static_cast<detail::PropBaseType *>(prop);
  switch (base->type) {
  case Prop::FLOAT32:
    func(static_cast<Float32Prop *>(prop));
    break;
  case Prop::FLOAT64:
    func(static_cast<Float64Prop *>(prop));
    break;
  case Prop::INT64:
    func(static_cast<Int64Prop *>(prop));
    break;
  case Prop::UINT64:
    func(static_cast<Uint64Prop *>(prop));
    break;
  case Prop::INT32:
    func(static_cast<Int32Prop *>(prop));
    break;
  case Prop::UINT32:
    func(static_cast<Uint32Prop *>(prop));
    break;
  case Prop::INT16:
    func(static_cast<Int16Prop *>(prop));
    break;
  case Prop::UINT16:
    func(static_cast<Uint16Prop *>(prop));
    break;
  case Prop::INT8:
    func(static_cast<Int8Prop *>(prop));
    break;
  case Prop::UINT8:
    func(static_cast<Uint8Prop *>(prop));
    break;
  case Prop::BOOL:
    func(static_cast<BoolProp *>(prop));
    break;
  case Prop::VEC2F:
    func(static_cast<Vec2Prop *>(prop));
    break;
  case Prop::VEC3F:
    func(static_cast<Vec3Prop *>(prop));
    break;
  case Prop::VEC4F:
    func(static_cast<Vec4Prop *>(prop));
    break;
  default:
    break;
  }
}

/* Property for util::strings */
struct StringProp : public detail::PropBase<StringProp, util::string> {
  using Base = detail::PropBase<StringProp, util::string>;

  using value_type = util::string;
  using string = util::string;

  StringProp() : Base(Prop::STRING)
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

  StaticStringProp() : Base(Prop::STATIC_STRING)
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

struct ArrayBufferProp : public detail::PropBase<ArrayBufferProp, void *> {
  using Base = detail::PropBase<ArrayBufferProp, void *>;

  Prop subtype = Prop::UINT8;

  ArrayBufferProp() : Base(Prop::ARRAYBUFFER)
  {
  }
};

} // namespace sculptcore::props
