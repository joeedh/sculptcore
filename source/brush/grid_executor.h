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
 * meshlog, no attr overrides, no preview machinery. What it runs is whatever
 * the generated dispatch will instantiate here — there is no tool roster in
 * this file. A kernel is declined only for a missing capability (an attr layer
 * with no grid storage); those brushes fall back to the materialized path. The
 * dispatch rule is supportsBrush(), engine-owned and derived from each
 * kernel's own def.
 *
 * Stroke shape: beginStep() → applyDab()* → endStep(). endStep folds the
 * stroke into the grids store via Multires::gridsWriteback, restricted to the
 * touched verts' occurrence grids — O(region), not O(level). */

#include "accum_mode.h"
#include "automask.h"
#include "brush_command.h"
#include "brush_iterators.h"
#include "brush_program.h"
#include "brushes/all.h"
#include "capture_policy.h"
#include "grid_attr_bind.h"

#include "spatial/spatial.h"
#include "subdiv/grid_domain.h"
#include "subdiv/grid_draw_source.h"
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
 * face-domain saves likewise — a face kernel's cells are a store channel, and
 * every channel is captured at the fold, which is where the store is first
 * written (a per-dab snapshot here would capture nothing). */
struct GridCapturePolicy {
  template <mesh::ElemType Domain>
  static void capture(CommandCtxBase &ctx,
                      std::span<GridExecNode *> nodes,
                      std::span<const CaptureSaveDesc> saves);
};

/** Stroke-end fold shared by the CPU executor and the GPU session: capture
 * the touched verts' occurrence-grid store blocks into the undo log FIRST
 * (the store is untouched until this fold), then the restricted writeback
 * (positions), mask flush and attr-mirror scatter, then close the undo step.
 * Each channel's capture precedes its own write. O(region). */
