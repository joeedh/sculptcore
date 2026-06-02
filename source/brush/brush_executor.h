#pragma once

#include "binding/binding_constructor_builder.h"
#include "brush_command.h"
#include "brush_iterators.h"
#include "neighbor_source.h"
#include "brushes/all.h"
#include "dyntopo/dyntopo.h"
#include "mesh/attribute_bool.h"
#include "mesh/boundary.h"
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

// One sparse float override applied on top of a brush's authored props for the
// duration of a single sub-command. Keyed by `BrushProp` id (not a string —
// the TS binding runtime can't marshal a JS string into a `util::string` arg).
struct BrushFloatOverride {
  int propId = 0;
  float value = 0.0f;
};

// Redirects one of a kernel's declared attribute handles (by its 0-based index
// in the kernel's attr manifest) to a specific existing mesh layer (by its
// index in that domain's AttrGroup), instead of the codegen default of
// ensure-by-handle-name. This is how the TS attribute manager points the
// color/poly-group/UV brushes at the user-selected "active" layer per category
// — all ints, since the TS binding runtime can't marshal a JS string into a
// `util::string` method arg (same reason BrushFloatOverride is propId-keyed).
struct BrushAttrLayerOverride {
  int attrIdx = 0;    // index into BrushCommandDef::attrs (the manifest)
  int layerIndex = 0; // index into the domain's AttrGroup::attrs
};

// A single sub-command in a composite brush program: a brush type plus a set of
// sparse property overrides. Overrides are pushed onto the brush's authored
// props before the command runs and rolled back after, so the brush's base
// props survive the dab unmodified.
struct BrushCommandEntry {
  SculptBrushes type = SculptBrushes::DRAW;
  Vector<BrushFloatOverride> floatOverrides;
  Vector<BrushAttrLayerOverride> attrLayerOverrides;
  bool overrideInvert = false;
  bool invertValue = false;
};

// An ordered list of brush sub-commands run over the *same* node set per dab —
// the composite-brush ("command list") abstraction the brush executor doc
// describes. Autosmooth is a `[main, SMOOTH]` program; a future dyntopo pass is
// just an entry prepended to `commands` with no API change. Built from TS via
// the bound methods below and handed to `CommandExecutor::execProgram`.
struct BrushProgram {
  Vector<BrushCommandEntry> commands;

  void clear()
  {
    commands.clear();
  }

  // Append a command for the given SculptBrushes value (passed as int so the
  // binding stays a plain scalar method); returns its index.
  int addCommand(int type)
  {
    BrushCommandEntry entry;
    entry.type = static_cast<SculptBrushes>(type);
    commands.append(std::move(entry));
    return int(commands.size()) - 1;
  }

  void setCommandFloat(int idx, int propId, float v)
  {
    if (idx < 0 || idx >= int(commands.size())) {
      return;
    }
    commands[idx].floatOverrides.append(BrushFloatOverride{propId, v});
  }

  void setCommandInvert(int idx, bool inv)
  {
    if (idx < 0 || idx >= int(commands.size())) {
      return;
    }
    commands[idx].overrideInvert = true;
    commands[idx].invertValue = inv;
  }

  // Redirect declared attr handle `attrIdx` of command `idx` to the mesh layer
  // at `layerIndex` in that attr's domain group (see BrushAttrLayerOverride).
  void setCommandAttrLayer(int idx, int attrIdx, int layerIndex)
  {
    if (idx < 0 || idx >= int(commands.size())) {
      return;
    }
    commands[idx].attrLayerOverrides.append(
        BrushAttrLayerOverride{attrIdx, layerIndex});
  }

  static litestl::binding::types::Struct<BrushProgram> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<BrushProgram> *st = new types::Struct<BrushProgram>(
        "sculptcore::brush::BrushProgram", sizeof(BrushProgram));

    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_METHOD(st, clear, MARGS());
    BIND_STRUCT_METHOD(st, addCommand, MARGS("type"));
    BIND_STRUCT_METHOD(st, setCommandFloat, MARGS("idx", "propId", "v"));
    BIND_STRUCT_METHOD(st, setCommandInvert, MARGS("idx", "inv"));
    BIND_STRUCT_METHOD(st, setCommandAttrLayer, MARGS("idx", "attrIdx", "layerIndex"));

