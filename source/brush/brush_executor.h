#pragma once

#include "binding/binding_constructor_builder.h"
#include "brush_command.h"
#include "brush_iterators.h"
#include "brushes/all.h"
#include "litestl/binding/binding.h"
#include "litestl/util/task.h"
#include "meshlog/meshlog.h"
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
  using brush_command = BrushCommandDef<CommandCtx<CommandExecutor>>;

  Brush *brush;
  SpatialTree *tree;
  CommandCtxBase ctx;
  bool isFirstOfStep = false;
  meshlog::MeshLog *meshLog = nullptr;

  static litestl::binding::types::Struct<CommandExecutor> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<CommandExecutor> *st = new types::Struct<CommandExecutor>(
        "sculptcore::brush::CommandExecutor", sizeof(CommandExecutor));

    BIND_STRUCT_CONSTRUCTOR(st, "main", SpatialTree *, Brush *);
    BIND_STRUCT_MEMBER(st, brush);
    BIND_STRUCT_MEMBER(st, tree);
    BIND_STRUCT_MEMBER(st, meshLog);
    BIND_STRUCT_METHOD(st, execBrush, MARGS("brushType", "nodes", "origin", "normal"));
    BIND_STRUCT_METHOD(st, clearIsFirstOfStep, MARGS());

    return st;
  }

  CommandExecutor(SpatialTree *tree, Brush *brush) : tree(tree), brush(brush), ctx()
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
    brush_command def;

    switch (brushType) {
    case SculptBrushes::DRAW:
      command::createDrawBrush(def);
      return def;
    case SculptBrushes::INFLATE:
      command::createInflateBrush(def);
      return def;
    case SculptBrushes::CLAY:
      command::createClayBrush(def);
      return def;
    case SculptBrushes::PINCH:
      command::createPinchBrush(def);
      return def;
    case SculptBrushes::SHARP:
      command::createSharpBrush(def);
      return def;
    case SculptBrushes::MASK:
      command::createMaskBrush(def);
      return def;
    case SculptBrushes::SMOOTH:
      command::createSmoothBrush(def);
      return def;
    case SculptBrushes::KELVINLET:
      command::createKelvinletBrush(def);
      return def;
    default:
      printf("Unknown brush type %d\n", static_cast<int>(brushType));
      abort();
    }
  }

  void exec(brush_command &cmd, std::span<spatial::SpatialNode *> nodes)
  {
    vertex_iter_factory vertexIterFactory = createIterFactory();

    cmd.execPre(ctx, nodes);

#ifdef NO_PARALLEL_FOR
    for (auto *node : nodes) {
      CommandCtx<CommandExecutor> finalCtx(ctx, *node, vertexIterFactory, *brush);
      cmd.exec(finalCtx);
    }
#else
    litestl::task::parallel_for(util::IndexRange(nodes.size()), [&](IndexRange range) {
      for (int i : range) {
        SpatialNode *node = nodes[i];
        CommandCtx<CommandExecutor> finalCtx(ctx, *node, vertexIterFactory, *brush);
        cmd.exec(finalCtx);
      }
    }, 4);
#endif

    cmd.execPost(ctx, nodes);
  }

  void execBrush(SculptBrushes brushType,
                 Vector<spatial::SpatialNode *> *nodes,
                 float3 origin,
                 float3 normal)
  {
    auto cmd = createCommand(brushType);
    ctx.surfaceNo = normal;
    ctx.surfacePos = origin;
    ctx.meshLog = meshLog;
    ctx.isFirstOfStep = isFirstOfStep;

    exec(cmd, std::span<spatial::SpatialNode *>(nodes->data(), nodes->size()));
  }

  void clearIsFirstOfStep()
  {
    isFirstOfStep = false;
  }

  void beginStep()
  {
    isFirstOfStep = true;
    if (meshLog) {
      meshLog->beginStep();
    }
  }

  void endStep()
  {
    isFirstOfStep = false;
    if (meshLog) {
      meshLog->endStep();
    }
  }
};
} // namespace sculptcore::brush