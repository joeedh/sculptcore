#pragma once

#include "brush.h"
#include "brush_concepts.h"
#include "meshlog/meshlog.h"
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

  bool isFirstOfStep = false;
  
  meshlog::MeshLog *meshLog = nullptr;

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
    float t = 1.0f - std::min(brush.falloffDist(co - surfacePos), 1.0f);
    return brush.strength * brush.falloffEval(t) * brush.radius * 0.1f;
  }

  // Sample the brush texture at world point `co` with surface normal `no`,
  // mapping to UV per `brush.coord_space`. `no` is currently unused for the
  // matrix-driven modes but is part of the DSL signature so a future
  // tangent-space projection can use it without a kernel rewrite. Returns
  // 1.0 with no texture bound (kernels multiply by this unconditionally).
  float sampleBrushTex(float3 co, float3 no)
  {
    (void)no;
    float2 uv;
    switch (brush.coord_space) {
    case TexCoordSpace::Global:
      uv = float2{co[0], co[1]};
      break;
    case TexCoordSpace::ViewPlane: {
      float3 p = renderMatrix * co;
      uv = float2{p[0], p[1]};
      break;
    }
    case TexCoordSpace::ViewRepeat: {
      float3 p = renderMatrix * co;
      uv = float2{p[0] * brush.tex_repeat, p[1] * brush.tex_repeat};
      break;
    }
    }
    return brush.sampleTexBilinear(uv);
  }
};

enum _BrushFlags { None = 0, Serial = 1 << 0 };
MAKE_FLAGS_CLASS(BrushFlags, _BrushFlags, int);

template <typename CTX> struct BrushCommandDef {
  // Optional `host` stage from the DSL — runs once per dab on CPU before
  // any per-node work. Used to mutate ctx state / populate query buffers
  // that the per-vertex stage then reads. Never lowered to GPU backends.
  std::function<void(CommandCtxBase &, Brush &)> execHost;
  std::function<void(CommandCtxBase &, std::span<SpatialNode *>)> execPre;
  std::function<void(CTX &)> exec;
  std::function<void(CommandCtxBase &, std::span<SpatialNode *>)> execPost;
  BrushFlags flags = BrushFlags::None;
};

} // namespace sculptcore::brush
