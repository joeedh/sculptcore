#pragma once

#include "attribute.h"
#include "attribute_enums.h"
#include "litestl/binding/binding.h"
#include "litestl/math/math_bindings.h"
#include "litestl/math/vector.h"
#include "litestl/util/string.h"
#include "mesh_base.h"

#include <algorithm>
#include <concepts>
#include <cstdio>
#include <type_traits>

using namespace litestl;

namespace sculptcore::mesh {
template <typename T,
          util::StrLiteral Name,
          AttrFlag Flag = AttrFlag::NONE,
          AttrUse Use = AttrUse::NONE>
struct BuiltinAttr : public AttrRef {
  static binding::types::Struct<BuiltinAttr> *defineBindings()
  {
    using binding::types::Struct;
    Struct<BuiltinAttr> *st = new Struct<BuiltinAttr>(
        string("sculptcore::mesh::BuiltinAttr"), sizeof(BuiltinAttr));
    st->addTemplateParam(binding::Bind<T>(), "Type");
    st->addTemplateParam(new binding::types::StrLitType(string(Name), "name"), "Name");

    return st;
  }

  BuiltinAttr()
  {
    type = type_to_attrtype<T>();
    name = string(Name);
    flag = Flag;
    use = Use;
  }

  BuiltinAttr(BuiltinAttr &&b) = delete;
  BuiltinAttr &operator=(BuiltinAttr &&b) = delete;
  AttrRef &operator=(AttrRef &&b) = delete;

  bool ensure(AttrGroup &group, bool materialize = true)
  {
    if (data != nullptr) {
      return false;
    }

    bool ret = !group.has(type, name);

    AttrRef &attr = group.ensure(type, name);

    /* AttrGroup::ensure() builds the stored AttrRef via AttrRef(type, name),
     * which does not carry the builtin's AttrFlag/AttrUse. Stamp them here so
     * group entries report TOPO/TEMP/SELECT/etc. correctly (e.g. AttrGroup::swap's
     * TOPO guard, serialization, select-category discovery). */
    attr.flag = flag;
    attr.use = Use;
    data = attr.data;

    if (materialize) {
      if constexpr (!std::is_same_v<T, bool>) {
        get_data()->materialize_all();
      }
    }

    return ret;
  }

  template <typename T2 = void>
  inline AttrData<T> *get_data() const
    requires(!std::same_as<T, bool>)
  {
    return static_cast<AttrData<T> *>(data);
  }

  template <typename T2 = void>
  inline BoolAttrView *get_data() const
    requires std::same_as<T, bool>
  {
    return static_cast<BoolAttrView *>(data);
  }

  /* Special boolean setter. */
  template <typename T2 = void>
  bool set(int idx, const T &value)
    requires std::same_as<T, bool>
  {
    return get_data()->set(idx, value);
  }

  inline T &operator[](int idx)
    requires(!std::same_as<T, bool>)
  {
    return get_data()->operator[](idx);
  }

  inline const T operator[](int idx) const
  {
    return get_data()->operator[](idx);
  }
};
} // namespace sculptcore::mesh