    return st;
  }
};

struct CommandExecutor {
  using vertex_iter = BasicVertexIter;
  using vertex_iter_factory = std::function<vertex_iter(spatial::SpatialNode &)>;
  using face_iter = BasicFaceIter;
  using face_iter_factory = std::function<face_iter(spatial::SpatialNode &)>;
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
  /* Keep topology thawed across the stroke (don't freeze per dab). Set by the
   * dyntopo path: a dyntopo dab mutates topology and needs live disk/radial
   * links, so the per-dab freeze would otherwise force an O(mesh) thaw every
   * dab. Brushes that already need live links thaw regardless. */
  bool keepTopoThawed = false;
  NeighborMode neighborMode = NeighborMode::LiveDisk;
  meshlog::MeshLog *meshLog = nullptr;
  /* Stats of the most recent applyDynTopoDab, for the TS HUD (read after each
   * dab and accumulated per stroke). */
  dyntopo::DynTopoStats lastDynTopoStats;
  Vector<float3> coPrevStorage;  // backing store for ctx.co_prev (Jacobi snapshot)
  // Backing store for resolved DSL attribute bindings (ctx.attrBindings),
  // rebuilt per dab in exec().
  BrushAttrBindings attrBindingStorage;

  static litestl::binding::types::Struct<CommandExecutor> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<CommandExecutor> *st = new types::Struct<CommandExecutor>(
        "sculptcore::brush::CommandExecutor", sizeof(CommandExecutor));

    BIND_STRUCT_CONSTRUCTOR(st, "main", SpatialTree *, Brush *);
    BIND_STRUCT_MEMBER(st, brush);
    BIND_STRUCT_MEMBER(st, tree);
    BIND_STRUCT_MEMBER(st, meshLog);
    BIND_STRUCT_MEMBER(st, lastDynTopoStats);
    BIND_STRUCT_METHOD(st, execBrush, MARGS("brushType", "nodes", "origin", "normal"));
    BIND_STRUCT_METHOD(st, execProgram, MARGS("prog", "nodes", "origin", "normal"));
    BIND_STRUCT_METHOD(st, applyDynTopoDab, MARGS("center", "radius", "params", "seed"));
    BIND_STRUCT_METHOD(st, endDynTopoStroke, MARGS());
    BIND_STRUCT_METHOD(st, clearIsFirstOfStep, MARGS());
    BIND_STRUCT_METHOD(st, setNeighborMode, MARGS("mode"));

