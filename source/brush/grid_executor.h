#pragma once

/** Grids-native brush executor (grids-native brush path, G2).
 *
 * Runs the same generated `.sbrush` kernels the mesh CommandExecutor runs,
 * instantiated against a multires GridLevelDomain instead of a materialized
 * mesh::Mesh: the spatial unit is a GridTree leaf (whole grids, owned-vert
 * lists), positions/normals/mask are the domain's dense buffers, neighbors
 * come from the domain's lattice CSR, and undo capture goes through
 * GridStrokeLog block snapshots instead of the meshlog.
 *
 * Deliberately NOT a generalization of CommandExecutor: no dyntopo, no
 * meshlog, no attr overrides, no preview machinery — just the vertex-stage
 * roster (draw, grab, smooth, inflate, kelvinlet, pinch, sharp, the plane
 * family, mask). Unsupported brushes fall back to the materialized path; the
 * dispatch rule is supportsBrush(), engine-owned metadata.
 *
 * Stroke shape: beginStep() → applyDab()* → endStep(). endStep folds the
 * stroke into the grids store via Multires::gridsWriteback, restricted to the
 * touched verts' occurrence grids — O(region), not O(level). */

#include "accum_mode.h"
#include "automask.h"
#include "brush_command.h"
#include "brush_iterators.h"
#include "brushes/all.h"
#include "capture_policy.h"

#include "subdiv/grid_domain.h"
#include "subdiv/grid_stroke_log.h"
#include "subdiv/grid_tree.h"
#include "subdiv/multires.h"

#include "litestl/util/assert.h"
#include "litestl/util/task.h"

#include <chrono>
#include <cstdlib>

namespace sculptcore::brush {

struct GridBrushExecutor;

/** The grid executor's spatial unit — one GridTree leaf plus the per-stroke
 * scratch generated kernels expect on `ctx.node`. */
struct GridExecNode {
  GridBrushExecutor *exec = nullptr;
  int leaf = -1;
  Vector<int> affected_verts;
  spatial::NodeFlags flag = spatial::Spatial_None;
  /** From-base stamp walk-elision, mirroring SpatialNode's (topology is
   * always stable here, so it is always eligible). */
  uint32_t baseStampGen = 0;
  int baseStampOpts = -1;

  void update(spatial::NodeFlags f)
  {
    flag |= f;
  }
};

/** for_neighbor source over the domain's lattice 1-ring CSR. Both grids
 * backends (CPU and GPU) share this one adjacency, so their accumulation
 * order matches — unlike the mesh path's edge-cycle order. */
struct GridCsrNbr {
  static constexpr bool is_neighbor_source = true;

