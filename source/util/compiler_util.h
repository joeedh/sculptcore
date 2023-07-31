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

namespace sculptcore::util {
template <typename T> static constexpr bool is_simple()
{
  return std::is_integral_v<T> || std::is_pointer_v<T> || std::is_floating_point_v<T>;
}
} // namespace sculptcore::util