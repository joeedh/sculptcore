#pragma once

#include "automask.h"
#include "binding/binding_constructor_builder.h"
#include "brush_command.h"
#include "brush_iterators.h"
#include "enhance.h"
#include "brushes/all.h"
#include "displace/compositor.h"
#include "dyntopo/dyntopo.h"
#include "feature_field.h"
#include "litestl/binding/binding.h"
#include "litestl/util/map.h"
#include "litestl/util/task.h"
#include "mesh/attribute_bool.h"
#include "mesh/boundary.h"
#include "mesh/uv_reproject.h"
#include "meshlog/meshlog.h"
#include "neighbor_source.h"
#include "spatial/node.h"
#include "spatial/spatial.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <span>

namespace sculptcore::brush {

using namespace litestl::util;
using namespace litestl::math;

/** Result of the per-stroke uniform-dynamics validation (Wave 4). `ok == false`
 * means the active brush's dynamic bindings are misconfigured and the stroke is
 * skipped without mutating the mesh; `messages` carries one line per problem. */
struct UniformValidationResult {
  bool ok = true;
  Vector<string> messages;
};

/** One sparse float override applied on top of a brush's authored props for the
 * duration of a single sub-command. Resolved by `name` when set (the generated
 * per-kernel uniforms — `mu`, `planeoff`, ...), else by `BrushProp` id for the
 * common props (the TS binding runtime can't marshal a JS string into a
 * `util::string` arg, so the int path stays for that bridge surface). */
struct BrushFloatOverride {
  int propId = 0;
  float value = 0.0f;
  util::string name;
};

/** Redirects one of a kernel's declared attribute handles (by its 0-based index
 * in the kernel's attr manifest) to a specific existing mesh layer (by its
 * index in that domain's AttrGroup), instead of the codegen default of
 * ensure-by-handle-name. This is how the TS attribute manager points the
 * color/poly-group/UV brushes at the user-selected "active" layer per category
 * — all ints, since the TS binding runtime can't marshal a JS string into a
 * `util::string` method arg (same reason BrushFloatOverride is propId-keyed). */
struct BrushAttrLayerOverride {
  int attrIdx = 0;    // index into BrushCommandDef::attrs (the manifest)
  int layerIndex = 0; // index into the domain's AttrGroup::attrs
};

/** A single sub-command in a composite brush program: a brush type plus a set of
 * sparse property overrides. Overrides are pushed onto the brush's authored
 * props before the command runs and rolled back after, so the brush's base
 * props survive the dab unmodified. */
struct BrushCommandEntry {
  SculptBrushes type = SculptBrushes::DRAW;
  Vector<BrushFloatOverride> floatOverrides;
  Vector<BrushAttrLayerOverride> attrLayerOverrides;
  bool overrideInvert = false;
  bool invertValue = false;
};

/** An ordered list of brush sub-commands run over the *same* node set per dab —
 * the composite-brush ("command list") abstraction the brush executor doc
 * describes. Autosmooth is a `[main, SMOOTH]` program; a future dyntopo pass is
 * just an entry prepended to `commands` with no API change. Built from TS via
 * the bound methods below and handed to `CommandExecutor::execProgram`. */
struct BrushProgram {
  Vector<BrushCommandEntry> commands;

  void clear()
  {
    commands.clear();
  }

  /** Append a command for the given SculptBrushes value (passed as int so the
   * binding stays a plain scalar method); returns its index. */
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

  /** Override a kernel uniform by its declared name (the generated per-kernel
   * props: `mu`, `nu`, `planeoff`, ...). Distinct from setCommandFloat, which
   * keys the common props by int id. */
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

  /** Redirect declared attr handle `attrIdx` of command `idx` to the mesh layer
   * at `layerIndex` in that attr's domain group (see BrushAttrLayerOverride). */
  void setCommandAttrLayer(int idx, int attrIdx, int layerIndex)
  {
    if (idx < 0 || idx >= int(commands.size())) {
      return;
    }
    commands[idx].attrLayerOverrides.append(BrushAttrLayerOverride{attrIdx, layerIndex});
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
  /** vertex_iter names the AccumLive instantiation so the CommandTypes concept
   * and the factory typedefs are valid type-ids; the generated kernels pick the
   * AccumMode per command via makeVertexIter<AccMode>. */
  using vertex_iter = BasicVertexIter<AccumLive>;
  using vertex_iter_factory = std::function<vertex_iter(spatial::SpatialNode &)>;
  using face_iter = BasicFaceIter;
  using face_iter_factory = std::function<face_iter(spatial::SpatialNode &)>;
  using brush_command = BrushCommandDef<CommandCtx<CommandExecutor>>;

  /** Selects how for_neighbor kernels enumerate the 1-ring: the live disk walk
   * (default) or the cached CSR adjacency (MeshTopoCache::ring1). The choice is
   * made once here and lowered into the kernel instantiation, so the inner loop
   * has no per-neighbor branch. */
  enum class NeighborMode { LiveDisk, Csr };

  Brush *brush;
  SpatialTree *tree;
  CommandCtxBase ctx;
  bool isFirstOfStep = false;
  /** True while the current step (stroke) runs dyntopo — leaf element sets can
   * change mid-stroke, so the capture walk-elision stamps are disabled. */
  bool stepHasDyntopo = false;
  /** Sub-command slot of the exec() in flight (index into the brush program;
   * execBrush uses 0). -1 disables capture walk-elision for this exec(). */
  int curCaptureSlot = -1;
  int curCaptureTool = 0;
  /** Scratch for the capture walk-elision node subset (see exec()). */
  Vector<spatial::SpatialNode *> captureNodes_;
  /** coPrev incremental-refresh state: after the stroke's first full snapshot,
   * later needsCoPrev execs refresh only the verts of nodes an exec touched
   * since the previous refresh (coPrevDirty_, deduped by node->coPrevStamp
   * against coPrevGen_). Only valid on topology-stable strokes — same
   * condition as the capture stamps. */
  bool coPrevFull_ = false;
  int coPrevGen_ = 0;
  Vector<spatial::SpatialNode *> coPrevDirty_;

  /** UV slide-reprojection deferral (frozen-topology strokes): vert -> its
   * position before the first dab that moved it. Flushed by endStep — one
   * thaw + reproject per stroke instead of one per dab (which would also
   * perturb the frozen-CSR stroke state). */
  Map<int, float3> uvReprojPending_;
  /** Keep topology thawed across the stroke (don't freeze per dab). Set by the
   * dyntopo path: a dyntopo dab mutates topology and needs live disk/radial
   * links, so the per-dab freeze would otherwise force an O(mesh) thaw every
   * dab. Brushes that already need live links thaw regardless. */
  bool keepTopoThawed = false;
  NeighborMode neighborMode = NeighborMode::LiveDisk;

  /** Effective neighbor source for the current step. CSR only pays off while
   * topology is stable: a dyntopo step mutates topology every dab, which both
   * keeps the live links thawed (LiveDisk is free) and bumps `topo_stamp`
   * (making CSR an O(mesh) ensureRing1 rebuild per dab). Force LiveDisk there
   * regardless of the requested mode. */
  NeighborMode effectiveNeighborMode() const
  {
    return stepHasDyntopo ? NeighborMode::LiveDisk : neighborMode;
  }
  /** Non-accumulate mode (see plans/nonAccumMode.md). When `nonAccum` is set and a
   * command is accumulable, the executor stamps each in-region vert's stroke-start
   * position into `.brush.orig.*` (keyed by `strokeGen`) and runs the AccumOrig
   * kernel instantiation so deformation is measured from that snapshot. */
  bool nonAccum = false;
  /** Grab-class symmetry dab marker (#35). The dab dispatch calls setGrabAccumAdd
   * per symmetry image before applyDab: false on the primary image (which begins
   * a new logical dab → bumps `dabGen`), true on mirror images (same dab). The
   * AccumOrigGrab write-back uses the per-vert `.brush.dab.gen` stamp vs `dabGen`
   * to re-base the first image's verts and add later images' onto them. */
  bool grabAccumAdd = false;
  /** Monotonic per-dab counter for the grab symmetry first-touch stamp. Bumped on
   * each primary image (setGrabAccumAdd(false)); pushed into ctx.curDabGen in
   * exec(). Must be non-zero in use, since the `.brush.dab.gen` attr defaults 0. */
  uint32_t dabGen = 0;
  uint32_t strokeGen = 0;
  meshlog::MeshLog *meshLog = nullptr;
  /** Stats of the most recent applyDynTopoDab, for the TS HUD (read after each
   * dab and accumulated per stroke). */
  dyntopo::DynTopoStats lastDynTopoStats;
  Vector<float3> coPrevStorage; // backing store for ctx.co_prev (Jacobi snapshot)
  /** Backing store for resolved DSL attribute bindings (ctx.attrBindings),
   * rebuilt per dab in exec(). */
  BrushAttrBindings attrBindingStorage;
  /** Sculpt-layer edit brackets for written SCULPT_LAYER bindings, rebuilt per
   * dab in exec() (begin before the kernel, end after execPost). */
  Vector<displace::LayerEditScope> layerScopes;
  Vector<int> layerRegionVerts; // dab-region vert ids the scopes snapshot
  /** Attr-layer overrides applied when exec() is called without an explicit
   * override span (the applyDab/execBrush path — execProgram passes its own).
   * Lets a single-brush driver (debug_app `stroke layer=`) retarget a kernel's
   * attr handle at a chosen mesh layer. */
  Vector<BrushAttrLayerOverride> defaultAttrOverrides;
  /** Uniform-dynamics validation (Wave 4): run once per stroke (first dab) against
   * the active brush's manifest. On failure the whole stroke is skipped so the
   * mesh is never mutated by a misconfigured binding. `lastValidation` is the
   * most recent result (readable by the bridge); `strokeValidationFailed` gates
   * every dab of a failed stroke. */
  UniformValidationResult lastValidation;
  bool strokeValidationFailed = false;
  /** Wave 5: the active brush's uniform manifest, cached by queryUniformManifest
   * so the TS bridge can enumerate it by index (the binding runtime can't pass a
   * JS string into a `util::string` method arg). queriedUniformEntry hands each
   * entry back by pointer; the *UniformDynamics methods resolve index -> name and
   * delegate to the Brush by-name dynamics API. */
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
    BIND_STRUCT_METHOD(st, beginStep, MARGS("hasDyntopo"));
    BIND_STRUCT_METHOD(st, endStep, MARGS());
    BIND_STRUCT_METHOD(
        st, execBrush, MARGS("mesh", "brushType", "nodes", "origin", "normal"));
    BIND_STRUCT_METHOD(st, execProgram, MARGS("prog", "nodes", "origin", "normal"));
    BIND_STRUCT_METHOD(st, applyDynTopoDab, MARGS("center", "radius", "params", "seed"));
    BIND_STRUCT_METHOD_SIG(
        st, applyDab, int,
        MARGS("prog", "center", "normal", "radius", "params", "seed"),
        (BrushProgram *, float3, float3, float, dyntopo::DynTopoParams *, uint32_t));
    // params==nullptr disables dyntopo for the dab; mark it nullable so the
    // generated TS accepts undefined (the _SIG macro isn't chainable).
    st->methods[st->methods.size() - 1]->argIsNullable("params");
    BIND_STRUCT_METHOD(st, endDynTopoStroke, MARGS());
    BIND_STRUCT_METHOD(st, clearIsFirstOfStep, MARGS());
    BIND_STRUCT_METHOD(st, beginPreviewDab, MARGS("center", "radius"));
    BIND_STRUCT_METHOD(st, extendPreviewDab, MARGS("center", "radius"));
    BIND_STRUCT_METHOD(st, rollbackPreviewDab, MARGS());
    BIND_STRUCT_METHOD(st, previewActive, MARGS());
    BIND_STRUCT_METHOD(st, commitPreviewDab, MARGS());
    BIND_STRUCT_METHOD(st, setNeighborMode, MARGS("mode"));
    BIND_STRUCT_METHOD(st, setNonAccum, MARGS("nonAccum"));
    BIND_STRUCT_METHOD(st, setGrabAccumAdd, MARGS("add"));
    BIND_STRUCT_METHOD(st, setStrokeGen, MARGS("gen"));
    BIND_STRUCT_METHOD(st, lastUniformValidationOk, MARGS());
    BIND_STRUCT_METHOD(st, queryUniformManifest, MARGS("brushType"));
    BIND_STRUCT_METHOD(st, queriedUniformEntry, MARGS("idx"));
    BIND_STRUCT_METHOD(st, clearUniformDynamics, MARGS("idx"));
    BIND_STRUCT_METHOD(
        st, addUniformDynamic, MARGS("idx", "deviceType", "mixMode", "mixFactor"));
    BIND_STRUCT_METHOD(
        st, setUniformDynamicSample, MARGS("idx", "deviceType", "i", "n", "value"));
    BIND_STRUCT_METHOD(st, setRenderMatrix, MARGS("m16"));

    return st;
  }