  template <class Ctx> static litestl::util::span<const int> range(Ctx &ctx, int v)
  {
    subdiv::GridLevelDomain *d = ctx.executor.domain;
    int off = d->ring1Offsets[v];
    return litestl::util::span<const int>(d->ring1.data() + off,
                                          d->ring1Offsets[v + 1] - off);
  }
};

/** Undo-capture policy: first-touch leaf block snapshots into GridStrokeLog.
 * `No` saves are ignored (normals are derived state, refreshed after a seek);
 * face-domain saves likewise (no face state exists on the grids domain). */
struct GridCapturePolicy {
  template <mesh::ElemType Domain>
  static void capture(CommandCtxBase &ctx,
                      std::span<GridExecNode *> nodes,
                      std::span<const CaptureSaveDesc> saves);
};

/** Per-vertex iteration over a leaf's owned-vert list, binding the domain's
 * dense buffers — the grids counterpart of BasicVertexIter, sharing
 * CoProxy<AccMode, GridBrushExecutor> so AccumOrig/AccumOrigGrab work. */
template <class AccMode> struct GridVertexIter {
  struct PtrHelper {
    CoProxy<AccMode, GridBrushExecutor> co;
    float3 &no;
    float &mask;
    int v;
    int indexInNode = 0;

    GridBrushExecutor *exec;

    PtrHelper(float3 &co_, float3 base_, mesh::AttrData<float3> *disp_, float3 &no_,
              float &mask_, int v, GridBrushExecutor *exec)
        : co{co_, base_, exec, disp_, v}, no(no_), mask(mask_), v(v), exec(exec)
    {
    }
    PtrHelper(const PtrHelper &b)
        : co(b.co), no(b.no), mask(b.mask), v(b.v), indexInNode(b.indexInNode),
          exec(b.exec)
    {
    }
  };

  subdiv::GridLevelDomain *d;
  const Vector<int> *verts;
  GridBrushExecutor *exec;
  mesh::AttrData<float3> *dispVec;
  mesh::AttrData<int> *dispGen;
  uint32_t strokeGen;
  int idx = 0;
  PtrHelper ptrs;

  float3 baseFor(int v)
  {
    if constexpr (AccMode::reads_base) {
      if (dispVec && dispGen->safe_get(v) == int(strokeGen)) {
        return d->pos()[v] - dispVec->safe_get(v);
      }
    }
    return d->pos()[v];
  }

  mesh::AttrData<float3> *dispFor()
  {
    if constexpr (AccMode::reads_base) {
      return dispVec;
    }
    return nullptr;
  }

  /** Never create on a leaf with no owned verts (mirrors BasicVertexIter). */
  GridVertexIter(subdiv::GridLevelDomain *d,
                 const Vector<int> *verts,
                 GridBrushExecutor *exec,
                 mesh::AttrData<float3> *dispVec,
                 mesh::AttrData<int> *dispGen,
                 uint32_t strokeGen)
      : d(d), verts(verts), exec(exec), dispVec(dispVec), dispGen(dispGen),
        strokeGen(strokeGen),
        ptrs(d->pos()[(*verts)[0]], baseFor((*verts)[0]), dispFor(),
             d->no[(*verts)[0]], d->mask[(*verts)[0]], (*verts)[0], exec)
  {
  }

  GridVertexIter(const GridVertexIter &b)
      : d(b.d), verts(b.verts), exec(b.exec), dispVec(b.dispVec), dispGen(b.dispGen),
        strokeGen(b.strokeGen), idx(b.idx), ptrs(b.ptrs)
  {
  }

  bool operator==(const GridVertexIter &b)
  {
    return idx == b.idx;
  }
  bool operator!=(const GridVertexIter &b)
  {
    return idx != b.idx;
  }

  PtrHelper &operator*()
  {
    return ptrs;
  }

  GridVertexIter &operator++()
  {
    idx++;
    if (idx < int(verts->size())) {
      int v = (*verts)[idx];
      ptrs.~PtrHelper();
      new (&ptrs)
          PtrHelper(d->pos()[v], baseFor(v), dispFor(), d->no[v], d->mask[v], v, exec);
      ptrs.indexInNode = idx;
    }
    return *this;
  }

  GridVertexIter begin()
  {
    GridVertexIter it(*this);
    it.idx = 0;
    return it;
  }

  GridVertexIter end()
  {
    GridVertexIter it(*this);
    it.idx = int(verts->size());
    return it;
  }
};

struct GridBrushExecutor {
  using vertex_iter = GridVertexIter<AccumLive>;
  using vertex_iter_factory = std::function<vertex_iter(GridExecNode &)>;
  /** Face-stage kernels are never instantiated for grids; the typedef only
   * satisfies the CommandTypes concept. */
  using face_iter = BasicFaceIter;
  using face_iter_factory = std::function<face_iter(spatial::SpatialNode &)>;
  using node_type = GridExecNode;
  using capture_policy = GridCapturePolicy;
  using brush_command = BrushCommandDef<CommandCtx<GridBrushExecutor>>;

  Brush *brush = nullptr;
  subdiv::GridLevelDomain *domain = nullptr;
  subdiv::GridTree *tree = nullptr;
  /** Optional undo log; null disables capture. */
  subdiv::GridStrokeLog *log = nullptr;
  CommandCtxBase ctx;

  bool isFirstOfStep = false;
  /** Non-accumulate + anchored-grab stroke policy, mirroring CommandExecutor. */
  bool nonAccum = false;
  bool anchoredGrab = true;
  uint32_t dabGen = 0;
  uint32_t strokeGen = 0;

