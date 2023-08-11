#pragma once

#include "prop_enums.h"
#include "util/alloc.h"
#include "util/compiler_util.h"
#include "util/function.h"
#include "util/map.h"
#include "util/set.h"
#include "util/string.h"

namespace sculptcore::props {
namespace detail {

template <typename Child> struct PropBase {
  PropBase(PropType type_) : type(type_)
  {
  }

  PropBase(const PropBase &b) : type(b.type)
  {
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

  PropType type;
  util::string name, ui_name;
  int binding_offset;
  PropFlags flag;

  Child &childThis()
  {
    return *static_cast<Child *>(this);
  }

private:
};

template <typename T, typename Child> struct NumBase : public PropBase<Child> {
  using Base = PropBase<Child>;

  NumBase(PropType type) : Base(type)
  {
  }

  NumBase(const NumBase &b) : Base(b)
  {
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
};

class Empty {
};
} // namespace detail

using Property = detail::PropBase<detail::Empty>;

struct FloatProp : public detail::NumBase<float, FloatProp> {
  FloatProp() : detail::NumBase<float, FloatProp>(PROP_FLOAT32)
  {
  }
};

} // namespace sculptcore::props