  /** Whether the most recent stroke's uniform-dynamics validation passed (Wave 4).
   * The bridge reads this after the first dab to surface a skipped stroke. */
  bool lastUniformValidationOk() const
  {
    return lastValidation.ok;
  }

  CommandExecutor(SpatialTree *tree, Brush *brush) : tree(tree), brush(brush), ctx()
  {
  }

  /** Set `ctx.renderMatrix` (ViewPlane/ViewRepeat texture UV) from 16 flat
   * floats, in the same element order as the debug app's `set_render_matrix`
   * verb. Bound-Vector arg = the marshal-safe bridge seam. Wrong size is a
   * no-op. */
  void setRenderMatrix(Vector<float> &m16)
  {
    if (m16.size() != 16) {
      return;
    }
    float *dst = &ctx.renderMatrix[0][0];
    for (int i = 0; i < 16; i++) {
      dst[i] = m16[i];
    }
  }

  /** Select the SMOOTH for_neighbor source: 0 = LiveDisk (live topology links),
   * 1 = Csr (the cached ring1 adjacency). The LiteMesh sculpt path uses Csr —
   * a freshly built mesh doesn't maintain live disk links, so LiveDisk smooth
   * finds no neighbors and no-ops. Exposed as an int (NeighborMode is an
   * unbound enum). */
  void setNeighborMode(int mode)
  {
    neighborMode = static_cast<NeighborMode>(mode);
  }

  /** Enable non-accumulate mode for the upcoming stroke, and set its generation
   * stamp (a monotonic per-stroke counter; must be non-zero, since the
   * `.brush.orig.gen` attr defaults to 0 = "not stamped this stroke"). */
  void setNonAccum(bool v)
  {
    nonAccum = v;
  }

  /** Mark the upcoming grab-class dab/image (#35): false = primary image, which
   * begins a new logical dab and bumps the per-dab generation so every vert it
   * touches re-bases from orig; true = mirror image of the same dab (shared verts
   * add). The AccumOrigGrab write-back keys off the per-vert dab stamp, so this
   * only needs to advance `dabGen` once per dab (on the primary image). */
  void setGrabAccumAdd(bool v)
  {
    grabAccumAdd = v;
    if (!v) {
      dabGen++;
    }
  }
  void setStrokeGen(int gen)
  {
    strokeGen = uint32_t(gen);
  }

  /** Per-call iterator factories used by CommandCtx::vertexIter/faceIter. The
   * vertex iterator is parameterized by the AccumMode policy and threaded the
   * stroke-start cache (null/0 unless non-accumulate is active for this dab). */
  template <class AccMode>
  BasicVertexIter<AccMode> makeVertexIter(spatial::SpatialNode &node)
  {
    return BasicVertexIter<AccMode>(node, *this, ctx.origCo, ctx.origGen, ctx.strokeGen);
  }
  BasicFaceIter makeFaceIter(spatial::SpatialNode &node)
  {
    return BasicFaceIter(node, *this);
  }