  /** Accumulated per-phase wall time (ms) + counters, for the G3 perf gates.
   * Always collected — a handful of steady_clock reads per dab. */
  struct Stats {
    double queryMs = 0, captureMs = 0, coPrevMs = 0, stampMs = 0, automaskMs = 0,
           kernelMs = 0, normalsMs = 0, boundsMs = 0, writebackMs = 0;
    int dabs = 0, strokes = 0;
    void reset()
    {
      *this = Stats();
    }
    /** The plan's per-dab gate scope: kernel + capture + bounds (+ the normal
     * refresh, which the grids path pays per dab where the mesh path pays it
     * per frame — report both). */
    double perDabCoreMs() const
    {
      return dabs > 0 ? (kernelMs + captureMs + boundsMs) / double(dabs) : 0.0;
    }
    double perDabTotalMs() const
    {
      return dabs > 0 ? (queryMs + captureMs + coPrevMs + stampMs + automaskMs +
                         kernelMs + normalsMs + boundsMs) /
                            double(dabs)
                      : 0.0;
    }
  };
  Stats stats;

  GridBrushExecutor(subdiv::GridLevelDomain *d,
                    Brush *b,
                    subdiv::GridStrokeLog *lg = nullptr)
  {
    brush = b;
    log = lg;
    attach(d);
  }

  /** (Re)bind to a domain — required after any Multires fold point drops it.
   * Rebuilds the leaf-node array and the domain-sized stroke sidecars. */
  void attach(subdiv::GridLevelDomain *d)
  {
    domain = d;
    tree = d->ensureTree();
    int vc = d->vertCount();
    nodes_.clear();
    nodes_.resize(tree->leaves.size());
    for (int i = 0; i < int(nodes_.size()); i++) {
      nodes_[i].exec = this;
      nodes_[i].leaf = i;
    }
    dispVec_ = mesh::AttrData<float3>(string(".grid.disp.vec"), vc);
    dispGen_ = mesh::AttrData<int>(string(".grid.disp.gen"), vc);
    dabGen_ = mesh::AttrData<int>(string(".grid.dab.gen"), vc);
    cavity_ = mesh::AttrData<float>(string(".grid.automask.cavity"), vc);
    cavityGen_ = mesh::AttrData<int>(string(".grid.automask.gen"), vc);
    touchedStamp_.resize(vc);
    dabStamp_.resize(vc);
    for (int i = 0; i < vc; i++) {
      touchedStamp_[i] = 0;
      dabStamp_[i] = 0;
    }
    leafTouched_.resize(nodes_.size());
    for (int i = 0; i < int(leafTouched_.size()); i++) {
      leafTouched_[i] = 0;
    }
    if (log) {
      log->attach(d);
    }
  }

  // === kernel-facing seams (the domain half of the TYPES contract) ===

  template <class AccMode> GridVertexIter<AccMode> makeVertexIter(GridExecNode &node)
  {
    return GridVertexIter<AccMode>(domain, &tree->leaves[node.leaf].ownedVerts, this,
                                   ctx.dispVec, ctx.dispGen, ctx.strokeGen);
  }

  BasicFaceIter makeFaceIter(spatial::SpatialNode & /*node*/)
  {
    // Never reached: face-stage kernels are not in the grids roster.
    abort();
  }

  template <class Ctx> static float3 &nbrNo(Ctx &ctx, int v)
  {
    return ctx.executor.domain->no[v];
  }

  template <class Ctx> const float3 *liveVertNoPtr(Ctx & /*ctx*/, int v) const
  {
    return &domain->no[v];
  }

  // === command creation (mirrors CommandExecutor's AccumMode policy) ===

  template <class AccMode>
  static bool createCommandSwitch(SculptBrushes brushType, brush_command &def)
  {
    switch (brushType) {
    case SculptBrushes::DRAW:
      command::createDrawBrush<GridBrushExecutor, AccMode>(def);
      return true;
    case SculptBrushes::INFLATE:
      command::createInflateBrush<GridBrushExecutor, AccMode>(def);
      return true;
    case SculptBrushes::CLAY:
    case SculptBrushes::SCRAPE:
    case SculptBrushes::FILL:
      command::createPlaneBrush<GridBrushExecutor, AccMode>(def);
      return true;
    case SculptBrushes::PINCH:
      command::createPinchBrush<GridBrushExecutor, AccMode>(def);
      return true;
    case SculptBrushes::SHARP:
      command::createSharpBrush<GridBrushExecutor, AccMode>(def);
      return true;
    case SculptBrushes::MASK:
      command::createMaskBrush<GridBrushExecutor, AccMode>(def);
      return true;
    case SculptBrushes::SMOOTH:
      command::createSmoothBrush<GridBrushExecutor, GridCsrNbr, AccMode>(def);
      return true;
    case SculptBrushes::KELVINLET:
      command::createKelvinletBrush<GridBrushExecutor, AccMode>(def);
      return true;
    case SculptBrushes::GRAB:
      command::createGrabBrush<GridBrushExecutor, AccMode>(def);
      return true;
    default:
      return false;
    }
  }

