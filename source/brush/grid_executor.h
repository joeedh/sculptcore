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

#include "spatial/spatial.h"
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

/** Stroke-end fold shared by the CPU executor and the GPU session: capture
 * the touched verts' occurrence-grid store blocks into the undo log FIRST
 * (the store is untouched until this fold), then the restricted writeback
 * (positions) and/or mask flush, then close the undo step. O(region). */
inline void gridsFoldStroke(subdiv::GridLevelDomain *domain,
                            subdiv::GridStrokeLog *log,
                            std::span<const int> touched,
                            bool wroteCo,
                            bool wroteMask)
{
  subdiv::Multires *mr = domain->multires();
  int level = domain->level();
  if (touched.size() > 0) {
    Vector<bool> changed;
    changed.resize(domain->vertCount());
    for (int i = 0; i < domain->vertCount(); i++) {
      changed[i] = false;
    }
    Vector<int> grids;
    Vector<uint8_t> gridStamp;
    gridStamp.resize(domain->gridCount());
    for (int i = 0; i < domain->gridCount(); i++) {
      gridStamp[i] = 0;
    }
    for (int v : touched) {
      changed[v] = true;
      auto occs = domain->occurrences(v);
      for (size_t k = 0; k < occs.size(); k += 3) {
        if (!gridStamp[occs[k]]) {
          gridStamp[occs[k]] = 1;
          grids.append(occs[k]);
        }
      }
    }
    std::span<const int> gridSpan(grids.data(), grids.size());
    if (wroteCo) {
      if (log) {
        log->captureGrids(gridSpan, mr->writebackChannel());
      }
      mr->gridsWriteback(level, changed, grids);
    }
    if (wroteMask) {
      if (log) {
        log->captureGrids(gridSpan, mr->store.findChannel(string("mask")));
      }
      domain->flushMaskToStore(touched);
    }
  }
  if (log) {
    log->endStep(mr->downPropDebt(level));
  }
}

/** Interim ride-along mirror (plan §6): copy `verts`' domain positions and
 * normals into the resident level slot's mesh (dense ids match) and dirty the
 * owning spatial leaves, so extdraw and mesh-path queries stay current while
 * the grids path does the real work. No-op when the slot is not resident.
 * The mirror is write-only — the domain stays authoritative. */
inline void gridsMirrorToSlot(subdiv::Multires *mr,
                              int level,
                              std::span<const int> verts)
{
  subdiv::MultiresSlot *slot = mr->findSlot(level);
  if (!slot || !slot->mesh || !slot->tree) {
    return;
  }
  subdiv::GridLevelDomain *d = mr->gridDomain(level);
  mesh::Mesh *m = slot->mesh;
  for (int v : verts) {
    m->v.co[v] = d->pos()[v];
    m->v.no[v] = d->no[v];
  }
  for (int v : verts) {
    int nid = slot->tree->treeMesh.v.node[v];
    spatial::SpatialNode *node = slot->tree->node_from_id(nid);
    if (!node) {
      continue;
    }
    // Geometry-only: positions/normals were copied above and topology never
    // changes — the same flags a kernel dab sets minus UpdateNormals.
    // RegenTris here would re-triangulate every touched leaf (and force a
    // GPU partition recompute) on each draw refresh.
    node->flag |= spatial::Spatial_UpdateGPUGeom | spatial::Spatial_RegenBounds;
    for (spatial::SpatialNode *p = node->parent;
         p && !(p->flag & spatial::Spatial_RegenBounds); p = p->parent)
    {
      p->flag |= spatial::Spatial_RegenBounds;
    }
  }
  // Callers mirror every diverged vert (stroke-end touched set, undo/redo
  // full sync), so the slot is current again — writeback may diff it.
  mr->clearSlotStale(level);
}

/** Pull the paint mask from the resident slot mesh's `.spatial.v.mask` column
 * into the domain's dense mirror — the mesh column is the host-side mask
 * truth (flood fills, CD_GRID_PAINT_MASK import land there), and grids-path
 * kernels read the mirror. No-op without a resident slot (the store-channel
 * mirror from the domain build stands) or when the column was never created
 * (maskless sessions must not pay an O(level) copy per stroke). */