    return st;
  }

  CommandExecutor(SpatialTree *tree, Brush *brush) : tree(tree), brush(brush), ctx()
  {
  }

  // Select the SMOOTH for_neighbor source: 0 = LiveDisk (live topology links),
  // 1 = Csr (the cached ring1 adjacency). The LiteMesh sculpt path uses Csr —
  // a freshly built mesh doesn't maintain live disk links, so LiveDisk smooth
  // finds no neighbors and no-ops. Exposed as an int (NeighborMode is an
  // unbound enum).
  void setNeighborMode(int mode)
  {
    neighborMode = static_cast<NeighborMode>(mode);
  }

  auto createIterFactory()
  {
    return [this](spatial::SpatialNode &node) -> vertex_iter {
      return vertex_iter(node, *this);
    };
  }

  auto createFaceIterFactory()
  {
    return [this](spatial::SpatialNode &node) -> face_iter {
      return face_iter(node, *this);
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
    case SculptBrushes::SCRAPE:
    case SculptBrushes::FILL:
      // Clay-family plane brushes share one kernel; the bridge sets
      // planeoff/planeSide per tool to select build-up / cut / fill.
      command::createPlaneBrush(def);
      return def;
    case SculptBrushes::WINGSCRAPE:
      command::createWingscrapeBrush(def);
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
    case SculptBrushes::COLOR:
      command::createColorBrush(def);
      return def;
    case SculptBrushes::POLYGROUP:
      command::createPolygroupBrush(def);
      return def;
    case SculptBrushes::BSMOOTH:
      if (neighborMode == NeighborMode::Csr) {
        command::createBsmoothBrush<CommandExecutor, CsrNbr>(def);
      } else {
        command::createBsmoothBrush(def);
      }
      return def;
    default:
      printf("Unknown brush type %d\n", static_cast<int>(brushType));
      abort();
    }
  }

  // Map a declared attribute domain to the mesh's element AttrGroup.
  static mesh::AttrGroup *attrGroupForDomain(mesh::Mesh *m, AttrElemDomain d)
  {
    switch (d) {
    case AttrElemDomain::Vertex: return &m->v.attrs;
    case AttrElemDomain::Face:   return &m->f.attrs;
    case AttrElemDomain::Edge:   return &m->e.attrs;
    case AttrElemDomain::Corner: return &m->c.attrs;
    }
    return nullptr;
  }

  static int elemCountForDomain(mesh::Mesh *m, AttrElemDomain d)
  {
    switch (d) {
    case AttrElemDomain::Vertex: return m->v.count;
    case AttrElemDomain::Face:   return m->f.count;
    case AttrElemDomain::Edge:   return m->e.count;
    case AttrElemDomain::Corner: return m->c.count;
    }
    return 0;
  }

  void exec(brush_command &cmd,
            std::span<spatial::SpatialNode *> nodes,
            std::span<const BrushAttrLayerOverride> attrOverrides = {})
  {
    vertex_iter_factory vertexIterFactory = createIterFactory();
    face_iter_factory faceIterFactory = createFaceIterFactory();

    // Resolve declared attribute layers once per dab (shared across all nodes;
    // the AttrData pointers are mesh-wide and stable for the dab's duration).
    attrBindingStorage.clear();
    ctx.attrBindings = nullptr;
    if (cmd.attrs.size() > 0 && nodes.size() > 0) {
      mesh::Mesh *m = nodes[0]->data->m;
      for (int ai = 0; ai < int(cmd.attrs.size()); ai++) {
        auto &entry = cmd.attrs[ai];
        mesh::AttrGroup *grp = attrGroupForDomain(m, entry.domain);
        if (!grp) continue;

        // An override redirects this handle to the user-selected "active"
        // layer (by index). Honour it only when the layer exists and its type
        // matches the handle's declared type (the TS attribute manager already
        // constrains categories by type, so a mismatch means a stale index —
        // fall through to the default by-name binding rather than corrupt the
        // wrong-typed layer).
        int ovLayer = -1;
        for (const auto &ov : attrOverrides) {
          if (ov.attrIdx == ai) { ovLayer = ov.layerIndex; break; }
        }
        if (ovLayer >= 0 && ovLayer < int(grp->attrs.size()) &&
            grp->attrs[ovLayer].type == entry.type) {
          // Materialize the chosen layer by its (type,name) and bind it. Copy
          // the name first — ensure() of an existing layer won't realloc, but a
          // local keeps the AttrRef& from dangling regardless.
          string nm = grp->attrs[ovLayer].name;
          mesh::AttrRef ref = grp->ensure(entry.type, nm, /*materialize=*/true);
          attrBindingStorage.items.append(BrushAttrBinding{entry.handle, ref});
          continue;
        }

        string layer = entry.boundName.size() ? entry.boundName : entry.handle;
        bool existed = grp->has(entry.type, layer);
        mesh::AttrRef ref = grp->ensure(entry.type, layer, /*materialize=*/true);
        if (!existed) {
          // Value-init a freshly created layer so unpainted elements are
          // deterministic: paint reads+writes the layer, and an uninitialized
          // page would make output depend on heap garbage (breaking GPU A/B +
          // goldens). set_default zeroes simple/vector types.
          int n = elemCountForDomain(m, entry.domain);
          mesh::detail::type_dispatch(entry.type, [&]<typename T>() {
            if constexpr (std::is_same_v<T, bool>) {
              // BoolAttrView has no set_default; clear it explicitly so a fresh
              // bool layer isn't read as heap garbage (was previously skipped).
              auto *bv = static_cast<mesh::BoolAttrView *>(ref.data);
              for (int i = 0; i < n; i++) bv->set(i, false);
            } else {
              auto *dd = static_cast<mesh::AttrData<T> *>(ref.data);
              for (int i = 0; i < n; i++) dd->set_default(i);
            }
          });
        }
        attrBindingStorage.items.append(BrushAttrBinding{entry.handle, ref});
      }
      ctx.attrBindings = &attrBindingStorage;
    }

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
      CommandCtx<CommandExecutor> finalCtx(ctx, *node, vertexIterFactory, faceIterFactory, *brush);
      cmd.exec(finalCtx);
    }
