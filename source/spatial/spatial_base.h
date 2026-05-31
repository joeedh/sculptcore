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
  // Mesh face index of the hit triangle (tri.f). -1 if no hit; lets callers map
  // a ray hit back to a face attr (e.g. poly-group shift-to-extend sampling).
  int faceIndex = -1;

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
    BIND_STRUCT_MEMBER(st, faceIndex);

    return st;
  }
};

/* Build the 6 inward-facing frustum planes (near, far, bottom, right, top,
 * left) for a screen-rectangle selection volume, from its 8 unprojected corners
 * (object-local: corners[0..3] near plane, [4..7] far plane). Each normal is
 * flipped so the 8-corner centroid lies on its positive side, so the JS side
 * never has to agree on plane winding/order — it just passes the 8 corners (as
 * individual float3 args, since float3 arrays can't cross the WASM boundary). */
static inline void buildScreenRectPlanes(const litestl::math::float3 corners[8],
                                         litestl::math::float4 out[6])
{
  using float3 = litestl::math::float3;
  using float4 = litestl::math::float4;

  float3 cent(0.0f, 0.0f, 0.0f);
  for (int i = 0; i < 8; i++) {
    cent = cent + corners[i];
  }
  cent = cent * (1.0f / 8.0f);

  auto addPlane = [&](int idx, const float3 &a, const float3 &b, const float3 &c) {
    float3 nrm = (b - a).cross(c - a).normalized();
    float d = -nrm.dot(a);

    if (nrm.dot(cent) + d < 0.0f) {
      nrm = nrm * -1.0f;
      d = -nrm.dot(a);
    }

    out[idx] = float4(nrm[0], nrm[1], nrm[2], d);
  };

  addPlane(0, corners[0], corners[1], corners[2]); // near
  addPlane(1, corners[4], corners[5], corners[6]); // far
  addPlane(2, corners[0], corners[1], corners[5]); // bottom edge (0->1)
  addPlane(3, corners[1], corners[2], corners[6]); // right edge (1->2)
  addPlane(4, corners[2], corners[3], corners[7]); // top edge (2->3)
  addPlane(5, corners[3], corners[0], corners[4]); // left edge (3->0)
}

} // namespace sculptcore::spatial