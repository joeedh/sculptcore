#pragma once
#include "../brush_command.h"
#include "spatial/spatial_enums.h"

namespace sculptcore::brush::command {

template <CommandTypes TYPES>
static void draw(CommandCtx<TYPES> &ctx)
{
  using namespace sculptcore::spatial;
  for (auto &vi : ctx.vertexIter(ctx.node)) {
    vi.co += ctx.surfaceNo * ctx.strength(vi.co); // * vi.mask;
  }

  ctx.node.update(Spatial_UpdateNormals | Spatial_UpdateGPU | Spatial_RegenBounds);
}

} // namespace sculptcore::brush::command