  /** Engine-owned dispatch rule: can this tool run grids-native? Everything
   * else falls back to the materialized-mesh path. */
  static bool supportsBrush(SculptBrushes brushType)
  {
    brush_command def;
    return createCommandSwitch<AccumLive>(brushType, def);
  }

  brush_command createCommand(SculptBrushes brushType)
  {
    brush_command def;
    bool ok = createCommandSwitch<AccumLive>(brushType, def);
    Assert(ok, "grid executor: unsupported brush (gate on supportsBrush)");
    (void)ok;
    if (def.grabModeCapable && anchoredGrab) {
      def.grabMode = true;
      def.uniforms = decltype(def.uniforms)();
      def.attrs = decltype(def.attrs)();
      createCommandSwitch<AccumOrigGrab>(brushType, def);
    } else if (nonAccum && def.accumulable && !def.relaxesBase) {
      def.uniforms = decltype(def.uniforms)();
      def.attrs = decltype(def.attrs)();
      createCommandSwitch<AccumOrig>(brushType, def);
    }
    return def;
  }

  // === stroke lifecycle ===

  void beginStep()
  {
    isFirstOfStep = true;
    strokeSeq_++;
    strokeGen = strokeSeq_;
    strokeTouchedVerts_.clear();
    strokeTouchedLeaves_.clear();
    for (int i = 0; i < int(leafTouched_.size()); i++) {
      leafTouched_[i] = 0;
    }
    strokeWroteCo_ = false;
    strokeWroteMask_ = false;
    grabPinned_ = false;
    grabLeaves_.clear();
    if (brush) {
      brush->resetStrokePath();
    }
    if (log) {
      log->beginStep();
    }
  }

  /** Mark the upcoming grab-class dab/image: false = primary (bumps the
   * per-dab first-touch generation), true = mirror image of the same dab. */
  void setGrabAccumAdd(bool add)
  {
    if (!add) {
      dabGen++;
    }
  }

