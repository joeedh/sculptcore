#pragma once

#include "attribute_base.h"
#include "attribute_bool.h"
#include "attribute_enums.h"
#include "math/vector.h"
#include "util/alloc.h"
#include "util/string.h"
#include "util/vector.h"

#include <cmath>
#include <type_traits>

namespace sculptcore::mesh {

/* Implements all attributes other than bools. */
template <typename T> struct AttrData : AttrDataBase {
  struct AttrPage {
    T *data = nullptr;
    bool exists = false;
    T value = T();

    ~AttrPage()
    {
      if (data) {
        alloc::release(static_cast<void *>(data));
      }
    }

    AttrPage()
    {
    }

    void move_intern(AttrPage &&b)
    {
      data = b.data;
      exists = b.exists;
      value = std::move(b.value);

      b.data = nullptr;
      b.exists = false;
    }

    AttrPage &operator=(AttrPage &&b)
    {
      move_intern(std::forward<AttrPage>(b));
      return *this;
    }

    AttrPage(AttrPage &&b)
    {
      move_intern(std::forward<AttrPage>(b));
    }

    AttrPage(const AttrPage &b)
    {
      if (b.data) {
        data = static_cast<T *>(
            alloc::alloc("Copied attr page data", sizeof(T) * ATTR_PAGESIZE));

        for (int i = 0; i < ATTR_PAGESIZE; i++) {
          data[i] = b.data[i];
        }
      }

      exists = b.exists;
      value = b.value;
    }
  };

  AttrData(const string &name_) : AttrDataBase(type_to_attrtype<T>(), name_)
  {
  }

  AttrDataBase &operator=(AttrDataBase &&vb) override
  {
    AttrData<T> &&b = static_cast<AttrData<T> &&>(vb);

    pages_ = std::move(b.pages_);
    size_ = b.size_;

    return *this;
  }

  AttrData(const string &name_, int size)
      : size_(size), AttrDataBase(type_to_attrtype<T>(), name_)
  {
    int pages = int(std::ceil(double(size) / double(ATTR_PAGESIZE)));
    pages_.resize(pages);
  }

  AttrData(const string &name_, int size, T value)
      : size_(size), AttrDataBase(type_to_attrtype<T>(), name_)
  {
    int pages = int(std::ceil(double(size) / double(ATTR_PAGESIZE)));
    pages_.resize(pages);
    for (AttrPage &chunk : pages_) {
      chunk.value = value;
    }
  }

  /* Safely read from attribute, checking if a page has data allocated.*/
  T safe_get(int idx)
  {
    AttrPage &page = pages_[idx >> ATTR_PAGESHIFT];

    if (!page.exists) {
      return page.value;
    }

    return page.data[idx & ATTR_PAGEMASK];
  }

  /* Ensure page associated with idx is fully allocated. */
  void materialize(int idx)
  {
    AttrPage &page = pages_[idx >> ATTR_PAGESHIFT];

    if (!page.exists) {
      materialize_page(page);
    }
  }

  void materialize_all()
  {
    for (AttrPage &page : pages_) {
      if (!page.exists) {
        materialize_page(page);
      }
    }
  }

  T &operator[](int idx)
  {
    return pages_[idx >> ATTR_PAGESHIFT].data[idx & ATTR_PAGEMASK];
  }

  T operator[](int idx) const
  {
    return pages_[idx >> ATTR_PAGESHIFT].data[idx & ATTR_PAGEMASK];
  }

  void set(int idx, const T &value)
  {
    operator[](idx) = value;
  }

  int size()
  {
    return size_;
  }

  ~AttrData()
  {
  }

private:
  void materialize_page(AttrPage &page)
  {
    page.data =
        static_cast<T *>(alloc::alloc("AttrPage data", sizeof(T) * ATTR_PAGESIZE));

    for (int i = 0; i < ATTR_PAGESIZE; i++) {
      page.data[i] = page.value;
    }
    page.exists = true;
  }

  util::Vector<AttrPage> pages_;
  int size_ = 0;
};

struct AttrRef {
  AttrDataBase *data = nullptr;
  string name;
  AttrType type;

  AttrRef()
  {
  }

  AttrRef(AttrType type_, const char name_[]) : type(type_), name(name_)
  {
  }

  AttrRef(AttrType type_, string name_) : type(type_), name(name_)
  {
  }

  AttrRef(const AttrRef &b) : name(b.name), type(b.type)
  {
  }

  bool exists()
  {
    return data != nullptr;
  }

  ~AttrRef()
  {
  }

  AttrData<float> *Float()
  {
    return static_cast<AttrData<float> *>(data);
  }
};

namespace detail {
template <typename Lambda> void type_dispatch(AttrType type, Lambda callback)
{
  switch (type) {
  case ATTR_FLOAT:
    callback.operator()<float>();
    break;
  case ATTR_FLOAT2:
    callback.operator()<math::float2>();
    break;
  case ATTR_FLOAT3:
    callback.operator()<math::float3>();
    break;
  case ATTR_FLOAT4:
    callback.operator()<math::float4>();
    break;
  case ATTR_BOOL:
    callback.operator()<bool>();
    break;
  case ATTR_INT:
    callback.operator()<int>();
    break;
  case ATTR_INT2:
    callback.operator()<math::int2>();
    break;
  case ATTR_BYTE:
    callback.operator()<uint8_t>();
    break;
  case ATTR_SHORT:
    callback.operator()<short>();
    break;
  }
}
} // namespace detail

struct AttrGroup {
  util::Vector<AttrRef> attrs;
  PackedBoolAttrs bool_attrs;

  ~AttrGroup()
  {
    for (AttrRef &attr : attrs) {
      detail::type_dispatch(attr.type, [&]<typename T>() {
        if (attr.data) {
          AttrData<T> *data = static_cast<AttrData<T> *>(attr.data);

          if (data) {
            alloc::Delete(data);
          }
        }
      });
    }
  }

  AttrRef find_attribute(AttrType type, string name)
  {
    for (AttrRef &attr : attrs) {
      if (attr.type == type && attr.name == name) {
        return attr;
      }
    }

    /* Return empty attr reference. */
    return AttrRef(type, name);
  }

  bool has(AttrType type, string name)
  {
    for (AttrRef &attr : attrs) {
      if (attr.type == type && attr.name == name) {
        return true;
      }
    }

    return false;
  }

  ATTR_NO_OPT AttrRef &ensure(AttrType type, const string name, int element_count)
  {
    for (AttrRef &attr : attrs) {
      if (attr.type == type && attr.name == name) {
        return attr;
      }
    }

    AttrRef *ret = nullptr;

    detail::type_dispatch(type, [&]<typename T>() {
      AttrRef attr(type, name);

      if constexpr (!std::is_same_v<T, bool>) {
        AttrData<T> *data = alloc::New<AttrData<T>>("AttrData", name, element_count);
        attr.data = data;
      } else {
        PackedBoolAttrs::Offset offset = bool_attrs.add(name);

        BoolAttrView *data =
            alloc::New<BoolAttrView>("BoolAttrView", name, bool_attrs, offset);
        attr.data = static_cast<AttrDataBase *>(data);
      }

      attrs.append(attr);
      ret = &attrs[attrs.size() - 1];
    });

    if (type == ATTR_BOOL && bool_attrs.size() != element_count) {
      bool_attrs.resize(element_count);
    }

    return *ret;
  }
};

} // namespace sculptcore::mesh
