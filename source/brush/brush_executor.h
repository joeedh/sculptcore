#pragma once

#include "binding/binding_constructor_builder.h"
#include "brush_command.h"
#include "brush_iterators.h"
#include "neighbor_source.h"
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

  // Selects how for_neighbor kernels enumerate the 1-ring: the live disk walk
  // (default) or the cached CSR adjacency (MeshTopoCache::ring1). The choice is
  // made once here and lowered into the kernel instantiation, so the inner loop
  // has no per-neighbor branch.
  enum class NeighborMode { LiveDisk, Csr };

  Brush *brush;
  SpatialTree *tree;
  CommandCtxBase ctx;
  bool isFirstOfStep = false;
  NeighborMode neighborMode = NeighborMode::LiveDisk;
  meshlog::MeshLog *meshLog = nullptr;
  Vector<float3> coPrevStorage;  // backing store for ctx.co_prev (Jacobi snapshot)

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
      if (neighborMode == NeighborMode::Csr) {
        command::createSmoothBrush<CommandExecutor, CsrNbr>(def);
      } else {
        command::createSmoothBrush(def);
      }
      return def;
    case SculptBrushes::KELVINLET:
      command::createKelvinletBrush(def);
      return def;
    case SculptBrushes::POSE:
      command::createPoseBrush(def);
      return def;
    case SculptBrushes::TEXDRAW:
      command::createTexdrawBrush(def);
      return def;
    default:
      printf("Unknown brush type %d\n", static_cast<int>(brushType));
      abort();
    }
  }

  void exec(brush_command &cmd, std::span<spatial::SpatialNode *> nodes)
  {
    vertex_iter_factory vertexIterFactory = createIterFactory();

    if (cmd.execHost) cmd.execHost(ctx, *brush);
    cmd.execPre(ctx, nodes);

    // Jacobi snapshot: capture pre-dab vertex positions so for_neighbor reads
    // a consistent state regardless of the parallel node loop's interleaving.
    if (cmd.needsCoPrev && nodes.size() > 0) {
      mesh::Mesh *m = nodes[0]->data->m;
      coPrevStorage.resize(m->v.count);
      for (int i = 0; i < m->v.count; i++) {
        coPrevStorage[i] = m->v.co[i];
      }
      ctx.co_prev = &coPrevStorage;

      // CSR neighbor source is static across the stroke — (re)build once,
      // single-threaded, before the parallel node loop reads it.
      if (neighborMode == NeighborMode::Csr) {
        m->topo_cache.ensureRing1(*m);
      }
    }

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

  // SMOOTH is the only brush with a for_neighbor loop, and only its CSR
  // instantiation reads neighbors from the cache rather than the live disk.
  // Every other brush (and CSR-mode smooth) touches no live TOPO link during a
  // dab, so the mesh can sit topology-frozen — dropping the link pages — for
  // the whole stroke. A live-disk smooth dab is the lone case that needs the
  // links back.
  bool brushNeedsLiveLinks(SculptBrushes brushType) const
  {
    return brushType == SculptBrushes::SMOOTH && neighborMode != NeighborMode::Csr;
  }

  void execBrush(SculptBrushes brushType,
                 Vector<spatial::SpatialNode *> *nodes,
                 float3 origin,
                 float3 normal)
  {
    // Enter/leave frozen-topology mode per dab (both calls early-out when
    // already in the target state, so this is cheap to re-check every dab).
    // Note: this is the C++ executor path only; the GPU dispatch in gpu_stroke
    // has its own neighbor handling and is unaffected.
    if (nodes->size() > 0) {
      mesh::Mesh *m = (*nodes)[0]->data->m;
      if (brushNeedsLiveLinks(brushType)) {
        if (m->topo_frozen) m->thawTopo();
      } else if (!m->topo_frozen) {
        m->freezeTopo();
      }
    }

    auto cmd = createCommand(brushType);
    ctx.surfaceNo = normal;
    ctx.surfacePos = origin;
    ctx.meshLog = meshLog;
    ctx.isFirstOfStep = isFirstOfStep;

    // Record this dab center so STROKE_CURVED can map vertices onto the
    // accumulated stroke polyline. Incremental on purpose: a dab's vertices
    // see the path up to and including this dab.
    brush->pushStrokeSample(origin, normal);

    exec(cmd, std::span<spatial::SpatialNode *>(nodes->data(), nodes->size()));
  }

  void clearIsFirstOfStep()
  {
    isFirstOfStep = false;
  }

  void beginStep()
  {
    isFirstOfStep = true;
    if (brush) {
      brush->resetStrokePath();
    }
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