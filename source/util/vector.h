#pragma once
#include "alloc.h"
#include "compiler_util.h"

namespace sculptcore::util {
template <typename T, int static_size = 4> class Vector {
public:
  Vector() : data_(static_storage_), capacity_(static_size)
  {
  }

  ~Vector()
  {
    release_data();
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

  T &operator[](int idx)
  {
    return data_[idx];
  }

  T &&operator[](int idx)
  {
    return std::move(data_[idx]);
  }

  size_t size() const
  {
    return size_;
  }

private:
  T &append_intern()
  {
    ensure_size(size_ + 1);
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

    for (int i = 0; i < size_; i++) {
      data_[i] = std::move(old[i]);
    }

    release_data(old);
  }

  void release_data(T *old = data_, int size = size_)
  {
    if constexpr (!is_simple(old)) {
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