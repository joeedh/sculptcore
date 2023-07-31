#pragma once
#include "alloc.h"
#include "compiler_util.h"
#include <cstdio>
#include <cstring>
#include <utility>

namespace sculptcore::util {
template <typename T, int static_size = 4> class Vector {
public:
  using value_type = T;

  struct iterator {
    iterator(Vector &vec, int i) : i_(i), vec_(vec)
    {
    }

    iterator(const iterator &b) : i_(b.i_), vec_(b.vec_)
    {
    }

    bool operator==(const iterator &b)
    {
      return b.i_ == i_;
    }
    bool operator!=(const iterator &b)
    {
      return b.i_ != i_;
    }

    T &operator*()
    {
      return vec_[i_];
    }

    iterator &operator++()
    {
      i_++;
      return *this;
    }

  private:
    int i_;
    Vector &vec_;
  };

  Vector() : capacity_(static_size)
  {
    data_ = static_storage_;
  }

  ~Vector()
  {
    release_data(data_, size_);
  }

  Vector(const Vector &b)
  {
    size_ = b.size_;
    capacity_ = b.capacity_;

    if (size_ > static_size) {
      data_ = static_cast<T *>(alloc::alloc(sizeof(T) * b.capacity_));
    } else {
      data_ = static_storage_;
    }

    for (int i = 0; i < size_; i++) {
      data_[i] = b.data_[i];
    }
  }

  Vector(const Vector &&b)
  {
    size_ = b.size_;
    capacity_ = b.capacity_;

    if (size_ <= static_size) {
      data_ = static_storage_;

      for (int i = 0; i < size_; i++) {
        data_[i] = std::move(b.data_[i]);
      }
    } else {
      data_ = b.data_;
      b.data_ = nullptr;
    }
  }

  iterator begin()
  {
    return iterator(*this, 0);
  }

  iterator end()
  {
    return iterator(*this, size_);
  }

  bool contains(const T &value) const
  {
    return index_of(value) != -1;
  }

  bool remove(const T &value, bool swap_end_only = false)
  {
    return remove_intern<const T &>(value, swap_end_only);
  }

  template <typename TArg> bool remove_intern(TArg value, bool swap_end_only = false)
  {
    int i = index_of(value);
    if (i < 0) {
      fprintf(stderr, "Item not in list\n");
      return false;
    }

    if constexpr (!is_simple<T>()) {
      data_[i].~T();
    }

    if (swap_end_only) {
      data_[i] = std::move(data_[size_ - 1]);
    } else {
      while (i < size_ - 1) {
        data_[i] = std::move(data_[i + 1]);
        i++;
      }

      /* Run end's destructor even though we moved its contents. */
      if constexpr (!is_simple<T>()) {
        data_[i].~T();
      }
    }

    size_--;
    return true;
  }

  int index_of(const T &value) const
  {
    for (int i = 0; i < size_; i++) {
      if (data_[i] == value) {
        return i;
      }
    }

    return -1;
  }

  bool append_once(T &&value)
  {
    if (index_of(value) == -1) {
      append(value);
      return true;
    }

    return false;
  }

  bool append_once(T &value)
  {
    if (index_of(value) == -1) {
      append(value);
      return true;
    }

    return false;
  }

  void append(T &value)
  {
    append_intern() = value;
  }

  void append(T &&value)
  {
    append_intern() = value;
  }

  template <bool construct = true> void resize(size_t newsize)
  {
    size_t remain = 0;
    if (newsize > size_) {
      remain = newsize - size_;
    }

    ensure_size(newsize);
    size_ = newsize;

    /* Construct new elements. */
    if constexpr (construct) {
      for (int i = 0; i < remain; i++) {
        if constexpr (!is_simple(data_)) {
          operator new (&data_[size_ - i - 1])();
        } else {
          data_[size_ - i - 1] = T(0);
        }
      }
    }
  }

  T &operator[](int idx)
  {
    return data_[idx];
  }

  size_t size() const
  {
    return size_;
  }

private:
  T &append_intern()
  {
    ensure_size(size_ + 1);
    size_++;
    return data_[size_ - 1];
  }

  void ensure_size(size_t newsize)
  {
    if (newsize < capacity_) {
      return;
    }

    size_t new_capacity = (newsize + 1);
    new_capacity += new_capacity >> 1;
    capacity_ = new_capacity;

    T *old = data_;
    data_ = static_cast<T *>(
        static_cast<void *>(alloc::alloc("Vector alloc", new_capacity * sizeof(T))));

    if constexpr (is_simple<T>()) {
      memcpy(static_cast<void *>(data_), static_cast<void *>(old), sizeof(T) * size_);
    } else {
      for (int i = 0; i < size_; i++) {
        data_[i] = std::move(old[i]);
      }
    }

    release_data(old, size_);
  }

  void release_data(T *old, int size)
  {
    if constexpr (!is_simple<T>()) {
      /* Run destructors. */
      for (int i = 0; i < size; i++) {
        old[i].~T();
      }
    }

    if (old != static_storage_) {
      alloc::release(static_cast<void *>(old));
    }
  }

  T *data_ = nullptr;
  T static_storage_[static_size];

  size_t capacity_ = 0;
  size_t size_ = 0;
};

} // namespace sculptcore::util