inline void gridsFoldStroke(subdiv::GridLevelDomain *domain,
                            subdiv::GridStrokeLog *log,
                            std::span<const int> touched,
                            bool wroteCo,
                            bool wroteMask,
                            std::span<GridAttrMirror *const> attrMirrors = {},
                            std::span<const int> touchedGrids = {})
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
    for (GridAttrMirror *m : attrMirrors) {
      if (!m->dirty || m->channel < 0 || m->domain != subdiv::GridElemDomain::Vertex) {
        continue;
      }
      if (log) {
        log->captureGrids(gridSpan, m->channel);
      }
      gridAttrScatter(*m, domain, touched);
    }
  }
  // Face mirrors ride the stroke's touched-*grid* set instead: a face stage
  // moves no vertex, so the occurrence walk above has nothing to derive from.
  if (touchedGrids.size() > 0) {
    for (GridAttrMirror *m : attrMirrors) {
      if (!m->dirty || m->channel < 0 || m->domain != subdiv::GridElemDomain::Face) {
        continue;
      }
      if (log) {
        log->captureGrids(touchedGrids, m->channel);
      }
      gridAttrScatterFace(*m, domain, touchedGrids);
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

/** Per-cell iteration over a leaf's grids — the grids counterpart of
 * BasicFaceIter. A GridTree leaf owns whole grids, so the S² quad cells of
 * each are visited exactly once with no canonical-corner rule and no shared
 * elements; `f` is the dense cell id gridAttrFaceIndex assigns, which is what
 * a Face-domain mirror column is indexed by.
 *
 * FacePtr carries no CommandExecutor: the mesh version's `ctx` member is
 * mesh-path state and no generated face kernel reads it. */
struct GridFaceIter {
  struct FacePtr {
    int f;
    float3 center;
    float3 &no;
    int indexInNode = 0;

    FacePtr(int f_, float3 center_, float3 &no_) : f(f_), center(center_), no(no_)
    {
    }
    FacePtr(const FacePtr &b) : f(b.f), center(b.center), no(b.no), indexInNode(b.indexInNode)
    {
    }
  };

  subdiv::GridLevelDomain *d = nullptr;
  const Vector<int> *grids = nullptr;
  int S = 0;
  int cellsPerGrid = 0;
  int idx = 0;
  /** Declared before `ptrs`, which holds a reference to it — and rebound by
   * every copy, so an iterator copy never points at the source's storage. */
  float3 noStorage_ = float3(0.0f, 0.0f, 1.0f);
  FacePtr ptrs;

  /** Never create a face iter on a leaf with no grids (mirrors BasicFaceIter). */
  GridFaceIter(subdiv::GridLevelDomain *d, const Vector<int> *grids)
      : d(d), grids(grids), S(d->gridSide()), cellsPerGrid(d->gridSide() * d->gridSide()),
        ptrs(0, float3(0.0f, 0.0f, 0.0f), noStorage_)
  {
    loadCell(0);
  }

  GridFaceIter(const GridFaceIter &b)
      : d(b.d), grids(b.grids), S(b.S), cellsPerGrid(b.cellsPerGrid), idx(b.idx),
        noStorage_(b.noStorage_), ptrs(b.ptrs.f, b.ptrs.center, noStorage_)
  {
    ptrs.indexInNode = b.ptrs.indexInNode;
  }

  int count() const
  {
    return int(grids->size()) * cellsPerGrid;
  }

  /** Centroid and geometric normal of cell `i` from the four lattice corners
   * — the same cross-of-diagonals the domain's own normal fill uses. */
  void loadCell(int i)
  {
    if (i >= count()) {
      return;
    }
    const int g = (*grids)[i / cellsPerGrid];
    const int cell = i % cellsPerGrid;
    const int u = cell % S, v = cell / S;
    const int w = S + 1;
    const int *gv = d->gridVerts(g);
    const float3 &a = d->pos()[gv[v * w + u]];
    const float3 &b = d->pos()[gv[v * w + u + 1]];
    const float3 &c = d->pos()[gv[(v + 1) * w + u + 1]];
    const float3 &e = d->pos()[gv[(v + 1) * w + u]];
    noStorage_ = (c - a).cross(e - b).normalized();
    ptrs.~FacePtr();
    new (&ptrs) FacePtr(gridAttrFaceIndex(g, u, v, S), (a + b + c + e) * 0.25f, noStorage_);
  }

  bool operator==(const GridFaceIter &b)
  {
    return idx == b.idx;
  }
  bool operator!=(const GridFaceIter &b)
  {
    return idx != b.idx;
  }

  FacePtr &operator*()
  {
    return ptrs;
  }

  GridFaceIter &operator++()
  {
    idx++;
    loadCell(idx);
    ptrs.indexInNode = idx;
    return *this;
  }

  GridFaceIter begin()
  {
    GridFaceIter it(*this);
    it.idx = 0;
    it.loadCell(0);
    it.ptrs.indexInNode = 0;
    return it;
  }

  GridFaceIter end()
  {
    GridFaceIter it(*this);
    it.idx = it.count();
    return it;
  }
};

struct GridBrushExecutor {
  using vertex_iter = GridVertexIter<AccumLive>;
  using vertex_iter_factory = std::function<vertex_iter(GridExecNode &)>;
  using face_iter = GridFaceIter;
  using face_iter_factory = std::function<face_iter(GridExecNode &)>;
  using node_type = GridExecNode;
  using capture_policy = GridCapturePolicy;
  /** A GridTree leaf owns whole grids, so a `face` stage has an element to
   * iterate: the S² quad cells of each (see GridFaceIter). Their per-cell
   * values live in a Face-domain session channel of the grids store, which is
   * what plan phase P4b added.
   *
   * Declared before brush_command: naming CommandCtx checks the CommandTypes
   * concept against a still-incomplete class, so anything the concept requires
   * has to be visible by then. */
  static constexpr bool supportsFaceStages = true;
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

  /** Set `ctx.renderMatrix` (ViewPlane/ViewRepeat texture UV) from 16 flat
   * floats, row-major — CommandExecutor::setRenderMatrix's twin, taking a raw
   * pointer because the grids path is reached through the c-api session rather
   * than the reflected object. Without it the texture kernels map every
   * view-pinned sample through an identity matrix. */
  void setRenderMatrix(const float *m16)
  {
    if (!m16) {
      return;
    }
    float *dst = &ctx.renderMatrix[0][0];
    for (int i = 0; i < 16; i++) {
      dst[i] = m16[i];
    }
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
    // Dense ids remap on a rebuild, so every attr column is stale: the zero
    // column only needs resizing (ensureAttrBindings does it), the mirrors
    // must be dropped and re-gathered from the store.
    zeroColumn_.resize(0);
    attrMirrors_.clear();
    attrBindings_.items.clear();
    leafTouched_.resize(nodes_.size());
    for (int i = 0; i < int(leafTouched_.size()); i++) {
      leafTouched_[i] = 0;
    }
    dabGrids_.clear();
    strokeTouchedGrids_.clear();
    dabGridStamp_.resize(d->gridCount());
    strokeGridStamp_.resize(d->gridCount());
    for (int i = 0; i < d->gridCount(); i++) {
      dabGridStamp_[i] = 0;
      strokeGridStamp_[i] = 0;
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

  GridFaceIter makeFaceIter(GridExecNode &node)
  {
    return GridFaceIter(domain, &tree->leaves[node.leaf].grids);
  }

  /** Quad cells across the level — the size of a Face-domain mirror column,
   * and one past the largest id GridFaceIter yields. */
  int faceCount() const
  {
    return domain->gridCount() * domain->gridSide() * domain->gridSide();
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

  /** Type-dispatch half of createCommand, stateless so supportsBrush can reach
   * a kernel's def without a live stroke. Both neighbor sources are GridCsrNbr:
   * the lattice CSR is the only adjacency a grid vertex has, so a @fulltopo
   * kernel's live-disk slot resolves to the same source rather than to nothing.
   *
   * The roster is the generated registry's — whatever each kernel's own
   * annotations permit on this domain — not a switch listing tools by name.
   * `brushOrNull` is the extras registry's uniform-default seed, as on the mesh
   * path; a null one just means extra kernels report unhandled. */
  template <class AccMode>
  static bool createCommandSwitch(SculptBrushes brushType,
                                  Brush *brushOrNull,
                                  brush_command &def)
  {
    if (command::createBuiltinBrush<GridBrushExecutor, GridCsrNbr, GridCsrNbr, AccMode>(
            int(brushType), /*csrNeighbors=*/true, def)) {
      return true;
    }
    return brushOrNull &&
           command::createExtraBrush<GridBrushExecutor, GridCsrNbr, GridCsrNbr, AccMode>(
               int(brushType), /*csrNeighbors=*/true, *brushOrNull, def);
  }

  /** Whether the domain can bind one of a kernel's declared attr layers.
   * Routing lives in grid_attr_bind.h and reads the entry's metadata plus
   * `attrs`' storage policy; ensureAttrBindings asserts on the same call, so
   * the capability gate and the binding it gates cannot drift apart. */
  static bool attrBindable(const BrushAttrManifestEntry &entry,
                           const subdiv::MultiresAttrs *attrs)
  {
    return gridAttrPlan(entry, attrs) != GridAttrPlanKind::Unbindable;
  }

  /** Engine-owned dispatch rule: can this tool run grids-native? Everything
   * else falls back to the materialized-mesh path.
   *
   * The answer comes from the kernel's own def, never from a tool list: an
   * attr layer this domain cannot bind has no storage to write. `attrs` is the
   * stack's storage policy — pass it whenever there is one, since a Derived
   * layer is bindable only in its absence (grid_attr_bind.h). */
  static bool supportsBrush(SculptBrushes brushType,
                            const subdiv::MultiresAttrs *attrs = nullptr)
  {
    Brush scratch;
    brush_command def;
    if (!createCommandSwitch<AccumLive>(brushType, &scratch, def)) {
      return false;
    }
    for (const auto &entry : def.attrs) {
      if (!attrBindable(entry, attrs)) {
        return false;
      }
    }
    return true;
  }

  brush_command createCommand(SculptBrushes brushType)
  {
    brush_command def;
    bool ok = createCommandSwitch<AccumLive>(brushType, brush, def);
    Assert(ok, "grid executor: unsupported brush (gate on supportsBrush)");
    (void)ok;
    if (def.grabModeCapable && anchoredGrab) {
      def.grabMode = true;
      def.uniforms = decltype(def.uniforms)();
      def.attrs = decltype(def.attrs)();
      createCommandSwitch<AccumOrigGrab>(brushType, brush, def);
    } else if (nonAccum && def.accumulable && !def.relaxesBase) {
      def.uniforms = decltype(def.uniforms)();
      def.attrs = decltype(def.attrs)();
      createCommandSwitch<AccumOrig>(brushType, brush, def);
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
    strokeTouchedGrids_.clear();
    for (int i = 0; i < int(leafTouched_.size()); i++) {
      leafTouched_[i] = 0;
    }
    strokeWroteCo_ = false;
    strokeWroteMask_ = false;
    for (GridAttrMirror *m : attrMirrors_.items) {
      m->dirty = false;
    }
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

    updateStrokeFrame(origin);
    brush->pushStrokeSample(origin, normal);

    stats.dabs++;
    float floorR = cmd.unbounded ? brush->radius * brush->unboundedExtent : 0.0f;
    if (!queryDabLeaves(cmd.grabMode, std::fmax(brush->radius, floorR), origin)) {
      isFirstOfStep = false;
      return 0;
    }

    dabSeq_++;
    dabMoved_.clear();
    dabGrids_.clear();
    execStage(cmd, origin, normal);
    finishDab();

    isFirstOfStep = false;
    // A face stage moves no vertex, so it reports its own unit: the grids it
    // touched. Callers read this only as "did the dab land".
    return dabMoved_.size() > 0 ? int(dabMoved_.size()) : int(dabGrids_.size());
  }

  /** One logical dab of a composite brush program — the grids mirror of
   * CommandExecutor::execProgram. One stroke sample and one node query serve
   * every entry; the entries run in order over that shared set with their
   * float/invert overrides pushed onto the brush's props and rolled back
   * after each stage, and normals/bounds refresh once at the end (the mesh
   * path never refreshes between entries — per-stage refresh would diverge).
   * Grab-class entries and attr-layer overrides are unsupported here.
   * Returns the union moved-vert count across stages, or (when no stage moved
   * a vertex, i.e. every stage was a face stage) the touched-grid count. */
  int applyProgram(BrushProgram *prog, float3 origin, float3 normal)
  {
    if (!prog || prog->commands.size() == 0) {
      return 0;
    }

    updateStrokeFrame(origin);
    brush->pushStrokeSample(origin, normal);

    stats.dabs++;
    // One query serves every stage, so its radius floor is the max over them.
    float floorR = 0.0f;
    for (auto &entry : prog->commands) {
      brush_command cmd = createCommand(entry.type);
      Assert(!cmd.grabMode, "grids programs: grab-class entries unsupported");
      Assert(entry.attrLayerOverrides.size() == 0,
             "grids programs: attr-layer overrides unsupported");
      if (cmd.unbounded) {
        floorR = std::fmax(floorR, brush->radius * brush->unboundedExtent);
      }
    }
    if (!queryDabLeaves(false, std::fmax(brush->radius, floorR), origin)) {
      isFirstOfStep = false;
      return 0;
    }

    dabSeq_++;
    dabMoved_.clear();
    dabGrids_.clear();

    for (auto &entry : prog->commands) {
      // Push the entry's sparse overrides onto the authored props, snapshot
      // under the resolved prop name for an exact rollback (execProgram's
      // model; the base props survive the dab unmodified).
      Vector<BrushFloatOverride> savedFloats;
      for (auto &ov : entry.floatOverrides) {
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
      if (cmd.registerProps && brush->props.struct_def) {
        cmd.registerProps(*brush->props.struct_def);
      }
      brush->loadCommonProps(&brush->deviceInputCtx);
      if (cmd.loadUniformProps) {
        cmd.loadUniformProps(*brush, &brush->deviceInputCtx);
      }

      execStage(cmd, origin, normal);

      for (auto &s : savedFloats) {
        brush->props.setFloat(s.name.c_str(), s.value);
      }
      if (entry.overrideInvert) {
        brush->props.setValue<bool>("invert", savedInvert);
      }
    }

    finishDab();
    isFirstOfStep = false;
    // Same fallback as applyDab: a face-only program moves nothing, so it
    // reports the grids it touched rather than reading as "the brush missed".
    return dabMoved_.size() > 0 ? int(dabMoved_.size()) : int(dabGrids_.size());
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
                    strokeWroteCo_, strokeWroteMask_,
                    std::span<GridAttrMirror *const>(attrMirrors_.items.data(),
                                                     attrMirrors_.items.size()),
                    std::span<const int>(strokeTouchedGrids_.data(),
                                         strokeTouchedGrids_.size()));
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
  /** The stroke's / the last dab's touched grids — a face stage's unit, empty
   * for every vertex kernel. */
  const Vector<int> &strokeTouchedGrids() const
  {
    return strokeTouchedGrids_;
  }
  Vector<int> &lastDabGrids()
  {
    return dabGrids_;
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
  static double msSince(std::chrono::steady_clock::time_point t0)
  {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                     t0)
        .count();
  }

  /** Node filter for one logical dab: fill dabLeaves_/nodePtrs_ with the
   * leaves within radius `r` of `origin`. Grab-class strokes pin their first
   * dab's set (the region is fixed at stroke start — the mesh path's
   * grabFilterNodes, without the dyntopo fallback). False when empty. */
  bool queryDabLeaves(bool grabMode, float r, float3 origin)
  {
    auto t0 = std::chrono::steady_clock::now();
    dabLeaves_.clear();
    if (grabMode) {
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
    nodePtrs_.clear();
    for (int li : dabLeaves_) {
      nodePtrs_.append(&nodes_[li]);
    }
    stats.queryMs += msSince(t0);
    return dabLeaves_.size() > 0;
  }

  /** One kernel stage over the current dabLeaves_/nodePtrs_ set: ctx setup,
   * undo capture, co_prev/disp/automask maintenance, the parallel kernel
   * loop, and folding the stage's writes into the logical dab's dabMoved_
   * union. Callers own dabSeq_++/dabMoved_.clear() (once per logical dab,
   * so the union dedups across a program's stages) and the end-of-dab
   * finishDab() normals/bounds refresh. */
  void execStage(brush_command &cmd, float3 origin, float3 normal)
  {
    ctx.m = nullptr;
    ctx.meshLog = nullptr;
    ctx.surfacePos = origin;
    ctx.surfaceNo = normal;
    ctx.isFirstOfStep = isFirstOfStep;

    // DSL attr manifest: each declared layer routes to a default column,
    // sculpt-layer scratch, or a session store channel (grid_attr_bind.h).
    ctx.attrBindings = nullptr;
    layerScratchActive_ = false;
    if (cmd.attrs.size() > 0) {
      ensureAttrBindings(cmd);
    }

    if (cmd.writesMask) {
      strokeWroteMask_ = true;
      if (isFirstOfStep) {
        // The channel must exist before the log captures its blocks.
        domain->ensureMaskChannel();
      }
    } else if (!cmd.faceMode) {
      // A face stage writes cells, never positions: claiming otherwise would
      // send the fold through gridsWriteback over an empty touched-vert set.
      strokeWroteCo_ = true;
    }

    using clock = std::chrono::steady_clock;
    std::span<GridExecNode *> nodeSpan(nodePtrs_.data(), nodePtrs_.size());
    auto t0 = clock::now();

    // Per-dab `host` stage, before capture as on the mesh path: it derives ctx
    // values the kernel then reads (kelvinlet's parameter clamp away from the
    // 1/(1-2nu) singularity, wingscrape's rotated wing normals).
    if (cmd.execHost) {
      cmd.execHost(ctx, *brush);
    }

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
    // No kernel opts into orig normals today; when one does it needs a grids
    // stampBase that saves them, not just the disp vector below.
    Assert(!cmd.needsOrigNormals, "grid executor: orig normals are not maintained here");
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
    const bool faceStage = cmd.faceMode;
    task::parallel_for(util::IndexRange(nodePtrs_.size()), [&](util::IndexRange range) {
      for (int i : range) {
        GridExecNode *node = nodePtrs_[i];
        const subdiv::GridTree::Leaf &leaf = tree->leaves[node->leaf];
        if ((faceStage ? leaf.grids.size() : leaf.ownedVerts.size()) == 0) {
          continue;
        }
        CommandCtx<GridBrushExecutor> finalCtx(ctx, *node, *this, *brush);
        cmd.exec(finalCtx);
      }
    });

    cmd.execPost(ctx, nodeSpan);
    stats.kernelMs += msSince(t0);

    // Sculpt-layer fold (the mesh path's LayerEditScope, per stage): the
    // scratch holds this stage's delta, so co += w * delta over the written
    // verts, then re-zero for the next stage or mirror image.
    if (layerScratchActive_) {
      subdiv::Multires *mr = domain->multires();
      const int li = mr->editTarget();
      const float lw = li >= 0 ? mr->layerWeight(li) : 0.0f;
      for (GridExecNode *node : nodeSpan) {
        for (int v : node->affected_verts) {
          float3 &dv = layerScratch_[v];
          domain->pos()[v] += dv * lw;
          dv = float3(0.0f, 0.0f, 0.0f);
        }
      }
    }

    // Fold the stage's writes into the logical dab's union: dabStamp_ vs the
    // caller-bumped dabSeq_ dedups, so a vert two stages touch appears once.
    bool wrote = false;
    for (GridExecNode *node : nodeSpan) {
      if (faceStage && bool(node->flag)) {
        // A face kernel has no affected-vert list to report through — it marks
        // the leaf instead — and a leaf owns whole grids, so leaf granularity
        // IS grid granularity. Generated face stages flag every leaf they
        // iterate, so this over-marks a dab's rim; the query radius bounds it.
        for (int g : tree->leaves[node->leaf].grids) {
          if (dabGridStamp_[g] != dabSeq_) {
            dabGridStamp_[g] = dabSeq_;
            dabGrids_.append(g);
            if (strokeGridStamp_[g] != strokeSeq_) {
              strokeGridStamp_[g] = strokeSeq_;
              strokeTouchedGrids_.append(g);
            }
          }
        }
      }
      if (node->affected_verts.size() > 0) {
        wrote = true;
        if (!leafTouched_[node->leaf]) {
          leafTouched_[node->leaf] = 1;
          strokeTouchedLeaves_.append(node->leaf);
        }
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
    // Mark the union stale for the co_prev stamps — per stage call, so a
    // later stage (or the mirror image) re-refreshes what this one wrote;
    // stamping the whole union over-marks only already-copied verts.
    if (wrote && coPrevPosEpoch_.size() > 0) {
      coPrevEpochSeq_++;
      for (int v : dabMoved_) {
        coPrevPosEpoch_[v] = coPrevEpochSeq_;
      }
    }
  }

  /** End-of-dab normals/bounds refresh over the logical dab's union moved
   * set — once per dab even for multi-stage programs (execProgram's model:
   * the mesh path refreshes per dab, never between entries). */
  void finishDab()
  {
    // A face dab moves no vertex, so it publishes off its own touched-grid set
    // and marks the draw source itself — the host's per-dab refresh has only
    // the moved-vert set to go on.
    if (dabGrids_.size() > 0) {
      std::span<const int> gridSpan(dabGrids_.data(), dabGrids_.size());
      for (GridAttrMirror *m : attrMirrors_.items) {
        if (m->dirty && m->channel >= 0 && m->domain == subdiv::GridElemDomain::Face) {
          gridAttrMirrorFaceToSamples(*m, domain, gridSpan, faceCellScratch_);
        }
      }
      if (subdiv::GridDrawSource *ds = domain->multires()->drawSource()) {
        ds->markGrids(gridSpan);
      }
    }
    if (dabMoved_.size() == 0) {
      return;
    }
    // The draw path reads derived samples, and the store only learns of the
    // stroke at the fold — so an attr stroke publishes its dab here.
    std::span<const int> movedSpan(dabMoved_.data(), dabMoved_.size());
    for (GridAttrMirror *m : attrMirrors_.items) {
      if (m->dirty && m->channel >= 0 && m->domain == subdiv::GridElemDomain::Vertex) {
        gridAttrMirrorToSamples(*m, domain, movedSpan);
      }
    }
    if (deferNormals) {
      for (int v : dabMoved_) {
        if (pendingNormalStamp_[v] != strokeSeq_) {
          pendingNormalStamp_[v] = strokeSeq_;
          pendingNormals_.append(v);
        }
      }
    } else {
      auto tn = std::chrono::steady_clock::now();
      domain->refreshNormals(std::span<const int>(dabMoved_.data(), dabMoved_.size()));
      stats.normalsMs += msSince(tn);
    }
    auto tb = std::chrono::steady_clock::now();
    tree->refreshBounds(std::span<const int>(dabLeaves_.data(), dabLeaves_.size()));
    stats.boundsMs += msSince(tb);
  }

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

  /** (Re)build `ctx.attrBindings` for this command. A read-only handle the
   * grids domain answers with zero gets the shared all-zero column; a written
   * vertex or face layer gets a dense mirror over a session store channel,
   * gathered once per step and scattered by the stroke-end fold. Both are
   * lazy, so an attr-free session pays nothing; attach() drops the mirrors. */
  void ensureAttrBindings(const brush_command &cmd)
  {
    const int vc = domain->vertCount();
    attrBindings_.items.clear();
    for (const auto &entry : cmd.attrs) {
      GridAttrPlanKind kind = gridAttrPlan(entry, &domain->multires()->gridAttrs());
      Assert(kind != GridAttrPlanKind::Unbindable,
             "grid executor: attr layer has no grid storage");
      mesh::AttrRef ref;
      if (kind == GridAttrPlanKind::DefaultColumn) {
        // New pages materialize from page.value == 0, and nothing ever writes
        // this column, so resize alone keeps it all-zero.
        if (zeroColumn_.size() != vc) {
          zeroColumn_.resize(vc);
        }
        ref.data = &zeroColumn_;
        ref.name = gridAttrLayerName(entry);
        ref.type = entry.type;
        attrBindings_.items.append(BrushAttrBinding{entry.handle, ref});
        continue;
      }
      if (kind == GridAttrPlanKind::LayerScratch) {
        // Pages materialize zero and the stage fold re-zeros every write, so
        // resize alone keeps the column all-zero between dabs.
        if (layerScratch_.size() != vc) {
          layerScratch_.resize(vc);
        }
        ref.data = &layerScratch_;
        ref.name = gridAttrLayerName(entry);
        ref.type = entry.type;
        attrBindings_.items.append(BrushAttrBinding{entry.handle, ref});
        layerScratchActive_ = true;
        continue;
      }
      const bool faceLayer = entry.domain == AttrElemDomain::Face;
      GridAttrMirror *m = attrMirrors_.find(entry.handle);
      if (!m) {
        m = alloc::New<GridAttrMirror>("grid attr mirror");
        m->handle = entry.handle;
        m->layer = gridAttrLayerName(entry);
        m->type = entry.type;
        m->domain = faceLayer ? subdiv::GridElemDomain::Face : subdiv::GridElemDomain::Vertex;
        m->floats = gridAttrTypeFloats(entry.type);
        m->column = gridAttrNewColumn(entry.type, m->layer,
                                      faceLayer ? faceCount() : vc);
        attrMirrors_.items.append(m);
      }
      m->channel = gridAttrEnsureChannel(domain->multires(), *m, domain->level());
      if (m->gatheredFor != strokeSeq_) {
        // Per step, not per bind: an undo/redo between strokes swaps store
        // bytes behind the column's back.
        if (faceLayer) {
          gridAttrGatherFace(*m, domain);
        } else {
          gridAttrGather(*m, domain);
        }
        m->gatheredFor = strokeSeq_;
      }
      m->dirty = true;
      ref.data = m->column;
      ref.name = m->layer;
      ref.type = m->type;
      attrBindings_.items.append(BrushAttrBinding{entry.handle, ref});
    }
    ctx.attrBindings = attrBindings_.items.size() ? &attrBindings_ : nullptr;
  }

  /** CommandExecutor::updateStrokeFrame, verbatim (Brush-only state). */
  void updateStrokeFrame(float3 origin)
  {
    if (!brush->strokeDirHostSet) {
      if (brush->strokePathCount == 0) {
        // First dab of the stroke: no tangent exists yet, and the previous
        // stroke's is not one (wing scrape would lean its wings along it).
        brush->strokeDir = float3(0.0f, 0.0f, 0.0f);
      } else {
        float3 d = origin - brush->strokePath[brush->strokePathCount - 1].pos;
        float len = d.length();
        if (len > 1e-7f) {
          brush->strokeDir = d / len;
        }
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
  /** Face-stage touched sets, the grid-granular twins of dabMoved_ /
   * strokeTouchedVerts_ (a leaf owns whole grids, so a face dab's unit is a
   * grid). Empty for every vertex kernel. */
  Vector<int> dabGrids_;
  Vector<uint32_t> dabGridStamp_;
  Vector<uint32_t> strokeGridStamp_;
  Vector<int> strokeTouchedGrids_;
  /** One grid's S² cells, reused by the per-dab face publish. */
  Vector<int> faceCellScratch_;

  mesh::AttrData<float3> dispVec_{string(".grid.disp.vec"), 0};
  mesh::AttrData<int> dispGen_{string(".grid.disp.gen"), 0};
  mesh::AttrData<int> dabGen_{string(".grid.dab.gen"), 0};
  mesh::AttrData<float> cavity_{string(".grid.automask.cavity"), 0};
  mesh::AttrData<int> cavityGen_{string(".grid.automask.gen"), 0};
  /** Shared all-zero column for read-only handles the grids domain answers
   * with zero (BSMOOTH/FEATURE_ALIGN's vclass; see ensureAttrBindings). */
  mesh::AttrData<int> zeroColumn_{string(".grid.attr.zero"), 0};
  /** Sculpt-layer per-dab scratch (LayerScratch plan). All-zero between dabs
   * by the stage fold's re-zero, so it never needs a per-dab reset. */
  mesh::AttrData<float3> layerScratch_{string(".grid.slayer.scratch"), 0};
  bool layerScratchActive_ = false;
  GridAttrMirrorSet attrMirrors_;
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
      // No: derived (refreshed after a seek). Attr: covered by the stroke-end
      // channel capture in gridsFoldStroke, which is where the store is first
      // written — a per-dab snapshot here would capture nothing.
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