inline void gridsSyncMaskFromSlot(subdiv::Multires *mr, int level)
{
  subdiv::MultiresSlot *slot = mr->findSlot(level);
  if (!slot || !slot->mesh || !slot->tree) {
    return;
  }
  mesh::AttrRef ref = slot->mesh->v.attrs.find_attribute(mesh::AttrType::FLOAT,
                                                         ".spatial.v.mask");
  if (!ref.exists() || !ref.data) {
    return;
  }
  subdiv::GridLevelDomain *d = mr->gridDomain(level);
  for (int v = 0; v < d->vertCount(); v++) {
    d->mask[v] = slot->tree->treeMesh.v.mask[v];
  }
}

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
  /** Defer the touched-set normal refresh to flushNormals() (host frame
   * cadence) instead of paying it per dab. Closely-spaced dabs overlap ~90%,
   * so per-dab refresh recomputes the same fans many times over — this is
   * the mesh path's (and native sculpt's) per-frame normal cadence. Bounds
   * still refresh per dab (raycast currency); kernels and raycast normals
   * read <=1-frame-stale values, exactly like the mesh path. Off by default
   * (the per-dab tests keep exact semantics); the addon opts in. */
  bool deferNormals = false;

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
    pendingNormalStamp_.resize(vc);
    for (int i = 0; i < vc; i++) {
      touchedStamp_[i] = 0;
      dabStamp_[i] = 0;
      pendingNormalStamp_[i] = 0;
    }
    pendingNormals_.clear();
    flushedNormals_.clear();
    // Drop the co_prev snapshot + stamps: a rebuild remaps dense ids, and the
    // lazy re-allocation in refreshCoPrevRegion re-zeros the stamps.
    coPrevStorage_.clear();
    coPrevStrokeGen_.clear();
    coPrevCopyEpoch_.clear();
    coPrevPosEpoch_.clear();
    // The vclass shim is all-zero by construction, so a rebuild only needs
    // the size to track the new domain (ensureVclassBinding resizes).
    vclass_.resize(0);
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
    case SculptBrushes::BSMOOTH:
      command::createBsmoothBrush<GridBrushExecutor, GridCsrNbr, AccMode>(def);
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

    // DSL attr manifest: grids have no boundary classifier, so BSMOOTH's
    // vclass handle binds to an executor-owned all-zero column (vclass 0 =
    // plain Laplacian; the manifest write flag only means ensure-materialized).
    ctx.attrBindings = nullptr;
    if (cmd.attrs.size() > 0) {
      for (const auto &entry : cmd.attrs) {
        Assert(entry.handle == string("vclass"),
               "grid executor: only the vclass shim is bindable");
      }
      ensureVclassBinding();
      ctx.attrBindings = &attrBindings_;
    }

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

    // Jacobi snapshot for for_neighbor kernels: region-restricted refresh
    // (this stage call's read set), not the old full-domain copy (O(level)
    // per dab — ~85-170 ms/stroke at 1M verts).
    ctx.co_prev = nullptr;
    if (cmd.needsCoPrev) {
      refreshCoPrevRegion(nodeSpan);
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
    // Mark this call's writes stale for the co_prev stamps — per stage call
    // (i.e. per mirror image), so a seam leaf in both images' query sets is
    // re-refreshed for the mirror after the primary's writes.
    if (moved > 0 && coPrevPosEpoch_.size() > 0) {
      coPrevEpochSeq_++;
      for (int v : dabMoved_) {
        coPrevPosEpoch_[v] = coPrevEpochSeq_;
      }
    }
    if (moved > 0) {
      if (deferNormals) {
        for (int v : dabMoved_) {
          if (pendingNormalStamp_[v] != strokeSeq_) {
            pendingNormalStamp_[v] = strokeSeq_;
            pendingNormals_.append(v);
          }
        }
      } else {
        auto tn = clock::now();
        domain->refreshNormals(
            std::span<const int>(dabMoved_.data(), dabMoved_.size()));
        stats.normalsMs += msSince(tn);
      }
      auto tb = clock::now();
      tree->refreshBounds(std::span<const int>(dabLeaves_.data(), dabLeaves_.size()));
      stats.boundsMs += msSince(tb);
    }

    isFirstOfStep = false;
    return moved;
  }

  /** Refresh the normals of every vert moved since the last flush (the
   * deferNormals accumulation) and return them — the host mirrors the same
   * set. No-op (empty) when nothing is pending. */
  Vector<int> &flushNormals()
  {
    if (pendingNormals_.size() > 0) {
      auto tn = std::chrono::steady_clock::now();
      domain->refreshNormals(
          std::span<const int>(pendingNormals_.data(), pendingNormals_.size()));
      stats.normalsMs +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                    tn)
              .count();
      flushedNormals_ = std::move(pendingNormals_);
      pendingNormals_ = Vector<int>();
      // Re-arm the dedup for the next accumulation window.
      for (int v : flushedNormals_) {
        pendingNormalStamp_[v] = 0;
      }
    } else {
      flushedNormals_.clear();
    }
    return flushedNormals_;
  }

  /** Stroke end: fold the touched region into the grids store (restricted
   * writeback over the touched verts' occurrence grids), flush mask writes,
   * and close the undo step. */
  void endStep()
  {
    auto t0 = std::chrono::steady_clock::now();
    isFirstOfStep = false;
    stats.strokes++;
    flushNormals();
    gridsFoldStroke(domain, log,
                    std::span<const int>(strokeTouchedVerts_.data(),
                                         strokeTouchedVerts_.size()),
                    strokeWroteCo_, strokeWroteMask_);
    stats.writebackMs +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
            .count();
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
  /** The most recent dab's moved verts (deduped) — the per-dab mirror set.
   * Non-const: litestl Vector exposes no const data(). */
  Vector<int> &lastDabMoved()
  {
    return dabMoved_;
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
  /** Refresh the Jacobi snapshot over this stage call's read set — the query
   * leaves' owned verts closed under the CSR 1-ring (for_neighbor reads the
   * full ring of every owned vert; Gaussian falloff has no compact support,
   * so a geometric pad would be wrong) — and only where positions changed
   * since the copy was last taken. Validity is (stroke generation, position
   * epoch): the stroke generation invalidates everything across strokes
   * (undo seeks and domain rebuilds happen between strokes), and the epoch,
   * bumped per stage call from the moved set, keeps intra-stroke copies
   * current — a program's smooth stage sees the main stage's writes, and a
   * mirror image sees the primary's. O(region) per call. */
  void refreshCoPrevRegion(std::span<GridExecNode *> nodes)
  {
    int vc = domain->vertCount();
    if (int(coPrevStorage_.size()) != vc) {
      coPrevStorage_.resize(vc);
      coPrevStrokeGen_.resize(vc);
      coPrevCopyEpoch_.resize(vc);
      coPrevPosEpoch_.resize(vc);
      for (int i = 0; i < vc; i++) {
        coPrevStrokeGen_[i] = 0;
        coPrevCopyEpoch_[i] = 0;
        coPrevPosEpoch_[i] = 0;
      }
    }
    const auto &pos = domain->pos();
    auto refresh = [&](int v) {
      if (coPrevStrokeGen_[v] == strokeSeq_ &&
          coPrevCopyEpoch_[v] == coPrevPosEpoch_[v]) {
        return;
      }
      coPrevStorage_[v] = pos[v];
      coPrevStrokeGen_[v] = strokeSeq_;
      coPrevCopyEpoch_[v] = coPrevPosEpoch_[v];
    };
    for (GridExecNode *node : nodes) {
      for (int v : tree->leaves[node->leaf].ownedVerts) {
        refresh(v);
        for (int nb : domain->neighbors(v)) {
          refresh(nb);
        }
      }
    }
  }

  /** (Re)build the vclass shim binding: a materialized all-zero int column
   * sized to the domain, exposed under BSMOOTH's "vclass" handle. Sized
   * lazily so vclass-free sessions never pay for it; a domain rebuild
   * resizes on the next dab (attach() drops it). */
  void ensureVclassBinding()
  {
    // New pages materialize from page.value == 0, and nothing ever writes
    // this column, so resize alone keeps it all-zero.
    int vc = domain->vertCount();
    if (vclass_.size() != vc) {
      vclass_.resize(vc);
    }
    if (attrBindings_.items.size() == 0) {
      mesh::AttrRef ref;
      ref.data = &vclass_;
      ref.name = string(".grid.boundary.vclass");
      ref.type = mesh::AttrType::INT;
      attrBindings_.items.append(BrushAttrBinding{string("vclass"), ref});
    }
  }

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
  /** All-zero vclass shim for BSMOOTH (see ensureVclassBinding). */
  mesh::AttrData<int> vclass_{string(".grid.boundary.vclass"), 0};
  BrushAttrBindings attrBindings_;
  Vector<float3> coPrevStorage_;
  /** co_prev validity stamps (see refreshCoPrevRegion): sized lazily with
   * coPrevStorage_, so smooth-free sessions never pay the allocation. */
  Vector<uint32_t> coPrevStrokeGen_;
  Vector<uint32_t> coPrevCopyEpoch_;
  Vector<uint32_t> coPrevPosEpoch_;
  uint32_t coPrevEpochSeq_ = 0;

  Vector<uint32_t> touchedStamp_;
  uint32_t strokeSeq_ = 0;
  Vector<int> pendingNormals_;
  Vector<int> flushedNormals_;
  Vector<uint32_t> pendingNormalStamp_;
  Vector<int> strokeTouchedVerts_;
  Vector<int> strokeTouchedLeaves_;
  Vector<uint8_t> leafTouched_;
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
