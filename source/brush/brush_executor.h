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
#include <cmath>
#include <cstdio>
#include <functional>
#include <span>

namespace sculptcore::brush {

using namespace litestl::util;
using namespace litestl::math;

// Result of the per-stroke uniform-dynamics validation (Wave 4). `ok == false`
// means the active brush's dynamic bindings are misconfigured and the stroke is
// skipped without mutating the mesh; `messages` carries one line per problem.
struct UniformValidationResult {
  bool ok = true;
  Vector<string> messages;
};

// One sparse float override applied on top of a brush's authored props for the
// duration of a single sub-command. Resolved by `name` when set (the generated
// per-kernel uniforms — `mu`, `planeoff`, ...), else by `BrushProp` id for the
// common props (the TS binding runtime can't marshal a JS string into a
// `util::string` arg, so the int path stays for that bridge surface).
struct BrushFloatOverride {
  int propId = 0;
  float value = 0.0f;
  util::string name;
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

  // Override a kernel uniform by its declared name (the generated per-kernel
  // props: `mu`, `nu`, `planeoff`, ...). Distinct from setCommandFloat, which
  // keys the common props by int id.
  void setCommandFloatByName(int idx, util::string name, float v)
  {
    if (idx < 0 || idx >= int(commands.size())) {
      return;
    }
    BrushFloatOverride ov;
    ov.value = v;
    ov.name = name;
    commands[idx].floatOverrides.append(std::move(ov));
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
    BIND_STRUCT_METHOD(st, setCommandFloatByName, MARGS("idx", "name", "v"));
    BIND_STRUCT_METHOD(st, setCommandInvert, MARGS("idx", "inv"));
    BIND_STRUCT_METHOD(st, setCommandAttrLayer, MARGS("idx", "attrIdx", "layerIndex"));

    return st;
  }
};

struct CommandExecutor {
  // vertex_iter names the AccumLive instantiation so the CommandTypes concept
  // and the factory typedefs are valid type-ids; the generated kernels pick the
  // AccumMode per command via makeVertexIter<AccMode>.
  using vertex_iter = BasicVertexIter<AccumLive>;
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
  // Non-accumulate mode (see plans/nonAccumMode.md). When `nonAccum` is set and a
  // command is accumulable, the executor stamps each in-region vert's stroke-start
  // position into `.brush.orig.*` (keyed by `strokeGen`) and runs the AccumOrig
  // kernel instantiation so deformation is measured from that snapshot.
  bool nonAccum = false;
  uint32_t strokeGen = 0;
  meshlog::MeshLog *meshLog = nullptr;
  /* Stats of the most recent applyDynTopoDab, for the TS HUD (read after each
   * dab and accumulated per stroke). */
  dyntopo::DynTopoStats lastDynTopoStats;
  Vector<float3> coPrevStorage;  // backing store for ctx.co_prev (Jacobi snapshot)
  // Backing store for resolved DSL attribute bindings (ctx.attrBindings),
  // rebuilt per dab in exec().
  BrushAttrBindings attrBindingStorage;
  // Uniform-dynamics validation (Wave 4): run once per stroke (first dab) against
  // the active brush's manifest. On failure the whole stroke is skipped so the
  // mesh is never mutated by a misconfigured binding. `lastValidation` is the
  // most recent result (readable by the bridge); `strokeValidationFailed` gates
  // every dab of a failed stroke.
  UniformValidationResult lastValidation;
  bool strokeValidationFailed = false;
  // Wave 5: the active brush's uniform manifest, cached by queryUniformManifest
  // so the TS bridge can enumerate it by index (the binding runtime can't pass a
  // JS string into a `util::string` method arg). queriedUniformEntry hands each
  // entry back by pointer; the *UniformDynamics methods resolve index -> name and
  // delegate to the Brush by-name dynamics API.
  Vector<BrushUniformManifestEntry> queriedUniforms;

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
    BIND_STRUCT_METHOD(st, setNonAccum, MARGS("nonAccum"));
    BIND_STRUCT_METHOD(st, setStrokeGen, MARGS("gen"));
    BIND_STRUCT_METHOD(st, lastUniformValidationOk, MARGS());
    BIND_STRUCT_METHOD(st, queryUniformManifest, MARGS("brushType"));
    BIND_STRUCT_METHOD(st, queriedUniformEntry, MARGS("idx"));
    BIND_STRUCT_METHOD(st, clearUniformDynamics, MARGS("idx"));
    BIND_STRUCT_METHOD(st, addUniformDynamic,
                       MARGS("idx", "deviceType", "mixMode", "mixFactor"));
    BIND_STRUCT_METHOD(st, setUniformDynamicSample,
                       MARGS("idx", "deviceType", "i", "n", "value"));