#else
    litestl::task::parallel_for(util::IndexRange(nodes.size()), [&](IndexRange range) {
      for (int i : range) {
        SpatialNode *node = nodes[i];
        CommandCtx<CommandExecutor> finalCtx(ctx, *node, vertexIterFactory, faceIterFactory, *brush);
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
    // Face-stage brushes walk the face loop (live links) to compute centroids.
    return ((brushType == SculptBrushes::SMOOTH || brushType == SculptBrushes::BSMOOTH) &&
            neighborMode != NeighborMode::Csr) ||
           brushType == SculptBrushes::POLYGROUP;
  }

  // The boundary-aware smooth brush reads the lazily-derived
  // `.boundary.vert.class`. Fold any pending boundary edits (seam marking,
  // poly-group paint) into it once at stroke start, while topology links are
  // live — recomputeDirty walks the disk/radial cycles and would touch freed
  // pages under frozen topology.
  //
  // Gated on m->boundaryDirty: when nothing changed since the last recompute
  // (the common case — e.g. plain smoothing with no boundaries marked) this is a
  // no-op and, crucially, does NOT thaw. An unconditional thaw here perturbs the
  // frozen-topology CSR neighbor set the stroke relies on, making bsmooth
  // diverge from plain smooth even with zero boundaries.
  void refreshBoundaryClassForBSmooth(mesh::Mesh *m)
  {
    if (!m->boundaryDirty) return;
    if (m->topo_frozen) m->thawTopo();
    mesh::boundary::recomputeDirty(m);
  }

  // After a poly-group dab, mark every face the touched nodes own boundary-dirty
  // so the next recomputeDirty reclassifies their inter-group edges. A superset
  // of the actually-repainted faces (bounded by the dab's node coverage), which
  // only costs extra recompute, never wrong results. Runs while topology is live
  // (POLYGROUP is a live-links brush).
  void markPolygroupDirty(std::span<spatial::SpatialNode *> nodes)
  {
    for (spatial::SpatialNode *node : nodes) {
      mesh::Mesh *m = node->data->m;
      for (int f : node->data->unique_faces) {
        mesh::boundary::markFaceDirty(m, f);
      }
    }
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
      if (brushType == SculptBrushes::BSMOOTH && isFirstOfStep) {
        refreshBoundaryClassForBSmooth(m);
      }
      if (brushNeedsLiveLinks(brushType) || keepTopoThawed) {
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

    std::span<spatial::SpatialNode *> nodeSpan(nodes->data(), nodes->size());
    exec(cmd, nodeSpan);

    if (brushType == SculptBrushes::POLYGROUP) {
      markPolygroupDirty(nodeSpan);
    }
  }

  // Run a composite brush program over one node set per dab. Each sub-command
  // resolves the brush's props (with its sparse overrides applied) into the
  // cached scalars, then runs like a standalone brush. Used for autosmooth
  // (`[main, SMOOTH]`): SMOOTH is a second `exec()` whose `co_prev` snapshot is
  // re-taken *after* the main pass mutated positions, so it smooths the result.
  void execProgram(BrushProgram *prog,
                   Vector<spatial::SpatialNode *> *nodes,
                   float3 origin,
                   float3 normal)
  {
    if (!prog || prog->commands.size() == 0) {
      return;
    }

    // Topology freeze/thaw is decided once for the whole dab: thaw if *any*
    // sub-command needs live disk links (a live-disk smooth), otherwise freeze
    // for the program's duration. Mixed programs (e.g. DRAW + SMOOTH) thaw,
    // which is harmless for the link-agnostic commands.
    if (nodes->size() > 0) {
      bool needsLive = false;
      bool hasBSmooth = false;
      for (auto &entry : prog->commands) {
        if (brushNeedsLiveLinks(entry.type)) needsLive = true;
        if (entry.type == SculptBrushes::BSMOOTH) hasBSmooth = true;
      }
      mesh::Mesh *m = (*nodes)[0]->data->m;
      // Must precede the freeze below — recomputeDirty needs live links.
      if (hasBSmooth && isFirstOfStep) {
        refreshBoundaryClassForBSmooth(m);
      }
      if (needsLive || keepTopoThawed) {
        if (m->topo_frozen) m->thawTopo();
      } else if (!m->topo_frozen) {
        m->freezeTopo();
      }
    }

    // Stroke tangent for this dab (Route A): direction from the previous dab
    // center to this one. Must be read *before* pushStrokeSample appends the
    // current origin. Drives wing-scrape and the oriented Box falloff.
    if (brush->strokePathCount > 0) {
      float3 d = origin - brush->strokePath[brush->strokePathCount - 1].pos;
      float len = d.length();
      if (len > 1e-7f) {
        brush->strokeDir = d / len;
      }
    }

    // The oriented Box falloff follows the stroke: align its primary axis with
    // the stroke tangent (the bridge only flips the shape to Box; the direction
    // is owned here so it stays consistent with wing-scrape's strokeDir).
    if (brush->falloff_shape == FalloffShape::Box) {
      brush->falloff_dir = brush->strokeDir;
    }

    // One stroke sample per dab (not per sub-command): the stroke advances once.
    brush->pushStrokeSample(origin, normal);

    for (auto &entry : prog->commands) {
      // Apply this command's sparse overrides onto the authored props,
      // snapshotting the prior values so the brush's base props survive the
      // dab unmodified (the next dab re-syncs them from the bridge regardless).
      Vector<BrushFloatOverride> savedFloats;
      for (auto &ov : entry.floatOverrides) {
        const char *nm = brushPropName(ov.propId);
        savedFloats.append(
            BrushFloatOverride{ov.propId, brush->props.lookupFloat(nm, 0.0f)});
        brush->props.setFloat(nm, ov.value);
      }
      bool savedInvert = brush->invert;
      if (entry.overrideInvert) {
        brush->props.setValue<bool>("invert", entry.invertValue);
      }

      // Resolve authored props → cached scalars (applies device dynamics).
      brush->loadProps();

      auto cmd = createCommand(entry.type);
      ctx.surfaceNo = normal;
      ctx.surfacePos = origin;
      ctx.meshLog = meshLog;
      ctx.isFirstOfStep = isFirstOfStep;

      std::span<spatial::SpatialNode *> nodeSpan(nodes->data(), nodes->size());
      exec(cmd, nodeSpan,
           std::span<const BrushAttrLayerOverride>(entry.attrLayerOverrides.data(),
                                                   entry.attrLayerOverrides.size()));

      if (entry.type == SculptBrushes::POLYGROUP) {
        markPolygroupDirty(nodeSpan);
      }

      // Roll the base props back.
      for (auto &s : savedFloats) {
        brush->props.setFloat(brushPropName(s.propId), s.value);
      }
      if (entry.overrideInvert) {
        brush->props.setValue<bool>("invert", savedInvert);
      }
    }
  }

  // Run one dynamic-topology dab under the cursor. Reproduces the native
  // debug harness's Scene::applyDynTopoDab wiring (thaw + combined meshlog/
  // spatial callbacks + in-region seed) MINUS tree->update() and the meshlog
  // step: the TS sculpt path already drives spatial.update() each frame
  // (LiteMesh.drawQ) and wraps the whole stroke in one meshlog step. Returns
  // splits+collapses applied (DynTopoStats.splits + .collapses).
  int applyDynTopoDab(float3 center, float radius, dyntopo::DynTopoParams *params,
                      uint32_t seed)
  {
    if (!tree || !params || !tree->m) {
      return 0;
    }
    mesh::Mesh *m = tree->m;

    // A dyntopo dab mutates topology and walks live disk/radial links; keep the
    // mesh thawed for the whole stroke (endDynTopoStroke releases it).
    keepTopoThawed = true;
    if (m->topo_frozen) {
      m->thawTopo();
    }

    // Fold any pending boundary edits (seam marking / poly-group paint, and the
    // flags propagated by in-stroke splits) into the derived flags + per-vert
    // class while links are live (recomputeDirty walks the disk/radial cycles).
    // Self-limiting: recomputeDirty processes only dirty elements and clears
    // m->boundaryDirty, so this is a no-op on dabs that changed nothing.
    if (params->preserve_features && m->boundaryDirty) {
      mesh::boundary::recomputeDirty(m);
    }

    // Combined callbacks: meshlog (undo) fanned with the spatial tree's
    // incremental-ownership handlers. getSpatialCallbacks() returns a shared
    // persistent member, so copy its three std::functions by value before
    // composing — never alias it across dabs.
    mesh::MeshCallbacks combined;
    mesh::MeshCallbacks *cb = nullptr;
    mesh::MeshCallbacks *sp = tree->getSpatialCallbacks();
    mesh::MeshCallbacks *ml = meshLog ? meshLog->callbacks() : nullptr;
    if (sp && ml) {
      combined = *ml;
      auto mlFC = combined.onFaceCreate, spFC = sp->onFaceCreate;
      combined.onFaceCreate = [mlFC, spFC](int f) {
        if (mlFC) mlFC(f);
        if (spFC) spFC(f);
      };
      auto mlFK = combined.onFaceKill, spFK = sp->onFaceKill;
      combined.onFaceKill = [mlFK, spFK](int f) {
        if (mlFK) mlFK(f); /* meshlog snapshots before the tree drops it */
        if (spFK) spFK(f);
      };
      auto mlVK = combined.onVertKill, spVK = sp->onVertKill;
      combined.onVertKill = [mlVK, spVK](int v) {
        if (mlVK) mlVK(v);
        if (spVK) spVK(v);
      };
      cb = &combined;
    } else {
      cb = ml ? ml : sp;
    }

    // Round-0 seed: verts of the in-region leaves, so the dab is O(brush region)
    // rather than O(mesh). The caller owns the spatial query; dyntopo stays
    // spatial-free and just receives the set.
    Vector<spatial::SpatialNode *> hit;
    tree->filterNodes(center, radius, hit);
    Vector<int> seedVerts;
    for (spatial::SpatialNode *n : hit) {
      for (int v : n->unique_verts()) {
        seedVerts.append(v);
      }
    }

    dyntopo::DynTopoStats st = dyntopo::applyBrushDab(
        *m, center, radius, *params, seed, cb,
        span<const int>(seedVerts.data(), seedVerts.size()));

    lastDynTopoStats = st;
    return st.splits + st.collapses;
  }

  // Release the stroke-long topology thaw set by applyDynTopoDab. Call once at
  // stroke end; the next non-dyntopo dab re-freezes on its own. Also folds in the
  // final dab's pending boundary marks (the per-dab recompute runs at the *next*
  // dab's start, so the last dab's new geometry would otherwise stay unclassified
  // until the next stroke) while topology links are still live.
  void endDynTopoStroke()
  {
    if (tree && tree->m && tree->m->boundaryDirty) {
      if (tree->m->topo_frozen) {
        tree->m->thawTopo();
      }
      mesh::boundary::recomputeDirty(tree->m);
    }
    keepTopoThawed = false;
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