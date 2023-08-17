#pragma once

#include "attribute.h"
#include "attribute_enums.h"
#include "math/vector.h"
#include "mesh_base.h"
#include "util/string.h"

#include <algorithm>
#include <concepts>
#include <cstdio>
#include <type_traits>

namespace sculptcore::mesh {
template <typename T, util::StrLiteral Name, AttrFlags Flag = ATTR_FLAG_NONE>
struct BuiltinAttr : protected AttrRef {
  BuiltinAttr()
  {
    type = type_to_attrtype<T>();
    name = string(Name);
    flag = Flag;
  }

  BuiltinAttr(BuiltinAttr &&b) = delete;
  BuiltinAttr &operator=(BuiltinAttr &&b) = delete;
  AttrRef &operator=(AttrRef &&b) = delete;

  bool ensure(AttrGroup &group, bool materialize = true)
  {
    bool ret = !group.has(type, name);

    AttrRef &attr = group.ensure(type, name);
    data = attr.data;

    if (materialize) {
      if constexpr (!std::is_same_v<T, bool>) {
        get_data()->materialize_all();
      }
    }

    return ret;
  }

  template <typename T2 = void>
  inline AttrData<T> *get_data() const requires (!std::same_as<T, bool>)
  {
    return static_cast<AttrData<T> *>(data);
  }

  template <typename T2 = void>
  inline BoolAttrView *get_data() const requires std::same_as<T, bool>
  {
    return static_cast<BoolAttrView *>(data);
  }

  /* Special boolean setter. */
  template <typename T2 = void>
  bool set(int idx, const T &value) requires std::same_as<T, bool>
  {
    BoolAttrView *bdata = get_data();
    printf("bview: %p\n", bdata);

    return get_data()->set(idx, value);
  }

  inline T &operator[](int idx) requires (!std::same_as<T, bool>)
  {
    return get_data()->operator[](idx);
  }

  inline const T operator[](int idx) const
  {
    return get_data()->operator[](idx);
  }
};
} // namespace sculptcore::mesh