  /** Fill `def` for `brushType` under a fixed AccumMode policy. createCommand
   * calls this once for AccumLive, then again for AccumOrig when non-accumulate
   * is active and the brush is accumulable — the second call overwrites def.exec
   * with the AccumOrig kernel while keeping the rest of the (identical) manifest. */
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
      if (effectiveNeighborMode() == NeighborMode::Csr) {
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
      if (effectiveNeighborMode() == NeighborMode::Csr) {
        command::createBsmoothBrush<CommandExecutor, CsrNbr, AccMode>(def);
      } else {
        command::createBsmoothBrush<CommandExecutor, LiveDiskNbr, AccMode>(def);
      }
      return;
    case SculptBrushes::COLORSMOOTH:
      if (effectiveNeighborMode() == NeighborMode::Csr) {
        command::createColorsmoothBrush<CommandExecutor, CsrNbr, AccMode>(def);
      } else {
        command::createColorsmoothBrush<CommandExecutor, LiveDiskNbr, AccMode>(def);
      }
      return;
    case SculptBrushes::FEATURE_ALIGN:
      // Always live-disk: the C++ cross-field pre-pass (updateCrossFieldRegion)
      // walks the vertex disk, so topology is thawed regardless of neighborMode.
      command::createFeaturealignBrush<CommandExecutor, LiveDiskNbr, AccMode>(def);
      return;
    case SculptBrushes::LAYERDRAW:
      command::createLayerdrawBrush<CommandExecutor, AccMode>(def);
      return;
    case SculptBrushes::ENHANCE:
      // Not a for_neighbor kernel: it applies the host pre-pass's cached
      // .brush.enhance.disp, so no neighbor-source template.
      command::createEnhanceBrush<CommandExecutor, AccMode>(def);
      return;
    default:
      // Extra (out-of-repo) kernels dispatch through the generated registry;
      // a no-op fallback compiles in when no extra kernel dirs are configured.
      if (command::createExtraBrush<CommandExecutor, AccMode>(
              int(brushType), effectiveNeighborMode() == NeighborMode::Csr, *brush, def)) {
        return;
      }
      printf("Unknown brush type %d\n", static_cast<int>(brushType));
      abort();
    }
  }

  /** Grab-class brushes (grab / kelvinlet) deform a fixed region from the
   * stroke-start position; they always use the from-original policy regardless
   * of the ACCUMULATE flag. Snakehook is intentionally excluded (its per-dab
   * drag-plus-gather IS the desired behavior). #35. */
  static bool isGrabBrush(SculptBrushes brushType)
  {
    return brushType == SculptBrushes::GRAB || brushType == SculptBrushes::KELVINLET;
  }

  brush_command createCommand(SculptBrushes brushType)
  {
    brush_command def;
    createCommandImpl<AccumLive>(brushType, def);
    if (isGrabBrush(brushType)) {
      // Always deform from each vert's stroke-start position, so the region is
      // fixed at stroke start and the grab follows the cursor (#35). One write-
      // back (AccumOrigGrab) serves every symmetry image: the first image to
      // touch a vert this dab re-bases it from orig, later images of the same
      // dab add their displacement onto it (arbitrated by the per-vert dab
      // stamp). Forced on regardless of the ACCUMULATE flag / @global (kelvinlet
      // is @global → not `accumulable`). The op marks each image via
      // setGrabAccumAdd, which advances the per-dab generation on the primary.
      def.grabMode = true;
      /* The second impl call re-appends the same uniform manifest — clear it
         first or grab-class brushes report every uniform twice (wave-5
         queryUniformManifest, the TS dynamics UI). */
      def.uniforms = decltype(def.uniforms)();
      createCommandImpl<AccumOrigGrab>(brushType, def);
    } else if (nonAccum && def.accumulable) {
      def.uniforms = decltype(def.uniforms)();
      createCommandImpl<AccumOrig>(brushType, def);
    }
    return def;
  }

  /** Map a declared attribute domain to the mesh's element AttrGroup. */
  static mesh::AttrGroup *attrGroupForDomain(mesh::Mesh *m, AttrElemDomain d)
  {
    switch (d) {
    case AttrElemDomain::Vertex:
      return &m->v.attrs;
    case AttrElemDomain::Face:
      return &m->f.attrs;
    case AttrElemDomain::Edge:
      return &m->e.attrs;
    case AttrElemDomain::Corner:
      return &m->c.attrs;
    }
    return nullptr;
  }

  static int elemCountForDomain(mesh::Mesh *m, AttrElemDomain d)
  {
    switch (d) {
    case AttrElemDomain::Vertex:
      return m->v.count;
    case AttrElemDomain::Face:
      return m->f.count;
    case AttrElemDomain::Edge:
      return m->e.count;
    case AttrElemDomain::Corner:
      return m->c.count;
    }
    return 0;
  }

  /** Per-domain element *capacity*. Use this (not the count) when iterating by raw
   * element id — dyntopo leaves freelist gaps, so a live element's id can exceed
   * the live count. */
  static int elemCapacityForDomain(mesh::Mesh *m, AttrElemDomain d)
  {
    switch (d) {
    case AttrElemDomain::Vertex:
      return int(m->v.capacity());
    case AttrElemDomain::Face:
      return int(m->f.capacity());
    case AttrElemDomain::Edge:
      return int(m->e.capacity());
    case AttrElemDomain::Corner:
      return int(m->c.capacity());
    }
    return 0;
  }

