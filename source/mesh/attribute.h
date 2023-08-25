#pragma once

#include "attribute_base.h"
#include "attribute_bool.h"
#include "attribute_enums.h"
#include "io/serial.h"
#include "math/vector.h"

#include "util/alloc.h"
#include "util/array.h"
#include "util/index_range.h"
#include "util/span.h"
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

    AttrPage(AttrPage &&b) noexcept
    {
      data = b.data;
      exists = b.exists;
      value = std::move(b.value);

      b.data = nullptr;
      b.exists = false;
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

    DEFAULT_MOVE_ASSIGNMENT(AttrPage)
  };

  AttrData(const string &name_) : AttrDataBase(type_to_attrtype<T>(), name_)
  {
  }

  AttrData(AttrData &&b)
  {
    pages_ = std::move(b.pages_);
    size_ = b.size_;
  }

  DEFAULT_MOVE_ASSIGNMENT(AttrData)

  AttrDataBase &operator=(AttrDataBase &&vb) noexcept override
  {
    if (this == &vb) {
      return *this;
    }

    AttrData *this2 = static_cast<AttrData *>(this);
    this2->~AttrData();

    AttrData &&b = static_cast<AttrData &&>(vb);
    new (static_cast<void *>(this)) AttrData(std::forward<AttrData>(b));

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

  void set_default(int elem)
  {
    if (util::is_simple<T>()) {
      operator[](elem) = T(0.0);
    } else {
      operator[](elem) = {};
    }
  }

  int size()
  {
    return size_;
  }

  int capacity()
  {
    return pages_.size() * ATTR_PAGESIZE;
  }

  void resize(size_t newsize, bool materialize_new_pages = true)
  {
    int page_count = newsize >> ATTR_PAGESHIFT;
    int old_page_count = pages_.size();

    if (newsize & ATTR_PAGEMASK) {
      page_count++;
    }

    while (page_count < pages_.size()) {
      pages_.pop_back();
    }

    if (page_count > pages_.size()) {
      int new_pages = int(page_count - pages_.size());
      pages_.resize(page_count);

      if (materialize_new_pages) {
        for (int i = 0; i < new_pages; i++) {
          materialize_page(pages_[old_page_count + i]);
        }
      }
    }

    size_ = newsize;
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
  AttrFlag flag;

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

  template <typename T> AttrData<T> *get_data()
  {
    return static_cast<AttrData<T> *>(data);
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
  case AttrType::NONE:
    break;
  case AttrType::FLOAT:
    callback.template operator()<float>();
    break;
  case AttrType::FLOAT2:
    callback.template operator()<math::float2>();
    break;
  case AttrType::FLOAT3:
    callback.template operator()<math::float3>();
    break;
  case AttrType::FLOAT4:
    callback.template operator()<math::float4>();
    break;
  case AttrType::BOOL:
    callback.template operator()<bool>();
    break;
  case AttrType::INT:
    callback.template operator()<int>();
    break;
  case AttrType::INT2:
    callback.template operator()<math::int2>();
    break;
  case AttrType::INT3:
    callback.template operator()<math::int3>();
    break;
  case AttrType::INT4:
    callback.template operator()<math::int4>();
    break;
  case AttrType::BYTE:
    callback.template operator()<uint8_t>();
    break;
  case AttrType::SHORT:
    callback.template operator()<short>();
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

  size_t size()
  {
    size_t ret = 0;

    for (AttrRef &attr : attrs) {
      if (attr.type == AttrType::BOOL) {
        continue;
      }

      detail::type_dispatch(attr.type, [&ret, &attr]<typename T>() {
        ret = static_cast<AttrData<T> *>(attr.data)->size();
      });
    }

    return ret;
  }

  void reorder(util::span<int> elem_map)
  {
    util::Array<int> reverse = {size()};

    for (int i : elem_map) {
      reverse[elem_map[i]] = i;
    }

    for (AttrRef &attr : attrs) {
      if (attr.type == AttrType::BOOL) {
        continue;
      }

      detail::type_dispatch(attr.type, [&]<typename T>() {
        //
        using AttrType = AttrData<T>;

        AttrType *old = static_cast<AttrType *>(attr.data);
        AttrType newattr(attr.name, old->size());

        for (int i : util::IndexRange(size())) {
          newattr[i] = std::move(old->operator[](reverse[i]));
        }

        old->~AttrData();
        *old = std::move(newattr);
      });
    }

    /* Handle bools separately. */
    PackedBoolAttrs new_bool_attrs = bool_attrs;
    new_bool_attrs.resize(size());

    int blocksize = bool_attrs.blocksize();

    for (int i : util::IndexRange(size())) {
      uint8_t *block = new_bool_attrs[i];
      uint8_t *block_old = bool_attrs[reverse[i]];

      for (int j = 0; j < blocksize; j++) {
        block[j] = block_old[j];
      }
    }

    bool_attrs.~PackedBoolAttrs();
    bool_attrs = std::move(new_bool_attrs);
  }

  void swap(int a, int b)
  {
    for (AttrRef &attr : attrs) {
      if (attr.flag & AttrFlag::TOPO) {
        continue;
      }

      if (attr.type == AttrType::BOOL) {
        BoolAttrView *view = static_cast<BoolAttrView *>(attr.data);
        bool tmp = (*view)[a];
        view->set(a, (*view)[b]);
        view->set(b, tmp);
      } else {
        detail::type_dispatch(attr.type, [&]<typename T>() {
          AttrData<T> *data = static_cast<AttrData<T> *>(attr.data);

          std::swap((*data)[a], (*data)[b]);
        });
      }
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

  AttrRef &ensure(AttrType type, const string name)
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
        AttrData<T> *data = alloc::New<AttrData<T>>("AttrData", name, capacity_);
        attr.data = data;
      } else {
        PackedBoolAttrs::Offset offset = bool_attrs.add(name);

        BoolAttrView *data =
            alloc::New<BoolAttrView>("BoolAttrView", name, &bool_attrs, offset);
        attr.data = static_cast<AttrDataBase *>(data);
      }

      attrs.append(attr);
      ret = &attrs[attrs.size() - 1];
    });

    if (type == AttrType::BOOL) {
      printf("bool\n");
    }

    if (type == AttrType::BOOL && bool_attrs.size() < capacity_) {
      bool_attrs.resize(capacity_);
    }

    return *ret;
  }

  void ensure_capacity(size_t size)
  {
    capacity_ = std::max(capacity_, size);

    if (bool_attrs.size() < size) {
      bool_attrs.resize(size);
    }

    for (AttrRef &attr : attrs) {
      if (attr.type == AttrType::BOOL) {
        continue;
      }

      detail::type_dispatch(attr.type, [&]<typename T>() {
        auto *data = attr.get_data<T>();
        if (data && data->size() < size) {
          data->resize(size);
        }
      });
    }
  }

  void set_default(int elem)
  {
    bool_attrs.set_default(elem);
    for (AttrRef &attr : attrs) {
      if (attr.type == AttrType::BOOL) {
        continue;
      }

      detail::type_dispatch(attr.type, [&]<typename T>() {
        auto *data = attr.get_data<T>();
        if (data) {
          data->set_default(elem);
        }
      });
    }
  }

private:
  size_t capacity_ = 0;
};

} // namespace sculptcore::mesh
