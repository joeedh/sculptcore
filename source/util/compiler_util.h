#pragma once

#include <cstdint>
#include <type_traits>

namespace sculptcore::util {
template <typename T> static constexpr bool is_simple(T t)
{
  return std::is_integral_v<T> || std::is_pointer_v<T> || std::is_floating_point_v<T>;
}
} // namespace sculptcore::util