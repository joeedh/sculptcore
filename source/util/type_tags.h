#pragma once

#include <type_traits>

namespace sculptcore::util::detail {
template <typename T> struct is_simple_override : std::false_type {
};

#define force_type_is_simple(Type)                                                       \
  template <>                                                                            \
  struct sculptcore::util::detail::is_simple_override<Type> : public std::true_type {    \
  }
} // namespace sculptcore::util::detail