  void exec(brush_command &cmd,
            std::span<spatial::SpatialNode *> nodes,
            std::span<const BrushAttrLayerOverride> attrOverrides = {})
  {
    // Resolve declared attribute layers once per dab (shared across all nodes;
    // the AttrData pointers are mesh-wide and stable for the dab's duration).
    if (attrOverrides.size() == 0 && defaultAttrOverrides.size() > 0) {
      attrOverrides = std::span<const BrushAttrLayerOverride>(
          defaultAttrOverrides.data(), defaultAttrOverrides.size());
    }
    attrBindingStorage.clear();
    ctx.attrBindings = nullptr;
    if (cmd.attrs.size() > 0 && nodes.size() > 0) {
      mesh::Mesh *m = nodes[0]->data->m;
      for (int ai = 0; ai < int(cmd.attrs.size()); ai++) {
        auto &entry = cmd.attrs[ai];
        mesh::AttrGroup *grp = attrGroupForDomain(m, entry.domain);
        if (!grp)
          continue;

        // An override redirects this handle to the user-selected "active"
        // layer (by index). Honour it only when the layer exists and its type
        // matches the handle's declared type (the TS attribute manager already
        // constrains categories by type, so a mismatch means a stale index —
        // fall through to the default by-name binding rather than corrupt the
        // wrong-typed layer).
        int ovLayer = -1;
        for (const auto &ov : attrOverrides) {
          if (ov.attrIdx == ai) {
            ovLayer = ov.layerIndex;
            break;
          }
        }
        if (ovLayer >= 0 && ovLayer < int(grp->attrs.size()) &&
            grp->attrs[ovLayer].type == entry.type)
        {
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
              for (int i = 0; i < n; i++)
                bv->set(i, false);
            } else {
              auto *dd = static_cast<mesh::AttrData<T> *>(ref.data);
              for (int i = 0; i < n; i++)
                dd->set_default(i);
            }
          });
        }
        attrBindingStorage.items.append(BrushAttrBinding{entry.handle, ref});
      }
      ctx.attrBindings = &attrBindingStorage;
    }

    // Sculpt-layer bracket: snapshot every written SCULPT_LAYER binding over
    // the dab region so the post-dab end() folds the kernel's delta edits into
    // evaluated v.co (displace compositor; frozen layers get reverted).
    layerScopes.clear();
    if (ctx.attrBindings && nodes.size() > 0) {
      mesh::Mesh *m = nodes[0]->data->m;
      layerRegionVerts.clear();
      for (auto &binding : attrBindingStorage.items) {
        if (!(binding.ref.use & mesh::AttrUse::SCULPT_LAYER)) {
          continue;
        }
        bool writes = false;
        for (auto &entry : cmd.attrs) {
          if (entry.handle == binding.handle && entry.write) {
            writes = true;
            break;
          }
        }
        if (!writes) {
          continue;
        }
        if (layerRegionVerts.size() == 0) {
          // Leaf unique_verts sets are disjoint, so no dedup is needed.
          for (auto *node : nodes) {
            for (int v : node->data->unique_verts) {
              layerRegionVerts.append(v);
            }
          }
        }
        displace::LayerEditScope scope;
        if (scope.begin(*m,
                        binding.ref.name,
                        std::span<const int>(layerRegionVerts.data(),
                                             layerRegionVerts.size())))
        {
          layerScopes.append(std::move(scope));
        }
      }
    }

    if (cmd.execHost) {
      cmd.execHost(ctx, *brush);
    }

    /* Capture walk-elision: execPre captures EVERY element of every node it is
     * handed (falloff-independent), so on a topology-stable stroke a leaf that
     * was already walked for this (stroke, sub-command slot) has nothing left
     * to capture — skip it wholesale instead of re-checking its per-element
     * stamps. Leaf element sets only change under dyntopo (splits/merges are
     * driven by dyntopo adds/collapses), which disables the stamps. */
    const bool stampCapture = ctx.meshLog && !stepHasDyntopo && curCaptureSlot >= 0 &&
                              curCaptureSlot < spatial::SpatialNode::MAX_CAPTURE_SLOTS;
    std::span<spatial::SpatialNode *> captureSpan(nodes.data(), nodes.size());
    int stampSid = 0;
    if (stampCapture) {
      stampSid = ctx.meshLog->curStrokeId() + 1;
      captureNodes_.clear();
      for (auto *node : nodes) {
        auto &st = node->captureStamps[curCaptureSlot];
        if (st.sid != stampSid || st.tool != curCaptureTool) {
          captureNodes_.append(node);
        }
      }
      captureSpan =
          std::span<spatial::SpatialNode *>(captureNodes_.data(), captureNodes_.size());
    }
    cmd.execPre(ctx, captureSpan);
    if (stampCapture) {
      for (auto *node : captureNodes_) {
        auto &st = node->captureStamps[curCaptureSlot];
        st.sid = stampSid;
        st.tool = curCaptureTool;
      }
    }

    // Jacobi snapshot: capture pre-dab vertex positions so for_neighbor reads
    // a consistent state regardless of the parallel node loop's interleaving.
    // Indexed by RAW vertex id (so are co[] and the CSR neighbor ids), so it must
    // span the full capacity, not v.count: dyntopo leaves freelist gaps, so a live
    // vertex's id can exceed v.count — a count-sized snapshot would be read
    // out-of-bounds for those verts/neighbors (garbage → smooth spikes).
    if (cmd.needsCoPrev && nodes.size() > 0) {
      mesh::Mesh *m = nodes[0]->data->m;
      int cap = int(m->v.capacity());

      /* Incremental refresh: on a topology-stable stroke, kernels only move
       * verts of the node sets they were handed — so after the stroke's first
       * full snapshot, entries can only be stale for nodes some exec touched
       * since the last refresh (coPrevDirty_). Neighbor reads outside every
       * touched set are still current from the full copy. */
      const bool canTrack = !stepHasDyntopo;
      if (canTrack && coPrevFull_ && int(coPrevStorage.size()) == cap) {
        litestl::task::parallel_for(
            util::IndexRange(coPrevDirty_.size()),
            [&](IndexRange range) {
              for (int i : range) {
                for (int v : coPrevDirty_[i]->data->unique_verts) {
                  coPrevStorage[v] = m->v.co[v];
                }
              }
            },
            1);
      } else {
        coPrevStorage.resize(cap);
        /* Full snapshot, copied page-wise: memcpy per materialized page
         * instead of the paged per-element operator[]. */
        mesh::AttrData<float3> *cod = m->v.co.get_data();
        float3 *dst = coPrevStorage.data();
        int copied = 0;
        for (auto &page : cod->pages) {
          int n = std::min(int(ATTR_PAGESIZE), cap - copied);
          if (n <= 0) {
            break;
          }
          if (page.data) {
            std::memcpy(static_cast<void *>(dst + copied),
                        static_cast<const void *>(page.data),
                        size_t(n) * sizeof(float3));
          } else {
            for (int i = 0; i < n; i++) {
              dst[copied + i] = page.value;
            }
          }
          copied += n;
        }
        coPrevFull_ = canTrack;
      }
      coPrevGen_++;
      coPrevDirty_.clear();
      ctx.co_prev = &coPrevStorage;

      // CSR neighbor source is static across the stroke — (re)build once,
      // single-threaded, before the parallel node loop reads it.
      if (effectiveNeighborMode() == NeighborMode::Csr) {
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
    ctx.dabGen = nullptr;
    ctx.curDabGen = 0;
    // Grab-class brushes always need the orig snapshot (cmd.grabMode), even when
    // not `accumulable` (kelvinlet is @global) and regardless of the ACCUMULATE
    // flag — they deform from it via AccumOrigGrab (#35).
    if ((cmd.grabMode || (nonAccum && cmd.accumulable)) && nodes.size() > 0) {
      mesh::Mesh *m = nodes[0]->data->m;
      // TEMP + NOCOPY: stroke-transient, not undoable. NOCOPY keeps the meshlog
      // from snapshotting these during a logged dyntopo step — their pages are
      // materialized lazily (only brushed verts), so a mid-stroke edge collapse
      // would otherwise capture an unmaterialized page (null on WASM → warn+skip;
      // a garbage pointer on native → crash, ImmediateTODOs #37). They're still
      // interpolated onto split verts (no NOINTERP) for non-accumulate accuracy.
      mesh::AttrRef &coRef =
          m->v.attrs.ensure(mesh::AttrType::FLOAT3, ".brush.orig.co", false);
      coRef.flag |= mesh::AttrFlag::TEMP | mesh::AttrFlag::NOCOPY;
      mesh::AttrRef &genRef =
          m->v.attrs.ensure(mesh::AttrType::INT, ".brush.orig.gen", false);
      genRef.flag |= mesh::AttrFlag::TEMP | mesh::AttrFlag::NOCOPY;
      ctx.origCo = static_cast<mesh::AttrData<float3> *>(coRef.data);
      ctx.origGen = static_cast<mesh::AttrData<int> *>(genRef.data);
      ctx.strokeGen = strokeGen;

      // Grab-class first-touch stamp (#35): ensure `.brush.dab.gen` + pass the
      // per-dab counter to the kernel. Pages are pre-materialized below (with the
      // orig stamp) so the parallel kernel only reads/writes existing slots.
      if (cmd.grabMode) {
        mesh::AttrRef &dabRef =
            m->v.attrs.ensure(mesh::AttrType::INT, ".brush.dab.gen", false);
        dabRef.flag |= mesh::AttrFlag::TEMP | mesh::AttrFlag::NOCOPY;
        ctx.dabGen = static_cast<mesh::AttrData<int> *>(dabRef.data);
        ctx.curDabGen = dabGen;
      }

      for (auto *node : nodes) {
        for (int v : node->data->unique_verts) {
          if (ctx.dabGen) {
            ctx.dabGen->materialize(v);
          }
          ctx.origGen->materialize(v);
          if ((*ctx.origGen)[v] != int(strokeGen)) {
            ctx.origCo->materialize(v);
            (*ctx.origCo)[v] = m->v.co[v];
            (*ctx.origGen)[v] = int(strokeGen);
          }
        }
      }
    }

    // Cavity automasking pre-fill (documentation/plans/2026-07-14-2007-cavity-
    // automasking.md): compute each in-region vert's 0..1 cavity factor once per
    // stroke (keyed by strokeGen) into the `.brush.automask.cavity` TEMP attr,
    // single-threaded before the parallel kernel loop reads it via
    // CommandCtx::strength. Freshly split verts (dyntopo) miss the stamp and
    // refill on first touch. The ring1 CSR the BFS walks is ensured live in
    // execProgram before the per-dab freeze; here it is a stamp-keyed no-op.
    ctx.automaskCavity = nullptr;
    ctx.automaskEnabled = false;
    if (brush->automask_cavity && nodes.size() > 0) {
      mesh::Mesh *m = nodes[0]->data->m;
      if (!m->topo_frozen || m->topo_cache.valid(*m)) {
        m->topo_cache.ensureRing1(*m);
      }
      if (m->topo_cache.valid(*m)) {
        mesh::AttrRef &cavRef =
            m->v.attrs.ensure(mesh::AttrType::FLOAT, ".brush.automask.cavity", false);
        cavRef.flag |= mesh::AttrFlag::TEMP | mesh::AttrFlag::NOCOPY;
        mesh::AttrRef &genRef =
            m->v.attrs.ensure(mesh::AttrType::INT, ".brush.automask.gen", false);
        genRef.flag |= mesh::AttrFlag::TEMP | mesh::AttrFlag::NOCOPY;
        auto *cav = static_cast<mesh::AttrData<float> *>(cavRef.data);
        auto *gen = static_cast<mesh::AttrData<int> *>(genRef.data);

        static_assert(int(Brush::kCavityCurveLutSize) == kCavityCurveSize,
                      "brush cavity_curve LUT size must match automask kCavityCurveSize");
        CavityParams cp;
        cp.enabled = true;
        cp.blur_steps = brush->cavity_blur_steps;
        cp.factor = brush->cavity_factor;
        cp.inverted = brush->cavity_inverted;
        cp.use_curve = brush->cavity_use_curve;
        cp.curve_lut = brush->cavity_curve.data();

        CavityScratch scr;
        for (auto *node : nodes) {
          for (int v : node->data->unique_verts) {
            gen->materialize(v);
            cav->materialize(v);
            if (strokeGen == 0 || (*gen)[v] != int(strokeGen)) {
              (*cav)[v] = cavityFactor(m, v, cp, scr);
              (*gen)[v] = int(strokeGen);
            }
          }
        }
        ctx.automaskCavity = cav;
        ctx.automaskEnabled = true;
      }
    }

    // Skip leaves emptied of verts: heavy in-stroke collapse can leave a zero-vert
    // leaf whose loose AABB still overlaps the brush sphere, so a vertex iterator on
    // it wild-reads unique_verts.begin() (brush_iterators.h: "never on empty node").
#ifdef NO_PARALLEL_FOR
    for (auto *node : nodes) {
      if (node->data->unique_verts.size() == 0) {
        continue;
      }
      CommandCtx<CommandExecutor> finalCtx(ctx, *node, *this, *brush);
      cmd.exec(finalCtx);
    }
#else
    litestl::task::parallel_for(
        util::IndexRange(nodes.size()),
        [&](IndexRange range) {
          for (int i : range) {
            SpatialNode *node = nodes[i];
            if (node->data->unique_verts.size() == 0) {
              continue;
            }
            CommandCtx<CommandExecutor> finalCtx(ctx, *node, *this, *brush);
            cmd.exec(finalCtx);
          }
        },
        4);
#endif

    cmd.execPost(ctx, nodes);

    // Fold sculpt-layer edits into evaluated positions (the kernel already
    // flagged the touched nodes Spatial_UpdateNormals|UpdateGPU|RegenBounds).
    for (auto &scope : layerScopes) {
      scope.end();
    }
    layerScopes.clear();

    // Border propagation: kernels flag only the node whose own verts moved, but
    // neighbouring leaves' tris draw replicas of (and integrate normals over) a
    // moved border vert — flag those too, appending the vert as a normals hint.
    {
      Set<int> movedVerts;
      for (auto *node : nodes) {
        for (int v : node->affected_verts) {
          movedVerts.add(v);
        }
      }
      if (movedVerts.size() > 0 && nodes.size() > 1) {
        mesh::Mesh *mm = nodes[0]->data->m;
        for (auto *node : nodes) {
          if (node->flag & spatial::Spatial_RegenTris) {
            continue; // tris are stale; the pending full regen covers this leaf
          }
          bool touched = false;
          for (auto &tri : node->data->tris) {
            for (int j = 0; j < 3; j++) {
              int v = mm->c.v[tri.c[j]];
              if (tree->treeMesh.v.node[v] != node->id && movedVerts.contains(v)) {
                node->affected_verts.append(v);
                touched = true;
              }
            }
          }
          if (touched) {
            node->update(spatial::Spatial_UpdateNormals | spatial::Spatial_UpdateGPUGeom |
                         spatial::Spatial_RegenBounds);
          }
        }
      }
    }

    /* coPrev bookkeeping: every vert this exec (kernel, execPost, layer fold)
     * may have moved lives in `nodes` — queue them for the next needsCoPrev
     * refresh. Skipped under dyntopo (node pointers/element sets unstable;
     * the refresh falls back to a full snapshot there anyway). */
    if (!stepHasDyntopo) {
      for (auto *node : nodes) {
        if (node->coPrevStamp != coPrevGen_) {
          node->coPrevStamp = coPrevGen_;
          coPrevDirty_.append(node);
        }
      }
    }

    /* UV slide-reprojection: re-anchor the dab's moved verts' UVs on their
     * pre-move ring. Rides the coPrev snapshot, so only needsCoPrev kernels
     * (the smooth family) qualify. Dyntopo strokes keep topology live —
     * reproject per dab (its ring changes under the stroke); frozen strokes
     * accumulate {vert -> first-seen pre-move position} and endStep flushes
     * once (a per-dab thaw would perturb the frozen-CSR stroke state). */
    if (brush && brush->reproject_uvs && cmd.needsCoPrev && nodes.size() > 0 &&
        ctx.co_prev == &coPrevStorage)
    {
      if (stepHasDyntopo) {
        mesh::Mesh *m = nodes[0]->data->m;
        Vector<int> rverts;
        Vector<float3> rold;
        Set<int, 64> seen;
        for (auto *node : nodes) {
          for (int v : node->affected_verts) {
            if (v >= 0 && v < int(coPrevStorage.size()) && seen.add(v)) {
              rverts.append(v);
              rold.append(coPrevStorage[v]);
            }
          }
        }
        if (rverts.size() > 0) {
          reprojectUvsWithCapture(
              m, std::span<const int>(rverts.data(), rverts.size()),
              std::span<const float3>(rold.data(), rold.size()));
        }
      } else {
        for (auto *node : nodes) {
          for (int v : node->affected_verts) {
            if (v >= 0 && v < int(coPrevStorage.size())) {
              uvReprojPending_.add(v, coPrevStorage[v]); /* first-seen wins */
            }
          }
        }
      }
    }
  }

  /** Run reprojectVertUVs with per-corner undo capture into the meshlog's
   * CORNER element store (the same AttrSaver-gated append-as-touched pattern
   * the generated kernels use for vertex positions), so a stroke's UV edits
   * revert with it. Requires live topology; thaws if needed. */
  void reprojectUvsWithCapture(mesh::Mesh *m,
                               std::span<const int> verts,
                               std::span<const float3> oldCo)
  {
    if (m->topo_frozen) {
      m->thawTopo();
    }
    mesh::MeshCallbacks cb;
    meshlog::AttrSaver<mesh::ElemType::CORNER> saver;
    meshlog::LogChunkElems *store = nullptr;
    Vector<mesh::AttrRef, 4> refs;
    int sid = 0, mask = 0;
    if (meshLog) {
      for (mesh::AttrRef &attr : m->c.attrs.attrs) {
        if (attr.type == mesh::AttrType::FLOAT2 && attr.data &&
            (int(attr.use) & int(mesh::AttrUse::UV)) != 0)
        {
          refs.append(attr);
        }
      }
      if (refs.size() > 0) {
        saver.ensure(*m);
        store = meshLog->elemStore(mesh::ElemType::CORNER);
        int bit = meshlog::CUSTOM_START;
        for (mesh::AttrRef &ref : refs) {
          store->data.ensureAttr(m->c.attrs, ref);
          mask |= saver.add(ref, 1 << bit);
          bit++;
        }
        sid = meshLog->curStrokeId();
      }
    }
    cb.onCornerChange = [&](int c) {
      if (store && saver.needsData(c, sid, mask)) {
        const int row = store->data.appendRows(1);
        store->data.cpyFrom(m->c.attrs, c, row);
        saver.updateSaved(c, sid, mask);
      }
      // Refill the face owner's GPU attribute streams: the deform kernels only
      // flag Spatial_UpdateGPUGeom, which skips them — without this the
      // viewport keeps drawing the pre-reprojection UVs.
      if (tree) {
        const int ni = tree->treeMesh.f.node[m->l.f[m->c.l[c]]];
        if (ni) {
          tree->node_from_id(ni)->update(spatial::NodeFlags::Spatial_UpdateGPU);
        }
      }
    };
    mesh::uvproj::reprojectVertUVs(m, verts, oldCo, &cb);
  }

  /** SMOOTH is the only brush with a for_neighbor loop, and only its CSR
   * instantiation reads neighbors from the cache rather than the live disk.
   * Every other brush (and CSR-mode smooth) touches no live TOPO link during a
   * dab, so the mesh can sit topology-frozen — dropping the link pages — for
   * the whole stroke. A live-disk smooth dab is the lone case that needs the
   * links back. */
  bool brushNeedsLiveLinks(SculptBrushes brushType) const
  {
    // Face-stage brushes walk the face loop (live links) to compute centroids.
    return ((brushType == SculptBrushes::SMOOTH || brushType == SculptBrushes::BSMOOTH ||
             brushType == SculptBrushes::COLORSMOOTH) &&
            neighborMode != NeighborMode::Csr) ||
           brushType == SculptBrushes::POLYGROUP ||
           // The cross-field / enhance-details pre-passes walk the vertex rings
           // (ring1 CSR) every dab, so keep topology live for those strokes.
           brushType == SculptBrushes::FEATURE_ALIGN ||
           brushType == SculptBrushes::ENHANCE ||
           // Extra kernels with for_neighbor loops mirror the SMOOTH rule.
           (extraBrushUsesForNeighbor(int(brushType)) && neighborMode != NeighborMode::Csr);
  }

  /** The boundary-aware smooth brush reads the lazily-derived
   * `.boundary.vert.class`. Fold any pending boundary edits (seam marking,
   * poly-group paint) into it once at stroke start, while topology links are
   * live — recomputeDirty walks the disk/radial cycles and would touch freed
   * pages under frozen topology.
   *
   * Gated on m->boundaryDirty: when nothing changed since the last recompute
   * (the common case — e.g. plain smoothing with no boundaries marked) this is a
   * no-op and, crucially, does NOT thaw. An unconditional thaw here perturbs the
   * frozen-topology CSR neighbor set the stroke relies on, making bsmooth
   * diverge from plain smooth even with zero boundaries. */
  void refreshBoundaryClassForBSmooth(mesh::Mesh *m)
  {
    if (!m->boundaryDirty)
      return;
    if (m->topo_frozen)
      m->thawTopo();
    mesh::boundary::recomputeDirty(m);
  }

  /** After a poly-group dab, mark every face the touched nodes own boundary-dirty
   * so the next recomputeDirty reclassifies their inter-group edges. A superset
   * of the actually-repainted faces (bounded by the dab's node coverage), which
   * only costs extra recompute, never wrong results. Runs while topology is live
   * (POLYGROUP is a live-links brush). */
  void markPolygroupDirty(std::span<spatial::SpatialNode *> nodes)
  {
    for (spatial::SpatialNode *node : nodes) {
      mesh::Mesh *m = node->data->m;
      for (int f : node->data->unique_faces) {
        mesh::boundary::markFaceDirty(m, f);
      }
    }
  }

  /** The fixed common float props are valid dynamics targets for any brush (the
   * bridge drives strength/radius/... by pressure regardless of the active
   * kernel), so they're exempt from the active-manifest membership check. */
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
      if (u.name == name)
        return &u;
    }
    return nullptr;
  }

  /** Validate the active brush's uniform dynamics once at stroke start, scoped to
   * its manifest. Catches the shared-`Brush`-struct traps (a stray dynamic left
   * over from another kernel, a dynamic on a `@static`/non-float uniform), an
   * unbaked 1-entry response curve, an out-of-range authored default, and an
   * inverted/NaN `@range`. Returns a structured result; the caller skips the
   * stroke on `!ok` so a misconfigured binding never mutates the mesh. */
  UniformValidationResult validateUniformDynamics(brush_command &cmd)
  {
    UniformValidationResult res;
    char buf[256];

    // (A) Static manifest checks — independent of any configured dynamic.
    for (const auto &u : cmd.uniforms) {
      if (!u.hasRange)
        continue;
      if (std::isnan(u.rangeMin) || std::isnan(u.rangeMax) || u.rangeMin > u.rangeMax) {
        res.ok = false;
        snprintf(buf,
                 sizeof(buf),
                 "uniform '%s': invalid @range [%g, %g]",
                 u.name.c_str(),
                 u.rangeMin,
                 u.rangeMax);
        res.messages.append(string(buf));
        continue; // a broken range makes the default check meaningless
      }
      if (u.isFloat && (u.def < u.rangeMin || u.def > u.rangeMax)) {
        res.ok = false;
        snprintf(buf,
                 sizeof(buf),
                 "uniform '%s': default %g outside @range [%g, %g]",
                 u.name.c_str(),
                 u.def,
                 u.rangeMin,
                 u.rangeMax);
        res.messages.append(string(buf));
      }
    }

    // (B) Dynamics checks — every prop carrying a configured device stack must
    // be a valid, dynamic-capable target of the active brush.
    if (brush && brush->props.struct_def) {
      for (props::Property *p : brush->props.struct_def->properties()) {
        props::Dynamics *dyn = brush->propDynamics(p->name);
        if (!dyn || dyn->devices.size() == 0)
          continue;

        const BrushUniformManifestEntry *entry = findUniformEntry(cmd, p->name);
        if (!entry && !isCommonFloatProp(p->name)) {
          res.ok = false;
          snprintf(buf,
                   sizeof(buf),
                   "stray dynamic on '%s': not a uniform of the active brush",
                   p->name.c_str());
          res.messages.append(string(buf));
          continue;
        }
        if (entry && !(entry->isFloat && entry->dynamic)) {
          res.ok = false;
          snprintf(buf,
                   sizeof(buf),
                   "dynamic on '%s': uniform is @static / non-float (not "
                   "dynamic-capable)",
                   p->name.c_str());
          res.messages.append(string(buf));
          continue;
        }
        for (const auto &dev : dyn->devices) {
          if (dev.curveTable.size() == 1) {
            res.ok = false;
            snprintf(buf,
                     sizeof(buf),
                     "uniform '%s': device response curve has 1 entry "
                     "(unbaked; need 0 or >=2)",
                     p->name.c_str());
            res.messages.append(string(buf));
          }
          int dt = (int)dev.type;
          if (dt < 0 || dt > (int)props::DeviceType::TWIST) {
            res.ok = false;
            snprintf(buf,
                     sizeof(buf),
                     "uniform '%s': invalid device type %d",
                     p->name.c_str(),
                     dt);
            res.messages.append(string(buf));
          }
        }
      }
    }

    return res;
  }

  /** --- Wave 5: per-kernel uniform manifest query for the TS bridge -----------
   * The binding runtime can't marshal a JS string into a `util::string` method
   * arg, so the bridge enumerates the active brush's manifest by index instead
   * of by name. queryUniformManifest caches the kernel's manifest (and registers
   * its props so propDynamics(name) resolves) and returns the entry count;
   * queriedUniformEntry exposes each entry as a bound read-only struct; the
   * *UniformDynamics methods resolve the index -> name and delegate to the Brush
   * by-name dynamics API (Wave 3). */

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
    brush->addPropDynamicByName(
        queriedUniforms[idx].name, deviceType, mixMode, mixFactor);
  }
  void setUniformDynamicSample(int idx, int deviceType, int i, int n, float value)
  {
    if (!brush || idx < 0 || idx >= int(queriedUniforms.size())) {
      return;
    }
    brush->setPropDynamicSampleByName(queriedUniforms[idx].name, deviceType, i, n, value);
  }

  // Update the per-dab stroke frame: the stroke tangent (this dab origin minus
  // the previous dab center) and, for the oriented Box falloff, its primary
  // axis. Must run *before* pushStrokeSample appends this origin, and before the
  // kernel executes. Shared by both the single-brush and program dab paths so
  // Box/wing-scrape orientation is identical regardless of entry point.
  void updateStrokeFrame(float3 origin)
  {
    // Under mirror symmetry the host owns strokeDir (it reflects the primary
    // tangent per image); the shared ring buffer would otherwise interleave
    // primary + mirror origins. Derive from the buffer only when host didn't.
    if (!brush->strokeDirHostSet && brush->strokePathCount > 0) {
      float3 d = origin - brush->strokePath[brush->strokePathCount - 1].pos;
      float len = d.length();
      if (len > 1e-7f) {
        brush->strokeDir = d / len;
      }
    }
    // The bridge only flips the shape to Box; the direction is owned here so it
    // stays consistent with wing-scrape's strokeDir.
    if (brush->falloff_shape == FalloffShape::Box) {
      brush->falloff_dir = brush->strokeDir;
    }
  }

  void execBrush(Mesh *m,
                 SculptBrushes brushType,
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
      if (brushType == SculptBrushes::BSMOOTH && isFirstOfStep) {
        refreshBoundaryClassForBSmooth(m);
      }
      if (brushNeedsLiveLinks(brushType) || keepTopoThawed) {
        if (m->topo_frozen)
          m->thawTopo();
      } else if (!m->topo_frozen) {
        m->freezeTopo();
      }
    }

    // Enhance-details pre-pass (single-tool path; mirrors the execProgram block):
    // fill the cached per-vertex difference-of-smooths displacement over the dab
    // region before the ENHANCE kernel reads .brush.enhance.disp. Topology is live
    // here (brushNeedsLiveLinks(ENHANCE)).
    if (brushType == SculptBrushes::ENHANCE && nodes->size() > 0) {
      Vector<int> regionVerts;
      for (spatial::SpatialNode *node : *nodes) {
        for (int v : node->data->unique_verts) {
          regionVerts.append(v);
        }
      }
      EnhanceParams ep;
      ep.rings = brush->enhance_rings;
      ep.inner = brush->enhance_inner;
      updateEnhanceRegion(*m, regionVerts, ep, strokeGen);
    }

    // Resolve common props with device dynamics applied (a bit-identical no-op
    // without configured devices; mirrors the execProgram load). No
    // loadUniformProps: this path's callers set kernel uniforms as raw fields.
    brush->loadCommonProps(&brush->deviceInputCtx);

    ctx.m = m;
    ctx.surfaceNo = normal;
    ctx.surfacePos = origin;
    ctx.meshLog = meshLog;
    ctx.isFirstOfStep = isFirstOfStep;

    // Stroke tangent + oriented-Box axis for this dab (before pushStrokeSample).
    updateStrokeFrame(origin);

    // Record this dab center so STROKE_CURVED can map vertices onto the
    // accumulated stroke polyline. Incremental on purpose: a dab's vertices
    // see the path up to and including this dab.
    brush->pushStrokeSample(origin, normal);

    std::span<spatial::SpatialNode *> nodeSpan(nodes->data(), nodes->size());
    curCaptureSlot = 0;
    curCaptureTool = int(brushType);
    exec(cmd, nodeSpan);
    curCaptureSlot = -1;

    if (brushType == SculptBrushes::POLYGROUP) {
      markPolygroupDirty(nodeSpan);
    }
  }

  /** Run a composite brush program over one node set per dab. Each sub-command
   * resolves the brush's props (with its sparse overrides applied) into the
   * cached scalars, then runs like a standalone brush. Used for autosmooth
   * (`[main, SMOOTH]`): SMOOTH is a second `exec()` whose `co_prev` snapshot is
   * re-taken *after* the main pass mutated positions, so it smooths the result. */
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
          for (auto &msg : r.messages)
            lastValidation.messages.append(msg);
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
        if (brushNeedsLiveLinks(entry.type))
          needsLive = true;
        // BSMOOTH and FEATURE_ALIGN both read the lazily-derived
        // `.boundary.vert.class`, so it must be refreshed at stroke start.
        if (entry.type == SculptBrushes::BSMOOTH ||
            entry.type == SculptBrushes::FEATURE_ALIGN)
          hasBSmooth = true;
      }
      mesh::Mesh *m = (*nodes)[0]->data->m;
      // Must precede the freeze below — recomputeDirty needs live links.
      if (hasBSmooth && isFirstOfStep) {
        refreshBoundaryClassForBSmooth(m);
      }
      // Cavity automasking's BFS blur reads the ring1 CSR. Build it here, while
      // topology links are live, before the per-dab freeze drops them: thaw a
      // stale frozen mesh (topology changed since the CSR was built), then
      // (re)build the stamp-keyed CSR (a no-op when already current). exec()'s
      // fill then reads the cached CSR even after the mesh re-freezes below.
      if (brush->automask_cavity) {
        if (m->topo_frozen && !m->topo_cache.valid(*m)) {
          m->thawTopo();
        }
        m->topo_cache.ensureRing1(*m);
      }
      if (needsLive || keepTopoThawed) {
        if (m->topo_frozen)
          m->thawTopo();
      } else if (!m->topo_frozen) {
        m->freezeTopo();
      }
    }

    // Feature-align cross-field maintenance: before the FEATURE_ALIGN command
    // runs, (re)seed + diffuse the per-vertex cross field over this dab's region
    // so the kernel reads an up-to-date field. Topology is live here
    // (brushNeedsLiveLinks(FEATURE_ALIGN)). Incremental — only the region's
    // verts are written, so the saved field grows as the stroke covers the mesh.
    if (nodes->size() > 0) {
      bool hasFeatureAlign = false;
      for (auto &entry : prog->commands) {
        if (entry.type == SculptBrushes::FEATURE_ALIGN) {
          hasFeatureAlign = true;
          break;
        }
      }
      if (hasFeatureAlign) {
        Vector<int> regionVerts;
        for (spatial::SpatialNode *node : *nodes) {
          for (int v : node->data->unique_verts) {
            regionVerts.append(v);
          }
        }
        FeatureFieldParams ffParams;
        updateCrossFieldRegion(*(*nodes)[0]->data->m, regionVerts, ffParams);
      }
    }

    // Enhance-details pre-pass: fill the per-vertex difference-of-smooths
    // displacement (.brush.enhance.disp) over the dab region before the ENHANCE
    // kernel reads it. Cached per stroke (keyed by strokeGen); topology is live
    // here (brushNeedsLiveLinks(ENHANCE)).
    if (nodes->size() > 0) {
      bool hasEnhance = false;
      for (auto &entry : prog->commands) {
        if (entry.type == SculptBrushes::ENHANCE) {
          hasEnhance = true;
          break;
        }
      }
      if (hasEnhance) {
        Vector<int> regionVerts;
        for (spatial::SpatialNode *node : *nodes) {
          for (int v : node->data->unique_verts) {
            regionVerts.append(v);
          }
        }
        EnhanceParams ep;
        ep.rings = brush->enhance_rings;
        ep.inner = brush->enhance_inner;
        updateEnhanceRegion(*(*nodes)[0]->data->m, regionVerts, ep, strokeGen);
      }
    }

    // Stroke tangent + oriented-Box axis for this dab (Route A). Must run before
    // pushStrokeSample appends this origin. Drives wing-scrape and Box falloff.
    updateStrokeFrame(origin);

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
        util::string nm =
            ov.name.size() ? ov.name : util::string(brushPropName(ov.propId));
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
      curCaptureSlot = int(&entry - prog->commands.data());
      curCaptureTool = int(entry.type);
      exec(cmd,
           nodeSpan,
           std::span<const BrushAttrLayerOverride>(entry.attrLayerOverrides.data(),
                                                   entry.attrLayerOverrides.size()));
      curCaptureSlot = -1;

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

  /** Run one dynamic-topology dab under the cursor. Reproduces the native
   * debug harness's Scene::applyDynTopoDab wiring (thaw + combined meshlog/
   * spatial callbacks + in-region seed) MINUS tree->update() and the meshlog
   * step: the TS sculpt path already drives spatial.update() each frame
   * (LiteMesh.drawQ) and wraps the whole stroke in one meshlog step. Returns
   * splits+collapses applied (DynTopoStats.splits + .collapses). */
  int applyDynTopoDab(float3 center,
                      float radius,
                      dyntopo::DynTopoParams *params,
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
        if (mlFC)
          mlFC(f);
        if (spFC)
          spFC(f);
      };
      auto mlFK = combined.onFaceKill, spFK = sp->onFaceKill;
      combined.onFaceKill = [mlFK, spFK](int f) {
        if (mlFK)
          mlFK(f); /* meshlog snapshots before the tree drops it */
        if (spFK)
          spFK(f);
      };
      auto mlVK = combined.onVertKill, spVK = sp->onVertKill;
      combined.onVertKill = [mlVK, spVK](int v) {
        if (mlVK)
          mlVK(v);
        if (spVK)
          spVK(v);
      };
      auto mlFCh = combined.onFaceChange, spFCh = sp->onFaceChange;
      combined.onFaceChange = [mlFCh, spFCh](int f) {
        if (mlFCh)
          mlFCh(f); /* meshlog records the rewired (Existed && Live) face */
        if (spFCh)
          spFCh(f); /* tree re-flags the owning leaf (in-place flip/split) */
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

    // #37: the non-accumulate `.brush.orig.*` snapshot is materialized lazily —
    // only verts a *prior* dab brushed. Once the column exists, dyntopo's attr
    // interpolation (split midpoints, collapse blend) on THIS dab's seed region
    // would read pages the brush never stamped (e.g. a symmetry-mirror dab's
    // region, #38) — null on WASM, a garbage deref + crash on native. The deform
    // stamps the region, but only AFTER this pre-pass. Stamp the seed region here
    // (first contact = current co) so every vert dyntopo touches has a live page;
    // the deform's stamp loop then skips them (gen already == strokeGen).
    if (m->v.attrs.has(mesh::AttrType::FLOAT3, ".brush.orig.co") &&
        m->v.attrs.has(mesh::AttrType::INT, ".brush.orig.gen")) {
      auto *origCo =
          m->v.attrs.find_attribute(mesh::AttrType::FLOAT3, ".brush.orig.co").get_data<float3>();
      auto *origGen =
          m->v.attrs.find_attribute(mesh::AttrType::INT, ".brush.orig.gen").get_data<int>();
      for (int v : seedVerts) {
        origGen->materialize(v);
        if ((*origGen)[v] != int(strokeGen)) {
          origCo->materialize(v);
          (*origCo)[v] = m->v.co[v];
          (*origGen)[v] = int(strokeGen);
        }
      }
    }

    dyntopo::DynTopoStats st =
        dyntopo::runDyntopoRemesh(*m,
                                  center,
                                  radius,
                                  *params,
                                  seed,
                                  cb,
                                  span<const int>(seedVerts.data(), seedVerts.size()));

    lastDynTopoStats = st;
    return st.splits + st.collapses;
  }

  /** One unified brush dab — the single dab sequence shared by every client
   * (TS app, debug interactive/script). Runs, in order: optional dyntopo pre-pass
   * (when params != nullptr), spatial node filter, deform program, then the
   * per-dab meshlog topo-chunk seal. Dyntopo runs BEFORE the deform so the brush
   * moves the freshly-refined geometry; the chunk is sealed AFTER the deform so a
   * Created vert's end_body captures its deformed co directly (endStep still
   * refreshes verts a LATER dab re-deforms without a topo touch). Caller owns
   * building `prog` and configuring `*params`; pass params=nullptr to disable
   * dyntopo. Call between beginStep(hasDyntopo) and endStep() (and
   * endDynTopoStroke() before endStep() on a dyntopo stroke). Returns
   * splits+collapses applied. */
  int applyDab(BrushProgram *prog,
               float3 center,
               float3 normal,
               float radius,
               dyntopo::DynTopoParams *params,
               uint32_t seed)
  {
    if (!tree || !tree->m) {
      return 0;
    }
    int topoApplied = 0;
    if (params) {
      topoApplied = applyDynTopoDab(center, radius, params, seed);
    }
    // `radius` here is the node-filter radius only (the falloff uses
    // brush->radius). Grab-class strokes pass a radius widened by the cumulative
    // drag so the deformed region's leaves stay in the set and can't shrink +
    // tear at leaf seams (#35); the caller (the dab dispatch) does the widening.
    Vector<spatial::SpatialNode *> nodes;
    tree->filterNodes(center, radius, nodes);
    if (nodes.size() > 0) {
      execProgram(prog, &nodes, center, normal);
    }
    if (params && meshLog) {
      /* Per-dab seal: deactivate this dab's topo chunk NOW (finalizing its
         Created end-states) so the next dab's topo captures land in a fresh
         chunk. Without it one step-wide chunk captures an Existed vert's
         begin-body at its FIRST TOPO touch — mid-step for verts the brush
         deformed in earlier dabs — and that stale body, sitting at position
         0, wins the reverse-order undo over the element store's true
         pre-step row (the multistep undo corruption). */
      meshLog->pushTopoChunk();
    }
    clearIsFirstOfStep();
    return topoApplied;
  }

  /** execBrush-based overload for callers driving a single brush type rather than
   * a BrushProgram (the native debug harness). Same dyntopo+filter+seal sequence. */
  int applyDab(SculptBrushes brushType,
               float3 center,
               float3 normal,
               float radius,
               dyntopo::DynTopoParams *params,
               uint32_t seed)
  {
    if (!tree || !tree->m) {
      return 0;
    }
    mesh::Mesh *m = tree->m;
    int topoApplied = 0;
    if (params) {
      topoApplied = applyDynTopoDab(center, radius, params, seed);
    }
    // `radius` is the node-filter radius only (caller widens it for grab-class
    // strokes; see the BrushProgram overload). #35
    Vector<spatial::SpatialNode *> nodes;
    tree->filterNodes(center, radius, nodes);
    if (nodes.size() > 0) {
      execBrush(m, brushType, &nodes, center, normal);
    }
    if (params && meshLog) {
      /* Per-dab seal: deactivate this dab's topo chunk NOW (finalizing its
         Created end-states) so the next dab's topo captures land in a fresh
         chunk. Without it one step-wide chunk captures an Existed vert's
         begin-body at its FIRST TOPO touch — mid-step for verts the brush
         deformed in earlier dabs — and that stale body, sitting at position
         0, wins the reverse-order undo over the element store's true
         pre-step row (the multistep undo corruption). */
      meshLog->pushTopoChunk();
    }
    clearIsFirstOfStep();
    return topoApplied;
  }

  /** Release the stroke-long topology thaw set by applyDynTopoDab. Call once at
   * stroke end; the next non-dyntopo dab re-freezes on its own. Also folds in the
   * final dab's pending boundary marks (the per-dab recompute runs at the *next*
   * dab's start, so the last dab's new geometry would otherwise stay unclassified
   * until the next stroke) while topology links are still live. */
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

  /** Anchored/Drag Dot live preview: snapshot the region the next preview-only
   * applyDab() is about to touch (see MeshLog::beginPreviewDab). Call
   * immediately before that dab (the primary/first dab of a driver tick; use
   * extendPreviewDab() for any symmetry mirror images of the same tick).
   * Roll the whole group back with rollbackPreviewDab() before the next
   * tick's dabs, or keep it with commitPreviewDab() at stroke end. */
  void beginPreviewDab(float3 center, float radius)
  {
    if (!meshLog || !tree) {
      return;
    }
    meshLog->beginPreviewDab(tree->m, tree, center, radius);
  }

  /** Add another region to the currently-open preview session (a mirror
   * image of the same driver tick) without resetting it — the whole group
   * rolls back together via one rollbackPreviewDab(). See
   * MeshLog::extendPreviewDab. */
  void extendPreviewDab(float3 center, float radius)
  {
    if (!meshLog || !tree) {
      return;
    }
    meshLog->extendPreviewDab(tree->m, tree, center, radius);
  }

  /** Undo the most recent preview dab (topology pop-and-undo + position/attr
   * restore) without closing the step. See MeshLog::rollbackPreviewDab. */
  void rollbackPreviewDab()
  {
    if (!meshLog || !tree) {
      return;
    }
    meshLog->rollbackPreviewDab(tree->m, tree);
  }

  /** True if a preview snapshot is pending rollback (diagnostic / debug-app use). */
  bool previewActive() const
  {
    return meshLog && meshLog->previewActive();
  }

  /** Keep the pending preview dab's effect and drop its snapshot bookkeeping.
   * Call once at the true end of an Anchored/Drag Dot stroke (after the last
   * dab, instead of a paired rollback) so previewActive() doesn't leak into
   * the next stroke's step. See MeshLog::commitPreviewDab. */
  void commitPreviewDab()
  {
    if (!meshLog) {
      return;
    }
    meshLog->commitPreviewDab();
  }

  void beginStep(bool hasDyntopo)
  {
    isFirstOfStep = true;
    strokeValidationFailed = false;
    stepHasDyntopo = hasDyntopo;
    /* Positions may have changed since the last stroke (undo, ops, other
     * tools) — force the stroke's first needsCoPrev exec to take a full
     * snapshot. The gen bump keeps stale node stamps from suppressing
     * dirty-list appends. */
    coPrevFull_ = false;
    coPrevDirty_.clear();
    coPrevGen_++;
    uvReprojPending_.clear();
    if (brush) {
      brush->resetStrokePath();
    }
    if (meshLog) {
      meshLog->beginStep(hasDyntopo);
    }
  }

  void endStep()
  {
    isFirstOfStep = false;
    /* Flush the stroke's deferred UV reprojection while the step is still
     * open, so the corner captures land inside it. */
    if (uvReprojPending_.size() > 0 && tree && tree->m) {
      Vector<int> rverts;
      Vector<float3> rold;
      for (const auto &pair : uvReprojPending_) {
        rverts.append(pair.key);
        rold.append(pair.value);
      }
      reprojectUvsWithCapture(tree->m,
                              std::span<const int>(rverts.data(), rverts.size()),
                              std::span<const float3>(rold.data(), rold.size()));
    }
    uvReprojPending_.clear();
    if (meshLog) {
      meshLog->endStep();
    }
  }
};

/** Declared in accum_mode.h; AccumKind::Grab write-back uses it. First image to
 * write vert `v` this dab → stamp curDabGen and return true (re-base from orig);
 * an already-stamped vert returns false (later image adds). The page was
 * pre-materialized in exec(), so this only reads/writes an existing slot. With no
 * stamp attr (single-image stroke, dab counter idle) every write re-bases. */
inline bool grabClaimFirstTouch(const CommandExecutor &exec, int v)
{
  mesh::AttrData<int> *dabGen = exec.ctx.dabGen;
  if (!dabGen) {
    return true;
  }
  if ((*dabGen)[v] == int(exec.ctx.curDabGen)) {
    return false;
  }
  (*dabGen)[v] = int(exec.ctx.curDabGen);
  return true;
}
} // namespace sculptcore::brush