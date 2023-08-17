#pragma once

using uchar = unsigned char;
using ushort = unsigned short;
using uint = unsigned int;

#include <cstdint>
#include <type_traits>
#include <utility>

#include "type_tags.h"

static inline void *pointer_offset(void *ptr, int n)
{
  return static_cast<void *>(static_cast<char *>(ptr) + n);
}

static inline const void *pointer_offset(const void *ptr, int n)
{
  return static_cast<const void *>(static_cast<const char *>(ptr) + n);
}

#define array_size(Array) (sizeof(Array) / sizeof(*(Array)))

#ifdef __clang__
#define ATTR_NO_OPT __attribute__((optnone))
#elif defined(_MSC_VER)
#define ATTR_NO_OPT __pragma(optimize("", off))
#elif defined(__GNUC__)
#define ATTR_NO_OPT __attribute__((optimize("O0")))
#else
#define ATTR_NO_OPT
#endif

#if defined(MSVC) && !defined(__clang__)
#define flatten_inline [[msvc::flatten]]
#define force_inline [[forceinline]]
#elif defined(__clang__)
#define flatten_inline [[gnu::flatten]]
#define force_inline [[clang::always_inline]]
#else
#define flatten_inline [[gnu::flatten]]
#define force_inline [[gnu::always_inline]]
#endif

#define FlagOperators(T)                                                                 \
  static constexpr T operator|(T a, T b)                                                 \
  {                                                                                      \
    return T(uint64_t(a) | uint64_t(b));                                                 \
  }                                                                                      \
  static constexpr T operator&(T a, T b)                                                 \
  {                                                                                      \
    return T(uint64_t(a) & uint64_t(b));                                                 \
  }                                                                                      \
  static constexpr T operator~(T a)                                                      \
  {                                                                                      \
    return T(~uint64_t(a));                                                              \
  }                                                                                      \
  static constexpr T operator^(T a, T b)                                                 \
  {                                                                                      \
    return T(uint64_t(a) ^ uint64_t(b));                                                 \
  }                                                                                      \
  static T operator^=(T a, T flag)                                                       \
  {                                                                                      \
    return T(uint64_t(a) ^ uint64_t(flag));                                              \
  }                                                                                      \
  static T operator|=(T a, T flag)                                                       \
  {                                                                                      \
    return T(uint64_t(a) | uint64_t(flag));                                              \
  }                                                                                      \
  static T operator&=(T a, T flag)                                                       \
  {                                                                                      \
    return T(uint64_t(a) & uint64_t(flag));                                              \
  }

namespace sculptcore::util {
namespace detail {
template <typename T> static constexpr bool is_simple(T *)
{
  return std::is_integral_v<T> || std::is_pointer_v<T> || std::is_floating_point_v<T> ||
         is_simple_override<T>::value;
}
} // namespace detail

template <typename T> static constexpr bool is_simple()
{
  return detail::is_simple(static_cast<std::remove_cv_t<T> *>(nullptr));
}

} // namespace sculptcore::util