  /** One dab of `brushType` at `origin`/`normal`. Returns moved-vert count. */
  int applyDab(SculptBrushes brushType, float3 origin, float3 normal)
  {
    auto cmd = createCommand(brushType);
    brush->loadCommonProps(&brush->deviceInputCtx);

    ctx.m = nullptr;
    ctx.meshLog = nullptr;
    ctx.surfacePos = origin;
    ctx.surfaceNo = normal;
    ctx.isFirstOfStep = isFirstOfStep;

    updateStrokeFrame(origin);
    brush->pushStrokeSample(origin, normal);

    if (cmd.writesMask) {
      strokeWroteMask_ = true;
      if (isFirstOfStep) {
        // The channel must exist before the log captures its blocks.
        domain->ensureMaskChannel();
      }
    } else {
      strokeWroteCo_ = true;
    }

    using clock = std::chrono::steady_clock;
    auto msSince = [](clock::time_point t0) {
      return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    };
    stats.dabs++;
    auto t0 = clock::now();

    // Node filter: the leaf set for this dab. Grab-class strokes pin their
    // first dab's set (the region is fixed at stroke start — the mesh path's
    // grabFilterNodes, without the dyntopo fallback).
    float floorR = cmd.unbounded ? brush->radius * brush->unboundedExtent : 0.0f;
    float r = std::fmax(brush->radius, floorR);
    dabLeaves_.clear();
    if (cmd.grabMode) {
      if (!grabPinned_) {
        tree->query(origin, r, grabLeaves_);
        grabPinned_ = true;
      }
      for (int li : grabLeaves_) {
        dabLeaves_.append(li);
      }
    } else {
      tree->query(origin, r, dabLeaves_);
    }
    if (dabLeaves_.size() == 0) {
      isFirstOfStep = false;
      return 0;
    }
    nodePtrs_.clear();
    for (int li : dabLeaves_) {
      nodePtrs_.append(&nodes_[li]);
    }
    std::span<GridExecNode *> nodeSpan(nodePtrs_.data(), nodePtrs_.size());
    stats.queryMs += msSince(t0);
    t0 = clock::now();

    // Undo capture (first-touch leaf snapshots, driven by the kernel's
    // `save` descriptors through GridCapturePolicy).
    cmd.execPre(ctx, nodeSpan);
    stats.captureMs += msSince(t0);
    t0 = clock::now();

    // Jacobi snapshot for for_neighbor kernels: the domain's dense positions,
    // full copy (the mesh path pays the same; restricting is a follow-up).
    ctx.co_prev = nullptr;
    if (cmd.needsCoPrev) {
      coPrevStorage_.resize(domain->vertCount());
      const auto &pos = domain->pos();
      task::parallel_for(util::IndexRange(size_t(domain->vertCount())),
                         [&](util::IndexRange range) {
                           for (int i : range) {
                             coPrevStorage_[i] = pos[i];
                           }
                         });
      ctx.co_prev = &coPrevStorage_;
    }
    stats.coPrevMs += msSince(t0);
    t0 = clock::now();

    // From-base stroke state (`.grid.disp.*`), mirroring the mesh executor's
    // stampBase walk with the same leaf-level elision.
    ctx.origNo = nullptr;
    ctx.dispVec = nullptr;
    ctx.dispGen = nullptr;
    ctx.strokeGen = 0;
    ctx.dabGen = nullptr;
    ctx.curDabGen = 0;
    Assert(!cmd.needsOrigNormals, "no grids-roster kernel opts into orig normals");
    if (cmd.grabMode || (nonAccum && cmd.accumulable && !cmd.relaxesBase)) {
      ctx.dispVec = &dispVec_;
      ctx.dispGen = &dispGen_;
      ctx.strokeGen = strokeGen;
      if (cmd.grabMode) {
        ctx.dabGen = &dabGen_;
        ctx.curDabGen = dabGen;
      }
      const int stampOpts = ctx.dabGen ? 2 : 0;
      for (GridExecNode *node : nodeSpan) {
        if (node->baseStampGen == strokeGen && node->baseStampOpts == stampOpts) {
          continue;
        }
        for (int v : tree->leaves[node->leaf].ownedVerts) {
          if (ctx.dabGen) {
            ctx.dabGen->materialize(v);
          }
          dispGen_.materialize(v);
          if (dispGen_[v] != int(strokeGen)) {
            dispVec_.materialize(v);
            dispVec_[v] = float3(0.0f, 0.0f, 0.0f);
            dispGen_[v] = int(strokeGen);
          }
        }
        node->baseStampGen = strokeGen;
        node->baseStampOpts = stampOpts;
      }
    }

    stats.stampMs += msSince(t0);
    t0 = clock::now();

    // Automasking: view-normal params are dynamic; cavity is cached per vert
    // per stroke over the domain CSR (the mesh path's contract).
    ctx.viewNormal = viewNormalParamsFor(*brush);
    ctx.automaskFactor = nullptr;
    ctx.automaskEnabled = false;
    if (brush->automask_cavity) {
      static_assert(int(Brush::kCavityCurveLutSize) == kCavityCurveSize,
                    "brush cavity_curve LUT size must match automask kCavityCurveSize");
      CavityParams cp;
      cp.enabled = true;
      cp.blur_steps = brush->cavity_blur_steps;
      cp.factor = brush->cavity_factor;
      cp.inverted = brush->cavity_inverted;
      cp.use_curve = brush->cavity_use_curve;
      cp.curve_lut = brush->cavity_curve.data();
      GridCavitySrc src{domain};
      CavityScratch scr;
      for (GridExecNode *node : nodeSpan) {
        for (int v : tree->leaves[node->leaf].ownedVerts) {
          cavityGen_.materialize(v);
          cavity_.materialize(v);
          if (cavityGen_[v] != int(strokeGen)) {
            cavity_[v] = cavityRemap(cp, cavityRawT(src, v, cp.blur_steps, scr));
            cavityGen_[v] = int(strokeGen);
          }
        }
      }
      ctx.automaskFactor = &cavity_;
      ctx.automaskEnabled = true;
    }

    stats.automaskMs += msSince(t0);
    t0 = clock::now();

    // The parallel kernel loop — leaves own disjoint vert sets, and
    // for_neighbor reads the Jacobi snapshot, so node order is free.
    task::parallel_for(util::IndexRange(nodePtrs_.size()), [&](util::IndexRange range) {
      for (int i : range) {
        GridExecNode *node = nodePtrs_[i];
        if (tree->leaves[node->leaf].ownedVerts.size() == 0) {
          continue;
        }
        CommandCtx<GridBrushExecutor> finalCtx(ctx, *node, *this, *brush);
        cmd.exec(finalCtx);
      }
    });

    cmd.execPost(ctx, nodeSpan);
    stats.kernelMs += msSince(t0);
    t0 = clock::now();

    // Consume the dab's results: refresh THIS dab's moved verts' normals
    // (+ their cell closure) and the touched leaves' bounds, and fold the
    // moved set into the stroke's touched accumulation. No border
    // propagation — dense ids have no replicas.
    dabSeq_++;
    dabMoved_.clear();
    for (GridExecNode *node : nodeSpan) {
      if (node->affected_verts.size() > 0 && !leafTouched_[node->leaf]) {
        leafTouched_[node->leaf] = 1;
        strokeTouchedLeaves_.append(node->leaf);
      }
      for (int v : node->affected_verts) {
        if (dabStamp_[v] != dabSeq_) {
          dabStamp_[v] = dabSeq_;
          dabMoved_.append(v);
          if (touchedStamp_[v] != strokeSeq_) {
            touchedStamp_[v] = strokeSeq_;
            strokeTouchedVerts_.append(v);
          }
        }
      }
      node->affected_verts.clear();
      node->flag = spatial::Spatial_None;
    }
    int moved = int(dabMoved_.size());
    if (moved > 0) {
      auto tn = clock::now();
      domain->refreshNormals(std::span<const int>(dabMoved_.data(), dabMoved_.size()));
      stats.normalsMs += msSince(tn);
      auto tb = clock::now();
      tree->refreshBounds(std::span<const int>(dabLeaves_.data(), dabLeaves_.size()));
      stats.boundsMs += msSince(tb);
    }

    isFirstOfStep = false;
    return moved;
  }

