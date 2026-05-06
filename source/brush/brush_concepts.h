#pragma once

#include "litestl/math/vector.h"
#include "litestl/util/compiler_util.h"
#include "spatial/node.h"

#include <concepts>
#include <type_traits>

namespace sculptcore::brush {
using litestl::math::float2;
using litestl::math::float3;

template <typename T>
concept VertexIter = requires(T vi, float3 co) {
  { vi.co } -> std::convertible_to<float3>;
  { vi.no } -> std::convertible_to<float3>;
  { vi.mask } -> std::convertible_to<float>;
  { vi.v } -> std::same_as<int>;
};

template <typename T, typename VI>
concept VertexIterFactory = requires(T factory) {
  VertexIter<VI>;
  std::is_invocable_v<T, VI(spatial::SpatialNode &)>;
};

template <typename T>
concept CommandTypes = requires() {
  VertexIter<typename T::vertex_iter>;
  VertexIterFactory<typename T::vertex_iter_factory, typename T::vertex_iter_factory>;
};

template <CommandTypes TYPES> struct CommandCtx;

template <typename T, typename TYPES>
concept BrushCommand = requires() {
  CommandTypes<TYPES>;
  std::is_invocable_v<T, void(CommandCtx<TYPES> &)>;
};

} // namespace sculptcore::brush