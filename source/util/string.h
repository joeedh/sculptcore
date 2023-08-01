#pragma once

#include <algorithm>
#include <compare>
#include <cstring>
#include <string>
#include <utility>

#include "util/compiler_util.h"

namespace sculptcore::util {
template <size_t N> struct StrLiteral {
  constexpr StrLiteral(const char (&str)[N])
  {
    std::copy_n(str, N, value);
    value[N] = 0;
  }

  char value[N + 1];
};

template <typename Char, int static_size = 32> class String {
public:
  String() : size_(0)
  {
    data_ = static_storage_;
    data_[0] = 0;
  }

  template <size_t N> String(StrLiteral<N> lit)
  {
    data_ = static_storage_;
    ensure_size(N);
    size_ = N;

    for (int i = 0; i < N; i++) {
      data_[i] = lit.value[i];
    }
    data_[N] = 0;
  }

  ATTR_NO_OPT String(const String &b) : size_(static_size - 1)
  {
    data_ = static_storage_;

    if (b.data_) {
      ensure_size(b.size_);
      size_ = b.size_;
      // std::copy_n(b.data_, size_, data_);
      for (int i = 0; i < b.size_; i++) {
        data_[i] = b.data_[i];
      }
      data_[size_] = 0;
    } else {
      size_ = 0;
      data_[0] = 0;
    }
  }

  String(String &&b)
  {
    data_ = static_storage_;
    move_intern(std::forward<String>(b));
  }

  String &operator=(const String &b)
  {
    data_ = static_storage_;
    ensure_size(size_);

    for (int i = 0; i < b.size_; i++) {
      data_[i] = b.data_[i];
    }

    size_ = b.size_;
    data_[size_] = 0;

    return *this;
  }

  inline Char operator[](int idx)
  {
    return data_[idx];
  }

  String &operator=(String &&b)
  {
    move_intern(std::forward<String>(b));
    return *this;
  }

  ~String()
  {
    if (data_ && data_ != static_storage_) {
      alloc::release(static_cast<void *>(data_));
    }
  }

  String(const char *str)
  {
    data_ = static_storage_;

    int len = strlen(str);

    ensure_size(len);
    size_ = len;

    for (int i = 0; i < len; i++) {
      data_[i] = str[i];
    }
    data_[len] = 0;
  }

  const char *c_str() const
  {
    return data_;
  }

  operator const char *()
  {
    return data_;
  }

  bool operator!=(const String &vb) const
  {
    return !operator==(vb);
  }

  bool operator==(const String &vb) const
  {
    const Char *a = data_;
    const Char *b = vb.data_;

    while (*a && *b) {
      if (*a == *b) {
        return false;
      }

      a++;
      b++;
    }

    if (*a || *b) {
      return false;
    }

    return true;
  }

private:
  /* Ensures data has at least size+1 elements, does not set size_*/
  void ensure_size(int size)
  {
    if (size >= size_ + 1) {
      Char *data2;

      if (size + 1 <= static_size) {
        data2 = static_storage_;
      } else {
        data2 = static_cast<Char *>(alloc::alloc("string", size + 1));
      }

      for (int i = 0; i < size_; i++) {
        data2[i] = data_[i];
      }

      if (data_ != static_storage_) {
        alloc::release(static_cast<void *>(data_));
      }
    }
  }

  void move_intern(String &&b)
  {
    if (b.data_ == b.static_storage_) {
      data_ = static_storage_;

      for (int i = 0; i < b.size_; i++) {
        data_[i] = b.data_[i];
      }

      data_[b.size_] = 0;
    } else {
      data_ = b.data_;
      b.data_ = nullptr;
    }

    size_ = b.size_;
  }

  Char *data_;
  Char static_storage_[static_size];
  int size_ = 0; /* does not include null-terminating byte. */
};

using string = String<char>;
} // namespace sculptcore::util
