#pragma once

#include <cstdint>
#include <type_traits>

#ifdef __clang__
#define ATTR_NO_OPT __attribute__((optnone))
#elif defined(_MSC_VER)
#define ATTR_NO_OPT __pragma(optimize("", off))
#elif defined(__GNUC__)
#define ATTR_NO_OPT __attribute__((optimize("O0")))
#else
#define ATTR_NO_OPT
#endif

#define flatten_inline [[msvc::flatten]]
#define force_inline [[forceinline]]

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
template <typename T> static constexpr bool is_simple()
{
  return std::is_integral_v<T> || std::is_pointer_v<T> || std::is_floating_point_v<T>;
}
} // namespace sculptcore::util