    return st;
  }

  // Whether the most recent stroke's uniform-dynamics validation passed (Wave 4).
  // The bridge reads this after the first dab to surface a skipped stroke.
  bool lastUniformValidationOk() const
  {
    return lastValidation.ok;
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

  // Enable non-accumulate mode for the upcoming stroke, and set its generation
  // stamp (a monotonic per-stroke counter; must be non-zero, since the
  // `.brush.orig.gen` attr defaults to 0 = "not stamped this stroke").
  void setNonAccum(bool v) { nonAccum = v; }
  void setStrokeGen(int gen) { strokeGen = uint32_t(gen); }

  // Per-call iterator factories used by CommandCtx::vertexIter/faceIter. The
  // vertex iterator is parameterized by the AccumMode policy and threaded the
  // stroke-start cache (null/0 unless non-accumulate is active for this dab).
  template <class AccMode> BasicVertexIter<AccMode> makeVertexIter(spatial::SpatialNode &node)
  {
    return BasicVertexIter<AccMode>(node, *this, ctx.origCo, ctx.origGen, ctx.strokeGen);
  }
  BasicFaceIter makeFaceIter(spatial::SpatialNode &node)
  {
    return BasicFaceIter(node, *this);
  }

  // Fill `def` for `brushType` under a fixed AccumMode policy. createCommand
  // calls this once for AccumLive, then again for AccumOrig when non-accumulate
  // is active and the brush is accumulable — the second call overwrites def.exec
  // with the AccumOrig kernel while keeping the rest of the (identical) manifest.
  template <class AccMode>
  void createCommandImpl(SculptBrushes brushType, brush_command &def)
  {
    switch (brushType) {
    case SculptBrushes::DRAW:
      command::createDrawBrush<CommandExecutor, AccMode>(def);
      return;
    case SculptBrushes::INFLATE:
      command::createInflateBrush<CommandExecutor, AccMode>(def);
      return;
    case SculptBrushes::CLAY:
    case SculptBrushes::SCRAPE:
    case SculptBrushes::FILL:
      // Clay-family plane brushes share one kernel; the bridge sets
      // planeoff/planeSide per tool to select build-up / cut / fill.
      command::createPlaneBrush<CommandExecutor, AccMode>(def);
      return;
    case SculptBrushes::WINGSCRAPE:
      command::createWingscrapeBrush<CommandExecutor, AccMode>(def);
      return;
    case SculptBrushes::PINCH:
      command::createPinchBrush<CommandExecutor, AccMode>(def);
      return;
    case SculptBrushes::SHARP:
      command::createSharpBrush<CommandExecutor, AccMode>(def);
      return;
    case SculptBrushes::MASK:
      command::createMaskBrush<CommandExecutor, AccMode>(def);
      return;
    case SculptBrushes::SMOOTH:
      if (neighborMode == NeighborMode::Csr) {
        command::createSmoothBrush<CommandExecutor, CsrNbr, AccMode>(def);
      } else {
        command::createSmoothBrush<CommandExecutor, LiveDiskNbr, AccMode>(def);
      }
      return;
    case SculptBrushes::KELVINLET:
      command::createKelvinletBrush<CommandExecutor, AccMode>(def);
      return;
    case SculptBrushes::GRAB:
      command::createGrabBrush<CommandExecutor, AccMode>(def);
      return;
    case SculptBrushes::SNAKEHOOK:
      command::createSnakehookBrush<CommandExecutor, AccMode>(def);
      return;
    case SculptBrushes::POSE:
      command::createPoseBrush<CommandExecutor, AccMode>(def);
      return;
    case SculptBrushes::TEXDRAW:
      command::createTexdrawBrush<CommandExecutor, AccMode>(def);
      return;
    case SculptBrushes::COLOR:
      command::createColorBrush<CommandExecutor, AccMode>(def);
      return;
    case SculptBrushes::POLYGROUP:
      command::createPolygroupBrush<CommandExecutor, AccMode>(def);
      return;
    case SculptBrushes::BSMOOTH:
      if (neighborMode == NeighborMode::Csr) {
        command::createBsmoothBrush<CommandExecutor, CsrNbr, AccMode>(def);
      } else {
        command::createBsmoothBrush<CommandExecutor, LiveDiskNbr, AccMode>(def);
      }
      return;
    default:
      printf("Unknown brush type %d\n", static_cast<int>(brushType));
      abort();
    }
  }

  brush_command createCommand(SculptBrushes brushType)
  {
    brush_command def;
    createCommandImpl<AccumLive>(brushType, def);
    if (nonAccum && def.accumulable) {
      createCommandImpl<AccumOrig>(brushType, def);
    }
    return def;
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

  // Per-domain element *capacity*. Use this (not the count) when iterating by raw
  // element id — dyntopo leaves freelist gaps, so a live element's id can exceed
  // the live count.
  static int elemCapacityForDomain(mesh::Mesh *m, AttrElemDomain d)
  {
    switch (d) {
    case AttrElemDomain::Vertex: return int(m->v.capacity());
    case AttrElemDomain::Face:   return int(m->f.capacity());
    case AttrElemDomain::Edge:   return int(m->e.capacity());
    case AttrElemDomain::Corner: return int(m->c.capacity());
    }
    return 0;
  }

  void exec(brush_command &cmd,
            std::span<spatial::SpatialNode *> nodes,
            std::span<const BrushAttrLayerOverride> attrOverrides = {})
  {
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
          // goldens). set_default zeroes simple/vector types. Span capacity (not
          // count): value-init by raw id so dyntopo's freelist-gap slots aren't
          // left as heap garbage for a paint that reads them.
          int n = elemCapacityForDomain(m, entry.domain);
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
    // Indexed by RAW vertex id (so are co[] and the CSR neighbor ids), so it must
    // span the full capacity, not v.count: dyntopo leaves freelist gaps, so a live
    // vertex's id can exceed v.count — a count-sized snapshot would be read
    // out-of-bounds for those verts/neighbors (garbage → smooth spikes).
    if (cmd.needsCoPrev && nodes.size() > 0) {
      mesh::Mesh *m = nodes[0]->data->m;
      int cap = int(m->v.capacity());
      coPrevStorage.resize(cap);
      for (int i = 0; i < cap; i++) {
        coPrevStorage[i] = m->v.co[i];
      }
      ctx.co_prev = &coPrevStorage;

      // CSR neighbor source is static across the stroke — (re)build once,
      // single-threaded, before the parallel node loop reads it.
      if (neighborMode == NeighborMode::Csr) {
        m->topo_cache.ensureRing1(*m);
      }
    }

    // Non-accumulate setup (see plans/nonAccumMode.md): ensure the `.brush.orig.*`
    // TEMP attrs and stamp each in-region vert's stroke-start position under the
    // current generation, single-threaded before the parallel loop. A vert is
    // stamped once per stroke (first contact); later dabs leave the snapshot
    // alone, so the AccumOrig kernel always measures from the stroke start.
    ctx.origCo = nullptr;
    ctx.origGen = nullptr;
    ctx.strokeGen = 0;
    if (nonAccum && cmd.accumulable && nodes.size() > 0) {
      mesh::Mesh *m = nodes[0]->data->m;
      mesh::AttrRef &coRef = m->v.attrs.ensure(mesh::AttrType::FLOAT3, ".brush.orig.co", false);
      coRef.flag |= mesh::AttrFlag::TEMP;
      mesh::AttrRef &genRef = m->v.attrs.ensure(mesh::AttrType::INT, ".brush.orig.gen", false);
      genRef.flag |= mesh::AttrFlag::TEMP;
      ctx.origCo = static_cast<mesh::AttrData<float3> *>(coRef.data);
      ctx.origGen = static_cast<mesh::AttrData<int> *>(genRef.data);
      ctx.strokeGen = strokeGen;
      for (auto *node : nodes) {
        for (int v : node->data->unique_verts) {
          ctx.origGen->materialize(v);
          if ((*ctx.origGen)[v] != int(strokeGen)) {
            ctx.origCo->materialize(v);
            (*ctx.origCo)[v] = m->v.co[v];
            (*ctx.origGen)[v] = int(strokeGen);
          }
        }
      }
    }

#ifdef NO_PARALLEL_FOR
    for (auto *node : nodes) {
      CommandCtx<CommandExecutor> finalCtx(ctx, *node, *this, *brush);
      cmd.exec(finalCtx);
    }
#else
    litestl::task::parallel_for(util::IndexRange(nodes.size()), [&](IndexRange range) {
      for (int i : range) {
        SpatialNode *node = nodes[i];
        CommandCtx<CommandExecutor> finalCtx(ctx, *node, *this, *brush);
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

  // The fixed common float props are valid dynamics targets for any brush (the
  // bridge drives strength/radius/... by pressure regardless of the active
  // kernel), so they're exempt from the active-manifest membership check.
  static bool isCommonFloatProp(const string &name)
  {
    return name == string("strength") || name == string("radius") ||
           name == string("spacing") || name == string("planeoff") ||
           name == string("autosmooth");
  }

  static const BrushUniformManifestEntry *findUniformEntry(brush_command &cmd,
                                                           const string &name)
  {
    for (const auto &u : cmd.uniforms) {
      if (u.name == name) return &u;
    }
    return nullptr;
  }

  // Validate the active brush's uniform dynamics once at stroke start, scoped to
  // its manifest. Catches the shared-`Brush`-struct traps (a stray dynamic left
  // over from another kernel, a dynamic on a `@static`/non-float uniform), an
  // unbaked 1-entry response curve, an out-of-range authored default, and an
  // inverted/NaN `@range`. Returns a structured result; the caller skips the
  // stroke on `!ok` so a misconfigured binding never mutates the mesh.
  UniformValidationResult validateUniformDynamics(brush_command &cmd)
  {
    UniformValidationResult res;
    char buf[256];

    // (A) Static manifest checks — independent of any configured dynamic.
    for (const auto &u : cmd.uniforms) {
      if (!u.hasRange) continue;
      if (std::isnan(u.rangeMin) || std::isnan(u.rangeMax) ||
          u.rangeMin > u.rangeMax) {
        res.ok = false;
        snprintf(buf, sizeof(buf),
                 "uniform '%s': invalid @range [%g, %g]", u.name.c_str(),
                 u.rangeMin, u.rangeMax);
        res.messages.append(string(buf));
        continue;  // a broken range makes the default check meaningless
      }
      if (u.isFloat && (u.def < u.rangeMin || u.def > u.rangeMax)) {
        res.ok = false;
        snprintf(buf, sizeof(buf),
                 "uniform '%s': default %g outside @range [%g, %g]",
                 u.name.c_str(), u.def, u.rangeMin, u.rangeMax);
        res.messages.append(string(buf));
      }
    }

    // (B) Dynamics checks — every prop carrying a configured device stack must
    // be a valid, dynamic-capable target of the active brush.
    if (brush && brush->props.struct_def) {
      for (props::Property *p : brush->props.struct_def->properties()) {
        props::Dynamics *dyn = brush->propDynamics(p->name);
        if (!dyn || dyn->devices.size() == 0) continue;

        const BrushUniformManifestEntry *entry = findUniformEntry(cmd, p->name);
        if (!entry && !isCommonFloatProp(p->name)) {
          res.ok = false;
          snprintf(buf, sizeof(buf),
                   "stray dynamic on '%s': not a uniform of the active brush",
                   p->name.c_str());
          res.messages.append(string(buf));
          continue;
        }
        if (entry && !(entry->isFloat && entry->dynamic)) {
          res.ok = false;
          snprintf(buf, sizeof(buf),
                   "dynamic on '%s': uniform is @static / non-float (not "
                   "dynamic-capable)",
                   p->name.c_str());
          res.messages.append(string(buf));
          continue;
        }
        for (const auto &dev : dyn->devices) {
          if (dev.curveTable.size() == 1) {
            res.ok = false;
            snprintf(buf, sizeof(buf),
                     "uniform '%s': device response curve has 1 entry "
                     "(unbaked; need 0 or >=2)",
                     p->name.c_str());
            res.messages.append(string(buf));
          }
          int dt = (int)dev.type;
          if (dt < 0 || dt > (int)props::DeviceType::TWIST) {
            res.ok = false;
            snprintf(buf, sizeof(buf), "uniform '%s': invalid device type %d",
                     p->name.c_str(), dt);
            res.messages.append(string(buf));
          }
        }
      }
    }

    return res;
  }

  // --- Wave 5: per-kernel uniform manifest query for the TS bridge -----------
  // The binding runtime can't marshal a JS string into a `util::string` method
  // arg, so the bridge enumerates the active brush's manifest by index instead
  // of by name. queryUniformManifest caches the kernel's manifest (and registers
  // its props so propDynamics(name) resolves) and returns the entry count;
  // queriedUniformEntry exposes each entry as a bound read-only struct; the
  // *UniformDynamics methods resolve the index -> name and delegate to the Brush
  // by-name dynamics API (Wave 3).

  int queryUniformManifest(int brushType)
  {
    queriedUniforms.clear();
    auto cmd = createCommand(static_cast<SculptBrushes>(brushType));
    if (cmd.registerProps && brush && brush->props.struct_def) {
      cmd.registerProps(*brush->props.struct_def);
    }
    for (const auto &u : cmd.uniforms) {
      queriedUniforms.append(u);
    }
    return int(queriedUniforms.size());
  }

  BrushUniformManifestEntry *queriedUniformEntry(int idx)
  {
    if (idx < 0 || idx >= int(queriedUniforms.size())) {
      return nullptr;
    }
    return &queriedUniforms[idx];
  }

  void clearUniformDynamics(int idx)
  {
    if (!brush || idx < 0 || idx >= int(queriedUniforms.size())) {
      return;
    }
    brush->clearPropDynamicsByName(queriedUniforms[idx].name);
  }
  void addUniformDynamic(int idx, int deviceType, int mixMode, float mixFactor)
  {
    if (!brush || idx < 0 || idx >= int(queriedUniforms.size())) {
      return;
    }
    brush->addPropDynamicByName(queriedUniforms[idx].name, deviceType, mixMode,
                                mixFactor);
  }
  void setUniformDynamicSample(int idx, int deviceType, int i, int n, float value)
  {
    if (!brush || idx < 0 || idx >= int(queriedUniforms.size())) {
      return;
    }
    brush->setPropDynamicSampleByName(queriedUniforms[idx].name, deviceType, i, n,
                                      value);
  }

  void execBrush(SculptBrushes brushType,
                 Vector<spatial::SpatialNode *> *nodes,
                 float3 origin,
                 float3 normal)
  {
    auto cmd = createCommand(brushType);

    // Validate uniform dynamics once per stroke (first dab), before any mesh or
    // topology state is touched: a misconfigured binding skips the whole stroke
    // so the mesh is never mutated. Subsequent dabs of a failed stroke re-skip
    // via the persisted flag.
    if (isFirstOfStep) {
      lastValidation = validateUniformDynamics(cmd);
      strokeValidationFailed = !lastValidation.ok;
      for (const auto &msg : lastValidation.messages) {
        fprintf(stderr, "brush uniform validation: %s\n", msg.c_str());
      }
    }
    if (strokeValidationFailed) {
      return;
    }

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

    // Validate every sub-command's uniform dynamics once per stroke, before any
    // mesh/topology state is touched. registerProps first so the manifest props
    // exist to inspect (idempotent — the per-command loop re-registers). Any
    // failure skips the whole program so the mesh is never mutated.
    if (isFirstOfStep) {
      lastValidation = UniformValidationResult{};
      for (auto &entry : prog->commands) {
        auto cmd = createCommand(entry.type);
        if (cmd.registerProps && brush && brush->props.struct_def) {
          cmd.registerProps(*brush->props.struct_def);
        }
        UniformValidationResult r = validateUniformDynamics(cmd);
        if (!r.ok) {
          lastValidation.ok = false;
          for (auto &msg : r.messages) lastValidation.messages.append(msg);
        }
      }
      strokeValidationFailed = !lastValidation.ok;
      for (const auto &msg : lastValidation.messages) {
        fprintf(stderr, "brush uniform validation: %s\n", msg.c_str());
      }
    }
    if (strokeValidationFailed) {
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
        // Name-keyed overrides target a generated kernel uniform; id-keyed ones
        // target a common prop. Resolve to the prop name either way and snapshot
        // the prior value under that same name for an exact rollback.
        util::string nm = ov.name.size() ? ov.name : util::string(brushPropName(ov.propId));
        BrushFloatOverride saved;
        saved.name = nm;
        saved.value = brush->props.lookupFloat(nm.c_str(), 0.0f);
        savedFloats.append(std::move(saved));
        brush->props.setFloat(nm.c_str(), ov.value);
      }
      bool savedInvert = brush->invert;
      if (entry.overrideInvert) {
        brush->props.setValue<bool>("invert", entry.invertValue);
      }

      auto cmd = createCommand(entry.type);

      // Register this kernel's scalar-float uniforms as props (idempotent,
      // defaults seeded), then resolve common props + the kernel's uniforms
      // into cached scalars (applies device dynamics). The uniform half is
      // generated per brush from its `uniform` declarations.
      if (cmd.registerProps && brush->props.struct_def) {
        cmd.registerProps(*brush->props.struct_def);
      }
      brush->loadCommonProps(&brush->deviceInputCtx);
      if (cmd.loadUniformProps) {
        cmd.loadUniformProps(*brush, &brush->deviceInputCtx);
      }

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

      // Roll the base props back (saved under the resolved prop name).
      for (auto &s : savedFloats) {
        brush->props.setFloat(s.name.c_str(), s.value);
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

    // Non-accumulate coherence: tell the remesh ops the active stroke's gen so
    // they keep each stamped vert's stroke-start snapshot coherent as topology
    // changes (nonAccumMode.md). Derived from this executor's stroke state, so it
    // can't drift from the brush command's stamp (both key off strokeGen).
    params->nonAccumGen = nonAccum ? strokeGen : 0;

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
    if (meshLog) {
      /* The topo-chunk callbacks no-op without an active mesh (Scene::
       * applyDynTopoDab does the same wiring on the debug-harness path). */
      meshLog->setActiveMesh(m);
    }
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
      auto mlFCh = combined.onFaceChange, spFCh = sp->onFaceChange;
      combined.onFaceChange = [mlFCh, spFCh](int f) {
        if (mlFCh) mlFCh(f); /* meshlog records the rewired (Existed && Live) face */
        if (spFCh) spFCh(f); /* tree re-flags the owning leaf (in-place flip/split) */
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
    strokeValidationFailed = false;
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

// Declared in accum_mode.h; CoProxy::commit uses it to derive the layer cap.
inline float dabFalloffFraction(const CommandExecutor &exec, const float3 &co)
{
  float t = 1.0f - std::min(exec.brush->falloffDist(co - exec.ctx.surfacePos), 1.0f);
  return exec.brush->falloffEval(t);
}
} // namespace sculptcore::brush