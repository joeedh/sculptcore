#pragma once

#include "litestl/binding/binding.h"
#include "litestl/math/math_bindings.h"
#include "litestl/math/vector.h"

namespace sculptcore::spatial {

struct CastRayIsect {
  using float3 = litestl::math::float3;
  using float2 = litestl::math::float2;

  float3 p;
  float3 normal;
  float t;
  float2 uv;
  int triIndex;
  int nodeIndex;

  CastRayIsect() = default;
  CastRayIsect(const CastRayIsect &b) = default;
  CastRayIsect &operator=(const CastRayIsect &b) = default;

  static litestl::binding::types::Struct<CastRayIsect> *defineBindings()
  {
    using namespace litestl::binding;
    using namespace litestl;
    types::Struct<CastRayIsect> *st = new types::Struct<CastRayIsect>(
        "sculptcore::spatial::CastRayIsect", sizeof(CastRayIsect));

    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);

    BIND_STRUCT_MEMBER(st, p);
    BIND_STRUCT_MEMBER(st, normal);
    BIND_STRUCT_MEMBER(st, t);
    BIND_STRUCT_MEMBER(st, uv);
    BIND_STRUCT_MEMBER(st, triIndex);
    BIND_STRUCT_MEMBER(st, nodeIndex);

    return st;
  }
};

} // namespace sculptcore::spatial