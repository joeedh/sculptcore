#pragma once
#include "../brush_command.h"
#include "spatial/spatial_enums.h"

namespace sculptcore::brush::command {

template <CommandTypes TYPES>
static void drawPre(CommandCtxBase &ctx, span<SpatialNode *> nodes)
{
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
      simple->f.ensureAttr(m->f.attrs, m->f.no);

      simple->v.cpyFrom(node->data->m->v.attrs, node->unique_verts());
      simple->f.cpyFrom(node->data->m->f.attrs, node->unique_faces());
    }
  }
}

template <CommandTypes TYPES> static void draw(CommandCtx<TYPES> &ctx)
{
  using namespace sculptcore::spatial;
  bool any_moved = false;
  for (auto &vi : ctx.vertexIter(ctx.node)) {
    float s = ctx.strength(vi.co); // * vi.mask;
    if (s == 0.0f) {
      continue;
    }
    vi.co += ctx.surfaceNo * s;
    ctx.node.affected_verts.append(vi.v);
    any_moved = true;
  }

  if (any_moved) {
    ctx.node.update(Spatial_UpdateNormals | Spatial_UpdateGPU | Spatial_RegenBounds);
  }
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