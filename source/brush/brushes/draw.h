#pragma once
#include "../brush_command.h"
#include "spatial/spatial_enums.h"

namespace sculptcore::brush::command {

template <CommandTypes TYPES>
static void drawPre(CommandCtxBase &ctx, span<SpatialNode *> nodes)
{
  printf("drawPre: meshLog=%p isFirstTime=%d\n", ctx.meshLog, ctx.isFirstOfStep);
  if (ctx.meshLog) {
    for (auto *node : nodes) {
      if (ctx.meshLog->hasSimpleChunk(node->id)) {
        continue;
      }

      auto *simple = ctx.meshLog->getSimpleChunk(
          node->id, node->unique_verts().size(), 0, 0, node->unique_faces().size());

      auto *m = node->data->m;

      simple->v.ensureAttr(m->v.attrs, m->v.co);
      simple->v.ensureAttr(m->v.attrs, m->v.no);

      simple->v.cpyFrom(node->data->m->v.attrs, node->unique_verts());
    }
  }
}

template <CommandTypes TYPES> static void draw(CommandCtx<TYPES> &ctx)
{
  using namespace sculptcore::spatial;
  for (auto &vi : ctx.vertexIter(ctx.node)) {
    vi.co += ctx.surfaceNo * ctx.strength(vi.co); // * vi.mask;
  }

  ctx.node.update(Spatial_UpdateNormals | Spatial_UpdateGPU | Spatial_RegenBounds);
}

template <CommandTypes TYPES>
static void drawPost(CommandCtxBase &ctx, span<SpatialNode *> nodes)
{
  //
}

template <CommandTypes TYPES>
static void createDrawBrush(BrushCommandDef<CommandCtx<TYPES>> &def)
{
  def.execPre = drawPre<TYPES>;
  def.exec = draw<TYPES>;
  def.execPost = drawPost<TYPES>;
}
} // namespace sculptcore::brush::command