  /** Stroke end: fold the touched region into the grids store (restricted
   * writeback over the touched verts' occurrence grids), flush mask writes,
   * and close the undo step. */
  void endStep()
  {
    auto t0 = std::chrono::steady_clock::now();
    isFirstOfStep = false;
    stats.strokes++;
    subdiv::Multires *mr = domain->multires();
    int level = domain->level();
    if (strokeTouchedVerts_.size() > 0) {
      // The touched verts' occurrence grids — the restricted writeback walk,
      // and exactly the store blocks the undo log must capture first (the
      // store is untouched until this fold, so blocks defer to here).
      Vector<bool> changed;
      changed.resize(domain->vertCount());
      for (int i = 0; i < domain->vertCount(); i++) {
        changed[i] = false;
      }
      Vector<int> grids;
      gridScratch_.resize(domain->gridCount());
      for (int i = 0; i < domain->gridCount(); i++) {
        gridScratch_[i] = 0;
      }
      for (int v : strokeTouchedVerts_) {
        changed[v] = true;
        auto occs = domain->occurrences(v);
        for (size_t k = 0; k < occs.size(); k += 3) {
          if (!gridScratch_[occs[k]]) {
            gridScratch_[occs[k]] = 1;
            grids.append(occs[k]);
          }
        }
      }
      std::span<const int> gridSpan(grids.data(), grids.size());
      if (strokeWroteCo_) {
        if (log) {
          log->captureGrids(gridSpan, mr->writebackChannel());
        }
        mr->gridsWriteback(level, changed, grids);
      }
      if (strokeWroteMask_) {
        if (log) {
          log->captureGrids(gridSpan,
                            mr->store.findChannel(string("mask")));
        }
        domain->flushMaskToStore(std::span<const int>(strokeTouchedVerts_.data(),
                                                      strokeTouchedVerts_.size()));
      }
    }
    stats.writebackMs +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
            .count();
    if (log) {
      log->endStep(mr->downPropDebt(level));
    }
  }

  /** The stroke's accumulated moved-vert set (dense ids, deduped). */
  const Vector<int> &strokeTouchedVerts() const
  {
    return strokeTouchedVerts_;
  }
  const Vector<int> &strokeTouchedLeaves() const
  {
    return strokeTouchedLeaves_;
  }

