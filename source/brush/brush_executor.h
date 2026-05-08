#pragma once

#include "binding/binding_constructor_builder.h"
#include "brush_command.h"
#include "brush_iterators.h"
#include "brushes/all.h"
#include "litestl/binding/binding.h"
#include "spatial/node.h"
#include "spatial/spatial.h"
#include <functional>
#include <span>

namespace sculptcore::brush {

using namespace litestl::util;
using namespace litestl::math;

struct CommandExecutor {
  using vertex_iter = BasicVertexIter;
  using vertex_iter_factory = std::function<vertex_iter(spatial::SpatialNode &)>;
  using brush_command = std::function<void(CommandCtx<CommandExecutor> &)>;

  Brush &brush;
  SpatialTree *tree;
  CommandCtxBase ctx;

  static litestl::binding::types::Struct<CommandExecutor> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<CommandExecutor> *st = new types::Struct<CommandExecutor>(
        "sculptcore::brush::CommandExecutor", sizeof(CommandExecutor));

    BIND_STRUCT_CONSTRUCTOR(st, "main", SpatialTree *, Brush &);
    BIND_STRUCT_MEMBER(st, brush);
    BIND_STRUCT_MEMBER(st, tree);
    BIND_STRUCT_METHOD(st, execBrush, MARGS("brushType", "nodes", "count"));

    return st;
  }

  CommandExecutor(SpatialTree *tree, Brush &brush) : tree(tree), brush(brush), ctx()
  {
  }

  auto createIterFactory()
  {
    return [this](spatial::SpatialNode &node) -> vertex_iter {
      return vertex_iter(node, *this);
    };
  }

  brush_command createCommand(SculptBrushes brushType)
  {
    switch (brushType) {
    case SculptBrushes::DRAW:
      return std::function(sculptcore::brush::command::draw<CommandExecutor>);
    default:
      printf("Unknown brush type %d\n", static_cast<int>(brushType));
      abort();
    }
  }

  void exec(brush_command &cmd, std::span<spatial::SpatialNode *> nodes)
  {
    vertex_iter_factory vertexIterFactory = createIterFactory();

    for (auto *node : nodes) {
      CommandCtx<CommandExecutor> finalCtx(ctx, *node, vertexIterFactory, brush);
      cmd(finalCtx);
    }
  }

  void execBrush(SculptBrushes brushType, spatial::SpatialNode **nodes, int count)
  {
    auto cmd = createCommand(brushType);
    exec(cmd, std::span<spatial::SpatialNode *>(nodes, count));
  }
};
} // namespace sculptcore::brush