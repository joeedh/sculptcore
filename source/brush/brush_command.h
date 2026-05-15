#pragma once

#include "brush.h"
#include "brush_concepts.h"
#include "litestl/math/matrix.h"
#include "litestl/math/vector.h"
#include "spatial/node.h"

using namespace sculptcore::spatial;
using namespace litestl::math;
using namespace litestl::util;
using namespace sculptcore::mesh;

namespace sculptcore::brush {
using litestl::math::float2;
using litestl::math::float3;
using litestl::math::mat4;

struct CommandCtxBase {
  float2 mouse;
  float3 mousePos;
  float3 surfacePos; // pos of vertex at center of brush dot
  float3 surfaceNo;  // normal of vertex at center of brush dot
  float3 mouseDir;
  mat4 renderMatrix;

  CommandCtxBase() = default;
  CommandCtxBase(const CommandCtxBase &) = default;
  CommandCtxBase(CommandCtxBase &&) = default;
  CommandCtxBase &operator=(const CommandCtxBase &) = default;
  CommandCtxBase &operator=(CommandCtxBase &&) = default;
};

template <CommandTypes TYPES> struct CommandCtx : public CommandCtxBase {
  Brush &brush;
  spatial::SpatialNode &node;
  TYPES::vertex_iter_factory &vertexIter;

  CommandCtx(const CommandCtxBase &base,
             spatial::SpatialNode &node,
             TYPES::vertex_iter_factory &vertexIter,
             Brush &brush)
      : CommandCtxBase(base), node(node), vertexIter(vertexIter), brush(brush)
  {
  }
  CommandCtx(const CommandCtx &b)
      : CommandCtxBase(b), node(b.node), vertexIter(b.vertexIter), brush(b.brush)
  {
  }
  float strength(float3 co)
  {
    float t = 1.0f - std::min((co - surfacePos).length() / brush.radius, 1.0f);
    t = t * t * (3.0 - 2.0 * t);

    return brush.strength * t;
  }
};

} // namespace sculptcore::brush