  /** cavityRawT source over the domain's dense buffers + lattice CSR. */
  struct GridCavitySrc {
    subdiv::GridLevelDomain *d;
    int vertCap() const
    {
      return d->vertCount();
    }
    float3 co(int v) const
    {
      return d->pos()[v];
    }
    float3 no(int v) const
    {
      return d->no[v];
    }
    std::span<const int> neighbors(int v) const
    {
      return d->neighbors(v);
    }
  };

  /** Grab first-touch stamp storage, read by grabClaimFirstTouch. */
  mesh::AttrData<int> *grabDabGen() const
  {
    return ctx.dabGen;
  }
  uint32_t grabCurDabGen() const
  {
    return ctx.curDabGen;
  }

private:
  /** CommandExecutor::updateStrokeFrame, verbatim (Brush-only state). */
  void updateStrokeFrame(float3 origin)
  {
    if (!brush->strokeDirHostSet && brush->strokePathCount > 0) {
      float3 d = origin - brush->strokePath[brush->strokePathCount - 1].pos;
      float len = d.length();
      if (len > 1e-7f) {
        brush->strokeDir = d / len;
      }
    }
    if (brush->falloff_shape == FalloffShape::Box) {
      brush->falloff_dir = brush->strokeDir;
    }
  }

  Vector<GridExecNode> nodes_; // parallel to tree->leaves
  Vector<int> dabLeaves_;
  Vector<GridExecNode *> nodePtrs_;
  Vector<int> dabMoved_;
  Vector<uint32_t> dabStamp_;
  uint32_t dabSeq_ = 0;

  mesh::AttrData<float3> dispVec_{string(".grid.disp.vec"), 0};
  mesh::AttrData<int> dispGen_{string(".grid.disp.gen"), 0};
  mesh::AttrData<int> dabGen_{string(".grid.dab.gen"), 0};
  mesh::AttrData<float> cavity_{string(".grid.automask.cavity"), 0};
  mesh::AttrData<int> cavityGen_{string(".grid.automask.gen"), 0};
  Vector<float3> coPrevStorage_;

  Vector<uint32_t> touchedStamp_;
  uint32_t strokeSeq_ = 0;
  Vector<int> strokeTouchedVerts_;
  Vector<int> strokeTouchedLeaves_;
  Vector<uint8_t> leafTouched_;
  Vector<uint8_t> gridScratch_;
  bool strokeWroteCo_ = false;
  bool strokeWroteMask_ = false;

  bool grabPinned_ = false;
  Vector<int> grabLeaves_;
};

template <mesh::ElemType Domain>
inline void GridCapturePolicy::capture(CommandCtxBase & /*ctx*/,
                                       std::span<GridExecNode *> nodes,
                                       std::span<const CaptureSaveDesc> saves)
{
  if constexpr (Domain != mesh::ElemType::VERTEX) {
    return; // face saves are derived state on the grids domain
  } else {
    if (nodes.empty() || saves.empty()) {
      return;
    }
    GridBrushExecutor *ex = nodes[0]->exec;
    if (!ex || !ex->log) {
      return;
    }
    bool positions = false, maskToo = false;
    for (const CaptureSaveDesc &sv : saves) {
      positions |= sv.field == CaptureField::Co;
      maskToo |= sv.field == CaptureField::Mask;
      // No: derived (refreshed after a seek). Attr: not in the grids roster.
      Assert(sv.field != CaptureField::Attr, "attr saves need the materialized path");
    }
    if (!positions && !maskToo) {
      return;
    }
    for (GridExecNode *node : nodes) {
      ex->log->captureLeaf(node->leaf, positions, maskToo);
    }
  }
}

/** Grab-class first-touch arbitration over the grid executor's dab stamp —
 * the mesh version's twin (see brush_executor.h). */
inline bool grabClaimFirstTouch(const GridBrushExecutor &exec, int v)
{
  mesh::AttrData<int> *dabGen = exec.grabDabGen();
  if (!dabGen) {
    return true;
  }
  if ((*dabGen)[v] == int(exec.grabCurDabGen())) {
    return false;
  }
  (*dabGen)[v] = int(exec.grabCurDabGen());
  return true;
}

} // namespace sculptcore::brush
