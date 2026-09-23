#pragma once

#include "automask.h"
#include "binding/binding_constructor_builder.h"
#include "brush_command.h"
#include "brush_hooks.h"
#include "brush_iterators.h"
#include "brush_program.h"
#include "brushes/all.h"
#include "capture_policy.h"
#include "displace/compositor.h"
#include "dyntopo/dyntopo.h"
#include "enhance.h"
#include "feature_field.h"
#include "litestl/binding/binding.h"
#include "litestl/util/map.h"
#include "litestl/util/task.h"
#include "mesh/attribute_bool.h"
#include "mesh/boundary.h"
#include "mesh/uv_reproject.h"
#include "meshlog/meshlog.h"
#include "neighbor_source.h"
#include "plane_frame.h"
#include "prepared_cavity.h"
#include "prepared_enhance.h"
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

struct CommandExecutor {
  /** vertex_iter names the AccumLive instantiation so the CommandTypes concept
   * and the factory typedefs are valid type-ids; the generated kernels pick the
   * AccumMode per command via makeVertexIter<AccMode>. */
  using vertex_iter = BasicVertexIter<AccumLive>;
  using vertex_iter_factory = std::function<vertex_iter(spatial::SpatialNode &)>;
  using face_iter = BasicFaceIter;
  using face_iter_factory = std::function<face_iter(spatial::SpatialNode &)>;
  /** Domain seam (grids-native brush path): this executor iterates spatial
   * leaves and captures undo through the meshlog. */
  using node_type = spatial::SpatialNode;
  using capture_policy = MeshCapturePolicy;
  /** A `face` stage is dispatched through makeFaceIter, which this domain has
   * (a spatial leaf owns faces). The generated registry reads this to decide
   * whether to instantiate a face-stage kernel at all. Declared before
   * brush_command â€” the CommandTypes check runs on an incomplete class, so the
   * bit must already be visible. */
  static constexpr bool supportsFaceStages = true;
  using brush_command = BrushCommandDef<CommandCtx<CommandExecutor>>;

  /** Neighbor-bundle normal for the generated for_neighbor loop (the domain
   * seam replacing the emitted `node.data->m->v.no[nb]` read). */
  template <class Ctx> static float3 &nbrNo(Ctx &ctx, int v);

  /** The live vertex normal the view-normal automask evaluates, or null when
   * unavailable (no mesh on the ctx â€” the historical skip). */
  template <class Ctx> const float3 *liveVertNoPtr(Ctx &ctx, int v) const
;

  /** Selects how for_neighbor kernels enumerate the 1-ring: the live disk walk
   * (default) or the cached CSR adjacency (MeshTopoCache::ring1). The choice is
   * made once here and lowered into the kernel instantiation, so the inner loop
   * has no per-neighbor branch. */
  enum class NeighborMode { LiveDisk, Csr };

  Brush *brush;
  PreparedCavityCache preparedCavity;
  PreparedEnhanceCache preparedEnhance;
  bool preparedCavityActive = false;

private:
  bool preparedStepOpen_ = false;
  bool preparedPreviewStroke_ = false;
  bool preparedPreviewClosed_ = false;
  SpatialTree *preparedStepTree_ = nullptr;
  mesh::Mesh *preparedStepMesh_ = nullptr;
  Brush *preparedStepBrush_ = nullptr;
  meshlog::MeshLog *preparedStepLog_ = nullptr;
  int preparedStepId_ = -1;

  bool preparedStepValid() const
;

public:
  bool preparedPreviewSupported() const
  {
    return !previewActive() ||
           (!preparedPreviewClosed_ && (isFirstOfStep || preparedPreviewStroke_));
  }

  void capturePreparedPreview();

  SpatialTree *tree;
  CommandCtxBase ctx;
  bool isFirstOfStep = false;
  /** True while the current step (stroke) runs dyntopo â€” leaf element sets can
   * change mid-stroke, so the capture walk-elision stamps are disabled. */
  bool stepHasDyntopo = false;
  /** Sub-command slot of the exec() in flight (index into the brush program;
   * execBrush uses 0). -1 disables capture walk-elision for this exec(). */
  int curCaptureSlot = -1;
  int curCaptureTool = 0;
  /** Scratch for the capture walk-elision node subset (see exec()). */
  Vector<spatial::SpatialNode *> captureNodes_;
  /** Per-dab filterNodes results, reused so the node list (tens to hundreds of
   * pointers under a large brush) is allocated once per stroke, not per dab.
   * The two are distinct because applyDab's dyntopo pre-pass runs inside it. */
  Vector<spatial::SpatialNode *> dabNodes_;
  Vector<spatial::SpatialNode *> dynTopoNodes_;
  /** Round-0 seed verts handed to runDyntopoRemesh, likewise reused. */
  Vector<int> dynTopoSeed_;
  /** coPrev incremental-refresh state: after the stroke's first full snapshot,
   * later needsCoPrev execs refresh only the verts of nodes an exec touched
   * since the previous refresh (coPrevDirty_, deduped by node->coPrevStamp
   * against coPrevGen_). Only valid on topology-stable strokes â€” same
   * condition as the capture stamps. */
  bool coPrevFull_ = false;
  int coPrevGen_ = 0;
  Vector<spatial::SpatialNode *> coPrevDirty_;

  /** "This vert moved in the current dab", stamped by generation. A hash set
   * cost ~50k inserts + ~33k probes per dab under a large brush; the border
   * pass only ever asks a membership question, so a stamp array answers it in
   * one indexed read. */
  Vector<uint32_t> movedStamp_;
  uint32_t movedStampGen_ = 0;

  /** Per-exec `nodes[i]->affected_verts.size()` taken before the kernels run,
   * so border propagation sees this dab's moved verts and not the whole
   * stroke's backlog. */
  Vector<int> affectedBase_;

  /** Pinned node set of one grab-class symmetry image (see grabFilterNodes).
   * Node ids, not pointers: a leaf can be freed and its slot reused between
   * dabs, and node_from_id null-checks for us. */
  struct GrabRegion {
    float3 center;
    float radius = 0.0f;
    Vector<int> nodeIds;
  };
  Vector<GrabRegion> grabRegions_;
  /** Drag-widened filter high-water for the dyntopo grab fallback, which can't
   * pin a region. Never allowed to shrink: a from-orig dab only writes verts
   * inside the filter, so a leaf it drops keeps its old displacement and leaves
   * a stale ring behind (#35). */
  float grabWidenRadius_ = 0.0f;

  /** UV slide-reprojection deferral (frozen-topology strokes): vert -> its
   * position before the first dab that moved it. Flushed by endStep â€” one
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
   * command is accumulable, the executor stamps each in-region vert into
   * `.brush.disp.*` (keyed by `strokeGen`) and runs the AccumOrig kernel
   * instantiation, so deformation is measured from the derived stroke-start
   * base `co - disp`. */
  bool nonAccum = false;
  /** True when this stroke anchors its origin, which makes a `@grabmode`
   * kernel take the from-orig fixed-region policy. It is a stroke property, not
   * a kernel one â€” the same kelvinlet dragged along a path accumulates like any
   * other brush â€” so the host owns it (TS: `strokeMethod === ANCHORED`).
   * Defaults true so a host that never sets it gets the historical behavior:
   * grab and kelvinlet, the only `@grabmode` kernels, always grab. */
  bool anchoredGrab = true;
  /** Grab-class symmetry dab marker (#35). The dab dispatch calls setGrabAccumAdd
   * per symmetry image before applyDab: false on the primary image (which begins
   * a new logical dab â†’ bumps `dabGen`), true on mirror images (same dab). The
   * AccumOrigGrab write-back uses the per-vert `.brush.dab.gen` stamp vs `dabGen`
   * to re-base the first image's verts and add later images' onto them. */
  bool grabAccumAdd = false;
  /** Monotonic per-dab counter for the grab symmetry first-touch stamp. Bumped on
   * each primary image (setGrabAccumAdd(false)); pushed into ctx.curDabGen in
   * exec(). Must be non-zero in use, since the `.brush.dab.gen` attr defaults 0. */
  uint32_t dabGen = 0;
  uint32_t strokeGen = 0;
  /** Plane-frame policy for `@planeFrame` kernels (plane_frame.h). Sticky like
   * `nonAccum`: the host pushes it before every stroke, default included.
   * `planeFrameState_` is the per-stroke memory it resolves against (reset by
   * beginStep and by setPlaneFrame). */
  PlaneFramePolicy planeFrame_;
  PlaneFrameState planeFrameState_;
  /** Symmetry image of the dab in flight: the component signs that map the
   * primary image onto it, and whether it is a mirror at all. A mirror plane
   * dab takes the reflected primary frame instead of running its own gather.
   * Set per image by the host (setImageSign) or the batch c-api; the default
   * is the primary. */
  float3 imageSign_{1.0f, 1.0f, 1.0f};
  bool imageIsMirror_ = false;
  /** The primary image's stroke tangent and the origin it was measured from,
   * so a mirror image takes the reflected primary tangent instead of the
   * difference between its own origin and the primary's (updateStrokeFrame). */
  float3 primaryStrokeDir_{0.0f, 0.0f, 0.0f};
  float3 primaryOrigin_{0.0f, 0.0f, 0.0f};
  bool hasPrimaryOrigin_ = false;
  /** Scratch for resolvePlaneFrame: the wider gather candidates, the per-node
   * gather partials, and the re-gathered dab region when the centre moved off
   * the cursor. */
  Vector<spatial::SpatialNode *> planeQueryNodes_;
  Vector<PlaneFrameAccum> planeAccums_;
  Vector<spatial::SpatialNode *> planeFrameNodes_;
  /** What the most recent plane-frame substitution did (tests / HUD): the
   * cursor it started from, the frame the kernel ran with, and whether one
   * happened at all in the last exec(). */
  bool lastPlaneResolved = false;
  float3 lastPlaneCursor{};
  float3 lastPlaneNormal{};
  float3 lastPlaneCenter{};
  // Memo for filterRadiusFloor / grabAnchoredTool: building a command def per
  // dab just to read a couple of codegen flags is wasteful, and the answer only
  // depends on the tool.
  int floorMemoTool_ = -1;
  bool floorMemoUnbounded_ = false;
  bool floorMemoGrabCapable_ = false;
  meshlog::MeshLog *meshLog = nullptr;
  /** Stats of the most recent applyDynTopoDab, for the TS HUD (read after each
   * dab and accumulated per stroke). */
  dyntopo::DynTopoStats lastDynTopoStats;
  /** Leaf count the most recent applyDab handed the deform program. The per-dab
   * cost is linear in it, so tests assert on it instead of on wall-clock. */
  int lastDabNodeCount = 0;
  Vector<float3> coPrevStorage; // backing store for ctx.co_prev (Jacobi snapshot)
  /** Backing store for resolved DSL attribute bindings (ctx.attrBindings),
   * rebuilt per dab in exec(). */
  BrushAttrBindings attrBindingStorage;
  /** Sculpt-layer edit brackets for written SCULPT_LAYER bindings, rebuilt per
   * dab in exec() (begin before the kernel, end after execPost). */
  Vector<displace::LayerEditScope> layerScopes;
  Vector<int> layerRegionVerts; // dab-region vert ids the scopes snapshot
  /** Attr-layer overrides applied when exec() is called without an explicit
   * override span (the applyDab/execBrush path â€” execProgram passes its own).
   * Lets a single-brush driver (debug_app `stroke layer=`) retarget a kernel's
   * attr handle at a chosen mesh layer. */
  Vector<BrushAttrLayerOverride> defaultAttrOverrides;
  /** Uniform-dynamics validation (Wave 4): run once per stroke (first dab) against
   * the active brush's manifest. On failure the whole stroke is skipped so the
   * mesh is never mutated by a misconfigured binding. `lastValidation` is the
   * most recent result (readable by the bridge); `strokeValidationFailed` gates
   * every dab of a failed stroke. */
  UniformValidationResult lastValidation;
  props::ScalarRegistrationResult lastRegistration;
  bool strokeValidationFailed = false;
  /** Legacy mutable manifest descriptors, borrowed until the next query.
   * Configuration methods resolve indices through the private canonical copy.
   * Checked callers also supply a query token and can request owned snapshots. */
  Vector<BrushUniformManifestEntry> queriedUniforms;

private:
  Vector<BrushUniformManifestEntry> canonicalUniforms_;
  int uniformQueryToken_ = 0;
  const CommandExecutor *querySource_ = nullptr;
  Brush *queryBrush_ = nullptr;
  props::StructDef *queryStruct_ = nullptr;
  static int allocateUniformQueryToken();
  int checkedUniformName(int token, int index, int scalarType, string &name) const
;

public:
  int uniformQueryToken() const
  {
    return querySource_ == this ? uniformQueryToken_ : 0;
  }
  BrushUniformManifestEntry uniformSnapshotChecked(int token, int uniformIndex) const
;

  static litestl::binding::types::Struct<CommandExecutor> *defineBindings();

  /** Whether the latest checked declaration or execution operation succeeded. */
  bool lastUniformValidationOk() const
  {
    return lastRegistration.error == props::PropError::ERROR_NONE;
  }

  CommandExecutor(SpatialTree *tree, Brush *brush) : tree(tree), brush(brush), ctx()
  {
  }

  /** Set `ctx.renderMatrix` (ViewPlane/ViewRepeat texture UV) from 16 flat
   * floats, in the same element order as the debug app's `set_render_matrix`
   * verb. Bound-Vector arg = the marshal-safe bridge seam. Wrong size is a
   * no-op. */
  void setRenderMatrix(Vector<float> &m16);

  /** Select the SMOOTH for_neighbor source: 0 = LiveDisk (live topology links),
   * 1 = Csr (the cached ring1 adjacency). The LiteMesh sculpt path uses Csr â€”
   * a freshly built mesh doesn't maintain live disk links, so LiveDisk smooth
   * finds no neighbors and no-ops. Exposed as an int (NeighborMode is an
   * unbound enum). */
  void setNeighborMode(int mode)
  {
    neighborMode = static_cast<NeighborMode>(mode);
  }

  /** Enable non-accumulate mode for the upcoming stroke, and set its generation
   * stamp (a monotonic per-stroke counter; must be non-zero, since the
   * `.brush.disp.gen` attr defaults to 0 = "not stamped this stroke"). */
  void setNonAccum(bool v)
  {
    nonAccum = v;
  }

  /** Declare whether the upcoming stroke is anchored (see `anchoredGrab`). Must
   * be set before createCommand, i.e. before the first dab of the stroke. */
  void setAnchoredGrab(bool v)
  {
    anchoredGrab = v;
  }

  /** Mark the upcoming grab-class dab/image (#35): false = primary image, which
   * begins a new logical dab and bumps the per-dab generation so every vert it
   * touches re-bases from orig; true = mirror image of the same dab (shared verts
   * add). The AccumOrigGrab write-back keys off the per-vert dab stamp, so this
   * only needs to advance `dabGen` once per dab (on the primary image). */
  void setGrabAccumAdd(bool v);
  void setStrokeGen(int gen)
  {
    strokeGen = uint32_t(gen);
  }

  /** Push the plane-frame policy for the upcoming stroke (plane_frame.h).
   * `normalMode` is a PlaneNormalMode (0 Surface, 1 Area, 2 View, 3..5
   * X/Y/Z), `centerMode` a PlaneCenterMode (0 Cursor, 1 Area); the view axis
   * is object-space, surface -> eye. Resets the stroke's frame memory, so
   * call it before the first dab. Out-of-range modes fall back to Surface /
   * Cursor. */
  void setPlaneFrame(int normalMode,
                     int centerMode,
                     bool originalNormal,
                     bool originalPlane,
                     float normalRadiusFactor,
                     float areaRadiusFactor,
                     float stabilizeNormal,
                     float stabilizePlane,
                     float viewX,
                     float viewY,
                     float viewZ)
  {
    PlaneFramePolicy p;
    p.normal = (normalMode >= 0 && normalMode <= int(PlaneNormalMode::Z))
                   ? PlaneNormalMode(normalMode)
                   : PlaneNormalMode::Surface;
    p.center = centerMode == int(PlaneCenterMode::Area) ? PlaneCenterMode::Area
                                                         : PlaneCenterMode::Cursor;
    p.originalNormal = originalNormal;
    p.originalPlane = originalPlane;
    p.normalRadiusFactor = std::isfinite(normalRadiusFactor) ? normalRadiusFactor : 1.0f;
    p.areaRadiusFactor = std::isfinite(areaRadiusFactor) ? areaRadiusFactor : 1.0f;
    p.stabilizeNormal =
        std::isfinite(stabilizeNormal) ? std::clamp(stabilizeNormal, 0.0f, 1.0f) : 0.0f;
    p.stabilizePlane =
        std::isfinite(stabilizePlane) ? std::clamp(stabilizePlane, 0.0f, 1.0f) : 0.0f;
    float3 axis(viewX, viewY, viewZ);
    const float len = axis.length();
    p.viewAxis = (std::isfinite(len) && len > 0.0f) ? axis * (1.0f / len)
                                                    : float3(0.0f, 0.0f, 1.0f);
    planeFrame_ = p;
    planeFrameState_.reset();
  }

  /** Declare the symmetry image of the next dab(s): the component signs that
   * map the primary image onto it, and whether it is a mirror. The primary is
   * (1, 1, 1, false). Only `@planeFrame` kernels read it. */
  void setImageSign(float sx, float sy, float sz, bool isMirror)
  {
    imageSign_ = float3(
        sx < 0.0f ? -1.0f : 1.0f, sy < 0.0f ? -1.0f : 1.0f, sz < 0.0f ? -1.0f : 1.0f);
    imageIsMirror_ = isMirror;
  }

  /** Per-call iterator factories used by CommandCtx::vertexIter/faceIter. The
   * vertex iterator is parameterized by the AccumMode policy and threaded the
   * displacement field (null/0 unless a from-base mode is active for this dab). */
  template <class AccMode>
  BasicVertexIter<AccMode> makeVertexIter(spatial::SpatialNode &node);
  BasicFaceIter makeFaceIter(spatial::SpatialNode &node)
  {
    return BasicFaceIter(node, *this);
  }

  /** Type-dispatch half of createCommandImpl, stateless so `queryBrushFlags` can
   * reach a kernel's manifest without a live stroke. `brushOrNull` is only used
   * by the extra-kernel registry (uniform defaults); a null one just means extra
   * kernels report unhandled. Returns false when `brushType` matched nothing.
   *
   * Both halves are generated: the built-ins from the kernels' @tool annotations
   * (brushes/generated/builtin_brushes.gen.h, graded by tests/test_brush_registry.cc)
   * and the extras from the configured extra-kernel dirs. Adding a brush touches
   * neither this function nor any other host conditional. */
  template <class AccMode>
  static bool createCommandSwitch(SculptBrushes brushType,
                                  bool csrNeighbors,
                                  Brush *brushOrNull,
                                  brush_command &def);

  /** Fill `def` for `brushType` under a fixed AccumMode policy. createCommand
   * calls this once for AccumLive, then again for AccumOrig when non-accumulate
   * is active and the brush is accumulable â€” the second call overwrites def.exec
   * with the AccumOrig kernel while keeping the rest of the (identical) manifest. */
  template <class AccMode>
  void createCommandImpl(SculptBrushes brushType, brush_command &def);

  brush_command createCommand(SculptBrushes brushType);

  /** A non-accumulating plane stroke gathering an AREA normal reads
   * stroke-start normals, so its command opts into the `.brush.orig.no` stamp.
   * Decided at command creation — before the stroke's first exec() — because
   * the stamp elision never re-stamps a vert, so a mid-stroke flip would leave
   * stale normals behind. Both factories (prepared and raw) call this. */
  void applyPlaneFrameOrigNormals(brush_command &def) const
  {
    if (def.usesPlaneFrame && nonAccum && def.accumulable && !def.relaxesBase &&
        planeFrame_.normal == PlaneNormalMode::Area)
    {
      def.needsOrigNormals = true;
    }
  }

  /** Lower bound on the node-filter radius for `brushType`. An `@unbounded`
   * kernel carries no distance falloff of its own â€” only `unboundedWindow`'s
   * cutoff at R = radius * unboundedExtent â€” so a filter sized from the brush
   * radius stops while the field is still live and the leaf boundary tears.
   * Hosts still own the radius (grab widens it by the drag); this only raises
   * it, and returns 0 for every ordinary kernel. */
  float filterRadiusFloor(SculptBrushes brushType)
  {
    ensureToolMemo(brushType);
    return floorMemoUnbounded_ ? brush->radius * brush->unboundedExtent : 0.0f;
  }

  void ensureToolMemo(SculptBrushes brushType);

  /** Whether `brushType` takes the from-orig fixed-region grab policy on this
   * stroke â€” the same condition createCommand uses to pick AccumOrigGrab. */
  bool grabAnchoredTool(SculptBrushes brushType)
  {
    ensureToolMemo(brushType);
    return floorMemoGrabCapable_ && anchoredGrab;
  }

  /** Drag-independent radius the pinned grab region is sized from: the falloff
   * radius, raised for an `@unbounded` kernel exactly as filterRadiusFloor does.
   * Hosts widen the radius they pass applyDab by the cumulative drag (the GPU
   * dab and the anchored preview snapshot need that); the pinned CPU region
   * must not follow it. */
  float grabPinRadius(SculptBrushes brushType)
  {
    return std::fmax(brush->radius, filterRadiusFloor(brushType));
  }

  /** Node set for one dab of a from-orig grab-class stroke.
   *
   * Such a stroke only ever moves verts within the falloff radius of its FIXED
   * anchor (it re-bases from each vert's stroke-start position), so the leaves
   * it needs include the first dab's leaves and newly reached support when
   * radius or symmetry changes. Retain reached leaves as they deform away from
   * the anchor; querying the drag-swept region would grow cost with drag distance.
   *
   * `hostRadius` is what the host asked for; it is only used by the dyntopo
   * fallback, which cannot pin anything. */
  void grabFilterNodes(float3 center,
                       float pinRadius,
                       float hostRadius,
                       Vector<spatial::SpatialNode *> &nodes);

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
   * element id â€” dyntopo leaves freelist gaps, so a live element's id can exceed
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

  /** Incoming host frame saved across a plane-frame substitution, restored
   * after the command's kernel so a later sub-command of the same program
   * (`[CLAY, SMOOTH]`) sees the cursor frame. */
  struct PlaneFrameScope {
    bool active = false;
    float3 pos{};
    float3 no{};
  };

  /** Area gather for a primary plane dab (Blender's
   * `calc_area_normal_and_center`): every vert of the candidate leaves within
   * `radius * normalRadiusFactor` (normal) / `radius * areaRadiusFactor`
   * (centre) of the cursor, in the vintage the stroke reads — live, or the
   * stroke-start position/normal (`co - disp`, `.brush.orig.no`) for verts
   * an earlier dab stamped when non-accumulating. The engine has no hidden
   * verts; the host strips them. */
  void queryPlaneFrameArea(brush_command &cmd,
                           std::span<spatial::SpatialNode *> nodes,
                           const float3 &cursor,
                           PlaneFrameAccum &accum)
  {
    const PlaneFramePolicy &p = planeFrame_;
    mesh::Mesh *m = nodes[0]->data->m;
    const float r = brush->radius;
    const float rN = r * p.normalRadiusFactor;
    const float rC = r * p.areaRadiusFactor;
    const float rMax = std::fmax(rN, rC);
    const bool fromBase = nonAccum && cmd.accumulable && !cmd.relaxesBase;

    // Normals are whatever the frame cadence left (updateQueries() skips the
    // normal phase): Blender's calc_area_normal reads the draw update's
    // normals too, so an accumulating gather lags one frame there as well.
    std::span<spatial::SpatialNode *> cand = nodes;
    if (rMax > brush->falloffSupportRadius(r)) {
      planeQueryNodes_.clear();
      tree->filterNodes(cursor, rMax, planeQueryNodes_);
      cand = std::span<spatial::SpatialNode *>(planeQueryNodes_.data(),
                                               planeQueryNodes_.size());
    }

    mesh::AttrData<float3> *dispVec = nullptr;
    mesh::AttrData<int> *dispGen = nullptr;
    mesh::AttrData<float3> *origNo = nullptr;
    if (fromBase) {
      // Look the stroke's TEMP attrs up without creating them: on the first
      // dab nothing is stamped yet and every read is live == original.
      if (m->v.attrs.has(mesh::AttrType::FLOAT3, ".brush.disp.vec") &&
          m->v.attrs.has(mesh::AttrType::INT, ".brush.disp.gen"))
      {
        dispVec = static_cast<mesh::AttrData<float3> *>(
            m->v.attrs.ensure(mesh::AttrType::FLOAT3, ".brush.disp.vec", false).data);
        dispGen = static_cast<mesh::AttrData<int> *>(
            m->v.attrs.ensure(mesh::AttrType::INT, ".brush.disp.gen", false).data);
        if (m->v.attrs.has(mesh::AttrType::FLOAT3, ".brush.orig.no")) {
          origNo = static_cast<mesh::AttrData<float3> *>(
              m->v.attrs.ensure(mesh::AttrType::FLOAT3, ".brush.orig.no", false).data);
        }
      }
    }

    // One partial per node, gathered in parallel like the kernel that follows
    // and reduced in node order, so the result is run-deterministic.
    accum.begin(cursor, p.viewAxis, rN, rC);
    planeAccums_.resize(cand.size());
    litestl::task::parallel_for(
        util::IndexRange(cand.size()),
        [&](util::IndexRange range) {
          for (int i : range) {
            PlaneFrameAccum &part = planeAccums_[i];
            part.begin(cursor, p.viewAxis, rN, rC);
            for (int v : cand[i]->data->unique_verts) {
              float3 co = m->v.co[v];
              float3 no = m->v.no[v];
              if (dispGen && dispGen->safe_get(v) == int(strokeGen)) {
                co -= dispVec->safe_get(v);
                if (origNo) {
                  no = origNo->safe_get(v);
                }
              }
              part.add(co, no);
            }
          }
        },
        1);
    for (const PlaneFrameAccum &part : planeAccums_) {
      accum.merge(part);
    }
  }

  /** Substitute the dab's surface frame for a `@planeFrame` command under a
   * non-default policy (plane_frame.h). Runs at the top of exec(), before the
   * stroke-start stamp — the gather reads unstamped verts as live == original
   * and stamped ones from the displacement base — and before the prepared
   * hooks that read the frame. A mirror image reflects the stroke's last
   * primary frame; a primary gathers, applies the original-normal/plane
   * toggles and the stabiliser, and records itself. When the centre moved off
   * the cursor the dab region is re-gathered around it (`nodes` is replaced by
   * the scratch list). Returns false when the dab must be skipped — an AREA
   * normal was requested and nothing usable was found. */
  bool resolvePlaneFrame(brush_command &cmd,
                         std::span<spatial::SpatialNode *> &nodes,
                         PlaneFrameScope &scope)
  {
    lastPlaneResolved = false;
    if (!cmd.usesPlaneFrame || !planeFrame_.active() || nodes.size() == 0 || !tree ||
        !brush)
    {
      return true;
    }
    const PlaneFramePolicy &p = planeFrame_;
    const float3 cursor = ctx.surfacePos;
    scope.active = true;
    scope.pos = ctx.surfacePos;
    scope.no = ctx.surfaceNo;

    PlaneFrameResolve resolve{p, planeFrameState_};
    float3 normal, center;
    if (!(imageIsMirror_ && resolve.mirror(imageSign_, normal, center))) {
      PlaneFrameAccum accum;
      const PlaneFrameAccum *accumPtr = nullptr;
      if (resolve.needsAreaQuery()) {
        queryPlaneFrameArea(cmd, nodes, cursor, accum);
        accumPtr = &accum;
      }
      if (!resolve.primary(ctx.surfaceNo, cursor, accumPtr, normal, center)) {
        return false;
      }
    }
    ctx.surfaceNo = normal;
    ctx.surfacePos = center;
    lastPlaneResolved = true;
    lastPlaneCursor = cursor;
    lastPlaneNormal = normal;
    lastPlaneCenter = center;

    // Blender re-gathers the PLANE brush's nodes around the resolved centre
    // (plane.cc, #123768): the falloff is centred there, so verts the cursor
    // filter never reached can be inside it.
    if (p.center == PlaneCenterMode::Area && (center - cursor).length() > 0.0f) {
      const float support =
          std::fmax(brush->falloffSupportRadius(brush->radius),
                    cmd.unbounded ? brush->radius * brush->unboundedExtent : 0.0f);
      planeFrameNodes_.clear();
      tree->filterNodes(center, support, planeFrameNodes_);
      nodes = std::span<spatial::SpatialNode *>(planeFrameNodes_.data(),
                                                planeFrameNodes_.size());
    }
    return true;
  }

  void exec(brush_command &cmd,
            std::span<spatial::SpatialNode *> nodes,
            std::span<const BrushAttrLayerOverride> attrOverrides = {})
  {
    // Plane-frame substitution first: it can replace the node list, and
    // everything below (attr binding, capture, stamps, hooks, kernel) must see
    // the final region and frame.
    PlaneFrameScope planeScope;
    if (!resolvePlaneFrame(cmd, nodes, planeScope)) {
      return;
    }
    struct PlaneFrameRestore {
      CommandCtxBase &ctx;
      const PlaneFrameScope &scope;
      ~PlaneFrameRestore()
      {
        if (scope.active) {
          ctx.surfacePos = scope.pos;
          ctx.surfaceNo = scope.no;
        }
      }
    } planeRestore{ctx, planeScope};

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
        if (preparedCavityActive && curCaptureTool == int(SculptBrushes::ENHANCE) &&
            entry.boundName == string(ENHANCE_DISP_ATTR))
        {
          preparedEnhance.beginGeometry(m);
          mesh::AttrRef ref(mesh::AttrType::FLOAT3, ENHANCE_DISP_ATTR);
          ref.flag = mesh::AttrFlag::TEMP | mesh::AttrFlag::NOCOPY;
          ref.data = preparedEnhance.active();
          attrBindingStorage.items.append(BrushAttrBinding{entry.handle, ref});
          continue;
        }
        mesh::AttrGroup *grp = attrGroupForDomain(m, entry.domain);
        if (!grp)
          continue;

        // An override redirects this handle to the user-selected "active"
        // layer (by index). Honour it only when the layer exists and its type
        // matches the handle's declared type (the TS attribute manager already
        // constrains categories by type, so a mismatch means a stale index â€”
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
          // the name first â€” ensure() of an existing layer won't realloc, but a
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
          // A first-ever poly-group layer means "every face in the host's
          // default set", not group 0 -- Mesh::ensureFaceGroups' rule, applied
          // here because the bind, not the host, created the layer.
          if ((entry.use & int(mesh::AttrUse::POLYGROUP)) &&
              entry.type == mesh::AttrType::INT && entry.domain == AttrElemDomain::Face &&
              m->default_group_id != 0)
          {
            auto *dd = static_cast<mesh::AttrData<int> *>(ref.data);
            for (int fi : m->f) {
              dd->materialize(fi);
              (*dd)[fi] = m->default_group_id;
            }
          }
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
          if (entry.handle == binding.handle && entry.kernelWrites) {
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
        if (scope.begin(
                *m,
                binding.ref.name,
                std::span<const int>(layerRegionVerts.data(), layerRegionVerts.size())))
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
     * to capture â€” skip it wholesale instead of re-checking its per-element
     * stamps. Leaf element sets only change under dyntopo (splits/merges are
     * driven by dyntopo adds/collapses), which disables the stamps. */
    ctx.preparedAttributeCapture = false;
    if (preparedCavityActive)
      for (const auto &attribute : cmd.attrs)
        ctx.preparedAttributeCapture |= attribute.kernelWrites;
    const bool stampCapture = ctx.meshLog && !ctx.preparedAttributeCapture && !stepHasDyntopo && curCaptureSlot >= 0 &&
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
    ctx.preparedAttributeCapture = false;
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
    // vertex's id can exceed v.count â€” a count-sized snapshot would be read
    // out-of-bounds for those verts/neighbors (garbage â†’ smooth spikes).
    if (cmd.needsCoPrev && nodes.size() > 0) {
      mesh::Mesh *m = nodes[0]->data->m;
      int cap = int(m->v.capacity());

      /* Incremental refresh: on a topology-stable stroke, kernels only move
       * verts of the node sets they were handed â€” so after the stroke's first
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

      // CSR neighbor source is static across the stroke â€” (re)build once,
      // single-threaded, before the parallel node loop reads it.
      if (effectiveNeighborMode() == NeighborMode::Csr) {
        m->topo_cache.ensureRing1(*m);
      }
    }

    // Stroke-start setup: ensure the `.brush.disp.*` TEMP attrs and stamp each
    // in-region vert under the current generation, single-threaded before the
    // parallel loop. A vert is stamped once per stroke (first contact) with a
    // zero displacement, so every from-base consumer derives the stroke-start
    // surface as `co - disp`.
    ctx.origNo = nullptr;
    ctx.dispVec = nullptr;
    ctx.dispGen = nullptr;
    ctx.strokeGen = 0;
    ctx.dabGen = nullptr;
    ctx.curDabGen = 0;
    // Grab-class brushes always need a stroke-start base (cmd.grabMode), even
    // when not `accumulable` (kelvinlet is @global) and regardless of the
    // ACCUMULATE flag â€” they deform from it via AccumOrigGrab (#35). A
    // @relaxation kernel never has one (it stays on AccumLive). Kernels may also
    // opt into the normal snapshot via cmd.needsOrigNormals.
    if ((cmd.grabMode || (nonAccum && cmd.accumulable && !cmd.relaxesBase) ||
         cmd.needsOrigNormals) &&
        nodes.size() > 0)
    {
      mesh::Mesh *m = nodes[0]->data->m;
      // TEMP + NOCOPY: stroke-transient, not undoable. NOCOPY keeps the meshlog
      // from snapshotting these during a logged dyntopo step â€” their pages are
      // materialized lazily (only brushed verts), so a mid-stroke edge collapse
      // would otherwise capture an unmaterialized page (null on WASM â†’ warn+skip;
      // a garbage pointer on native â†’ crash, ImmediateTODOs #37). Interpolation
      // stays enabled (no NOINTERP): a displacement field is correct to
      // interpolate onto split verts, unlike an absolute snapshot.
      mesh::AttrRef &dispRef =
          m->v.attrs.ensure(mesh::AttrType::FLOAT3, ".brush.disp.vec", false);
      dispRef.flag |= mesh::AttrFlag::TEMP | mesh::AttrFlag::NOCOPY;
      mesh::AttrRef &dispGenRef =
          m->v.attrs.ensure(mesh::AttrType::INT, ".brush.disp.gen", false);
      dispGenRef.flag |= mesh::AttrFlag::TEMP | mesh::AttrFlag::NOCOPY;
      ctx.dispVec = static_cast<mesh::AttrData<float3> *>(dispRef.data);
      ctx.dispGen = static_cast<mesh::AttrData<int> *>(dispGenRef.data);
      if (cmd.needsOrigNormals) {
        mesh::AttrRef &noRef =
            m->v.attrs.ensure(mesh::AttrType::FLOAT3, ".brush.orig.no", false);
        noRef.flag |= mesh::AttrFlag::TEMP | mesh::AttrFlag::NOCOPY;
        ctx.origNo = static_cast<mesh::AttrData<float3> *>(noRef.data);
      }
      ctx.strokeGen = strokeGen;

      // Grab-class first-touch stamp (#35): ensure `.brush.dab.gen` + pass the
      // per-dab counter to the kernel. Pages are pre-materialized below (with the
      // disp stamp) so the parallel kernel only reads/writes existing slots.
      if (cmd.grabMode) {
        mesh::AttrRef &dabRef =
            m->v.attrs.ensure(mesh::AttrType::INT, ".brush.dab.gen", false);
        dabRef.flag |= mesh::AttrFlag::TEMP | mesh::AttrFlag::NOCOPY;
        ctx.dabGen = static_cast<mesh::AttrData<int> *>(dabRef.data);
        ctx.curDabGen = dabGen;
      }

      // Lazy first-touch stamping, O(region): the stamp is zero, since an
      // untouched vert's base *is* its current position. Verts dyntopo creates
      // mid-stroke inherit an interpolated disp (and gen), so they arrive
      // already stamped and are skipped.
      //
      // Normals are looser than positions â€” the spatial halo refresh can rewrite
      // a vert's normal one fan-ring ahead of the brush â€” so when a kernel opted
      // into orig normals, each region leaf's SKIRT verts (its neighbor-owned
      // fan, exactly the set the halo can reach ahead of the region) are stamped
      // along with its own. The halo only refreshes fans of already-moved
      // (= already-stamped-leaf) verts, so this stays ahead of it without ever
      // sweeping the whole mesh.
      auto stampBase = [&](int v) {
        ctx.dispGen->materialize(v);
        if ((*ctx.dispGen)[v] != int(strokeGen)) {
          ctx.dispVec->materialize(v);
          (*ctx.dispVec)[v] = float3(0.0f, 0.0f, 0.0f);
          if (ctx.origNo) {
            ctx.origNo->materialize(v);
            (*ctx.origNo)[v] = m->v.no[v];
          }
          (*ctx.dispGen)[v] = int(strokeGen);
        }
      };
      // Same elision as the capture walk above: this visits every element of
      // each leaf and writes a stroke-fixed value, so re-walking a leaf a later
      // dab still covers is pure overhead once it is fully stamped.
      const int stampOpts = (ctx.origNo ? 1 : 0) | (ctx.dabGen ? 2 : 0);
      const bool elideStamp = !stepHasDyntopo && strokeGen != 0;
      for (auto *node : nodes) {
        if (elideStamp && node->baseStampGen == strokeGen &&
            node->baseStampOpts == stampOpts)
        {
          continue;
        }
        for (int v : node->data->unique_verts) {
          if (ctx.dabGen) {
            ctx.dabGen->materialize(v);
          }
          stampBase(v);
        }
        if (ctx.origNo) {
          for (const auto &tri : node->data->skirt_tris) {
            for (int k = 0; k < 3; k++) {
              stampBase(m->c.v[tri.c[k]]);
            }
          }
        }
        node->baseStampGen = elideStamp ? strokeGen : 0;
        node->baseStampOpts = elideStamp ? stampOpts : -1;
      }
    }

    // Prepared hooks see this command's initialized displacement base. Bind the
    // executor-owned active column without creating or modifying a mesh layer.
    if (preparedCavityActive && curCaptureTool == int(SculptBrushes::ENHANCE) &&
        nodes.size())
    {
      auto *m = nodes[0]->data->m;
      m->topo_cache.ensureRing1(*m);
      PreparedEnhanceCache::Entry *entry = nullptr;
      for (auto *node : nodes) {
        for (int v : node->data->unique_verts) {
          float3 contact = m->v.co[v];
          if ((cmd.grabMode || (nonAccum && cmd.accumulable && !cmd.relaxesBase)) &&
              ctx.dispVec && ctx.dispGen &&
              ctx.dispGen->safe_get(v) == int(ctx.strokeGen))
            contact -= ctx.dispVec->safe_get(v);
          if (preparedCavityContact(
                  *brush, false, contact, ctx.surfacePos, ctx.surfaceNo))
          {
            if (!entry)
              entry = &preparedEnhance.entry(brush->enhance_rings, brush->enhance_inner);
            preparedEnhance.fill(*entry, m, v);
          } else {
            preparedEnhance.write(v, float3(0, 0, 0));
          }
        }
      }
    }

    // View-normal automasking is DYNAMIC: resolve the stroke's params (shared
    // camera ray, limit, falloff) onto the ctx; strength() evaluates the factor
    // against each vertex's live normal on every call. No cache, no stamp
    // ordering, no per-vertex ray history.
    ctx.viewNormal = viewNormalParamsFor(*brush);

    // Cavity automask pre-fill (see automask.h for the caching contract): the
    // BFS ring-blur is too expensive per dab, so each in-region vert's 0..1
    // cavity factor is stamped once per stroke into `.brush.automask.cavity`,
    // single-threaded, before the parallel kernel loop reads it via strength().
    ctx.automaskFactor = nullptr;
    ctx.automaskEnabled = false;
    if (preparedCavityActive && brush->automask_cavity && nodes.size()) {
      auto *m = nodes[0]->data->m;
      preparedCavity.beginGeometry(nodes[0]->data->m, m->topo_stamp, int(m->v.capacity()));
      PreparedCavityCache::Entry *entry = nullptr;
      for (auto *node : nodes) {
        for (int v : node->data->unique_verts) {
          float3 contact = m->v.co[v];
          if ((cmd.grabMode || (nonAccum && cmd.accumulable && !cmd.relaxesBase)) &&
              ctx.dispVec && ctx.dispGen &&
              ctx.dispGen->safe_get(v) == int(ctx.strokeGen))
            contact -= ctx.dispVec->safe_get(v);
          if (preparedCavityContact(
                  *brush, cmd.unbounded, contact, ctx.surfacePos, ctx.surfaceNo))
          {
            if (!entry)
              entry = &preparedCavity.entry(*brush);
            preparedCavity.fill(*entry, MeshCavitySrc{m}, v);
          } else {
            preparedCavity.write(v, 1.0f);
          }
        }
      }
      ctx.automaskFactor = preparedCavity.active();
      ctx.automaskEnabled = entry != nullptr;
    } else if (brush->automask_cavity && nodes.size() > 0) {
      mesh::Mesh *m = nodes[0]->data->m;
      if (!m->topo_frozen || m->topo_cache.valid(*m)) {
        m->topo_cache.ensureRing1(*m);
      }
      if (m->topo_cache.valid(*m)) {
        mesh::AttrRef &facRef =
            m->v.attrs.ensure(mesh::AttrType::FLOAT, ".brush.automask.cavity", false);
        facRef.flag |= mesh::AttrFlag::TEMP | mesh::AttrFlag::NOCOPY;
        mesh::AttrRef &genRef =
            m->v.attrs.ensure(mesh::AttrType::INT, ".brush.automask.gen", false);
        genRef.flag |= mesh::AttrFlag::TEMP | mesh::AttrFlag::NOCOPY;
        auto *fac = static_cast<mesh::AttrData<float> *>(facRef.data);
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
            fac->materialize(v);
            if (strokeGen == 0 || (*gen)[v] != int(strokeGen)) {
              (*fac)[v] = cavityFactor(m, v, cp, scr);
              (*gen)[v] = int(strokeGen);
            }
          }
        }
        ctx.automaskFactor = fac;
        ctx.automaskEnabled = true;
      }
    }

    // Skip leaves emptied of verts: heavy in-stroke collapse can leave a zero-vert
    // leaf whose loose AABB still overlaps the brush sphere, so a vertex iterator on
    // it wild-reads unique_verts.begin() (brush_iterators.h: "never on empty node").
    // affected_verts is a sticky hint list: nothing clears it until the normals
    // pass consumes it, so by late stroke it holds every vert of every prior
    // dab. Remember where each node's list ends, so border propagation walks
    // only what this dab adds.
    affectedBase_.resize(nodes.size());
    for (int i : IndexRange(nodes.size())) {
      affectedBase_[i] = int(nodes[i]->affected_verts.size());
    }

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
    // moved border vert â€” flag those too, appending the vert as a normals hint.
    if (nodes.size() > 1) {
      mesh::Mesh *mm = nodes[0]->data->m;
      // Stamp the dab's moved verts instead of hashing them into a Set: the
      // border pass only ever asks a membership question, and at a large brush
      // radius that set costs ~50k inserts plus ~33k probes per dab.
      if (movedStamp_.size() < mm->v.capacity()) {
        movedStamp_.resize(mm->v.capacity());
        std::fill(movedStamp_.begin(), movedStamp_.end(), 0u);
        movedStampGen_ = 0;
      }
      if (++movedStampGen_ == 0) {
        std::fill(movedStamp_.begin(), movedStamp_.end(), 0u);
        movedStampGen_ = 1;
      }
      size_t movedCount = 0;
      for (int i : IndexRange(nodes.size())) {
        auto &av = nodes[i]->affected_verts;
        for (int j = affectedBase_[i]; j < int(av.size()); j++) {
          movedStamp_[av[j]] = movedStampGen_;
          movedCount++;
        }
      }
      if (movedCount > 0) {
        for (auto *node : nodes) {
          if (node->flag & spatial::Spatial_RegenTris) {
            continue; // tris are stale; the pending full regen covers this leaf
          }
          // The candidates are exactly this leaf's foreign verts, so scan the
          // tris only to (re)build that cache â€” not once per dab.
          tree->ensure_border_cache(node);
          bool touched = false;
          for (int v : node->data->foreign_verts) {
            if (movedStamp_[v] == movedStampGen_) {
              node->affected_verts.append(v);
              touched = true;
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
     * may have moved lives in `nodes` â€” queue them for the next needsCoPrev
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
     * (the smooth family) qualify. Dyntopo strokes keep topology live â€”
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
          reprojectUvsWithCapture(m,
                                  std::span<const int>(rverts.data(), rverts.size()),
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
      // flag Spatial_UpdateGPUGeom, which skips them â€” without this the
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

  /** Whether a stroke with this brush must keep the mesh's disk/radial link
   * pages live. A brush that touches no live TOPO link during a dab lets the
   * mesh sit topology-frozen â€” the pages dropped â€” for the whole stroke.
   *
   * Three kernel facts answer it, all reflected out of the DSL by the registry
   * generators rather than enumerated here (a hand-kept enum list is how a new
   * kernel silently gets a frozen stroke, and reading a dropped page is a
   * heap-layout-dependent UAF, not a wrong pixel â€” graded by
   * tests/test_brush_live_links.cc):
   *
   *  - a `for_neighbor` loop, which reads the 1-ring â€” but only its live-disk
   *    instantiation walks links; the CSR one reads MeshTopoCache::ring1;
   *  - `@fulltopo`, i.e. a host pre-pass that walks live topology every dab
   *    (the cross-field and enhance-details passes) â€” unconditional, since the
   *    thaw is what makes a stroke-start CSR snapshot go stale;
   *  - a `face` stage, dispatched per face, which reaches its verts through the
   *    live face loop.
   *
   * Called per dab, so every query is a switch on an int and allocates nothing.
   */
  bool brushNeedsLiveLinks(SculptBrushes brushType) const
  {
    const int id = int(brushType);
    if (builtinBrushFullTopo(id) || extraBrushFullTopo(id) || builtinBrushFaceMode(id) ||
        extraBrushFaceMode(id))
    {
      return true;
    }
    return (builtinBrushUsesForNeighbor(id) || extraBrushUsesForNeighbor(id)) &&
           neighborMode != NeighborMode::Csr;
  }

  /** The boundary-aware smooth brush reads the lazily-derived
   * `.boundary.vert.class`. Fold any pending boundary edits (seam marking,
   * poly-group paint) into it once at stroke start, while topology links are
   * live â€” recomputeDirty walks the disk/radial cycles and would touch freed
   * pages under frozen topology.
   *
   * Gated on m->boundaryDirty: when nothing changed since the last recompute
   * (the common case â€” e.g. plain smoothing with no boundaries marked) this is a
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
  static bool isCommonFloatProp(const string &name);

  static const BrushUniformManifestEntry *findUniformEntry(brush_command &cmd,
                                                           const string &name);

  /** Validate the active brush's uniform dynamics once at stroke start, scoped to
   * its manifest. Catches the shared-`Brush`-struct traps (a stray dynamic left
   * over from another kernel, a dynamic on a `@static`/non-float uniform), an
   * unbaked 1-entry response curve, an out-of-range authored default, and an
   * inverted/NaN `@range`. Returns a structured result; the caller skips the
   * stroke on `!ok` so a misconfigured binding never mutates the mesh. */
  UniformValidationResult validateUniformDynamics(brush_command &cmd);

  /** --- Wave 5: per-kernel uniform manifest query for the TS bridge -----------
   * The binding runtime can't marshal a JS string into a `util::string` method
   * arg, so the bridge enumerates the active brush's manifest by index instead
   * of by name. queryUniformManifest caches the kernel's manifest (and registers
   * its props so propDynamics(name) resolves) and returns the entry count;
   * queriedUniformEntry exposes each entry as a bound read-only struct; the
   * *UniformDynamics methods resolve the index -> name and delegate to the Brush
   * by-name dynamics API (Wave 3). */

  int queryUniformManifest(int brushType);

  BrushScalarResult
  readUniformScalarChecked(int token, int uniformIndex, int scalarType, bool evaluate)
  {
    string name;
    int error = checkedUniformName(token, uniformIndex, scalarType, name);
    if (error) {
      return {error, 0};
    }
    return brush->readScalarChecked(name, scalarType, evaluate);
  }

  int writeUniformScalarChecked(int token, int uniformIndex, int scalarType, double value);

  int configureUniformDynamicChecked(
      int token, int uniformIndex, int scalarType, int device, int mode, float factor);

  int enableUniformDynamicChecked(
      int token, int uniformIndex, int scalarType, int device, int enabled);

  int moveUniformDynamicChecked(
      int token, int uniformIndex, int scalarType, int device, int index);

  int clearUniformDynamicsChecked(int token, int uniformIndex, int scalarType);

  int replaceUniformDynamicTableChecked(int token,
                                        int uniformIndex,
                                        int scalarType,
                                        int device,
                                        util::Vector<float> &samples);

  int setUniformDynamicSampleChecked(int token,
                                     int uniformIndex,
                                     int scalarType,
                                     int device,
                                     int index,
                                     int count,
                                     float value);

  int replaceUniformDynamicsChecked(int token,
                                    int uniformIndex,
                                    int scalarType,
                                    util::Vector<int> &devices,
                                    util::Vector<int> &modes,
                                    util::Vector<float> &factors,
                                    util::Vector<int> &enabled,
                                    util::Vector<int> &offsets,
                                    util::Vector<float> &samples);

  int replaceUniformResponseDynamicsChecked(int token,
                                            int uniformIndex,
                                            int scalarType,
                                            util::Vector<int> &devices,
                                            util::Vector<int> &modes,
                                            util::Vector<float> &factors,
                                            util::Vector<int> &enabled,
                                            util::Vector<int> &offsets,
                                            util::Vector<float> &samples,
                                            util::Vector<int> &kinds,
                                            util::Vector<double> &parameters);

  bool registrationSucceeded()
  {
    if (lastRegistration.error == props::PropError::ERROR_NONE) {
      return true;
    }
    fprintf(stderr,
            "uniform declaration '%s': registration error %d\n",
            lastRegistration.name.c_str(),
            int(lastRegistration.error));
    return false;
  }

  static bool createDeclarationCommand(SculptBrushes type, brush_command &command)
  {
    Brush scratch;
    return createCommandSwitch<AccumLive>(type, true, &scratch, command);
  }

  bool prepareProgramDeclarations(BrushProgram *program)
  {
    if (!program || !brush || !brush->props.struct_def) {
      lastRegistration = {props::PropError::ERROR_INVALID_OWNER, "program"};
      return registrationSucceeded();
    }
    Vector<props::ScalarDeclaration> declarations;
    for (const auto &entry : program->commands) {
      if (entry.dynamicsOverrides.size() || entry.scalarOverrides.size() ||
          entry.cavityCurveOverride.size())
      {
        lastRegistration = {props::PropError::ERROR_INVALID_VALUE,
                            "typed program requires resolved execution"};
        return registrationSucceeded();
      }
      brush_command command;
      if (!createDeclarationCommand(entry.type, command)) {
        lastRegistration = {props::PropError::ERROR_NOT_EXISTS, "kernel"};
        return registrationSucceeded();
      }
      command.appendScalarDeclarations(declarations);
    }
    lastRegistration = brush->props.struct_def->registerScalars(
        {declarations.data(), declarations.size()});
    return registrationSucceeded();
  }

  BrushUniformManifestEntry *queriedUniformEntry(int idx);

  void clearUniformDynamics(int idx);
  void addUniformDynamic(int idx, int deviceType, int mixMode, float mixFactor);
  void setUniformDynamicSample(int idx, int deviceType, int i, int n, float value);

  // Update the per-dab stroke frame: the stroke tangent (this dab origin minus
  // the previous primary dab center, reflected for a mirror image) and, for
  // the oriented Box falloffs, their primary axis. Must run before the kernel
  // executes. Shared by both the single-brush and program dab paths so
  // Box/wing-scrape orientation is identical regardless of entry point.
  void updateStrokeFrame(float3 origin)
  {
    // Under mirror symmetry the host owns strokeDir (it reflects the primary
    // tangent per image); the shared ring buffer would otherwise interleave
    // primary + mirror origins. Derive from the buffer only when host didn't.
    if (!brush->strokeDirHostSet) {
      if (imageIsMirror_) {
        // A mirror image runs between two primaries; its tangent is the
        // primary's reflected, never the step from the primary's origin.
        brush->strokeDir = float3(primaryStrokeDir_[0] * imageSign_[0],
                                  primaryStrokeDir_[1] * imageSign_[1],
                                  primaryStrokeDir_[2] * imageSign_[2]);
      } else {
        if (!hasPrimaryOrigin_) {
          // First dab of the stroke: no tangent exists yet, and the previous
          // stroke's is not one (wing scrape would lean its wings along it).
          brush->strokeDir = float3(0.0f, 0.0f, 0.0f);
        } else {
          float3 d = origin - primaryOrigin_;
          float len = d.length();
          if (len > 1e-7f) {
            brush->strokeDir = d / len;
          }
        }
        primaryStrokeDir_ = brush->strokeDir;
        primaryOrigin_ = origin;
        hasPrimaryOrigin_ = true;
      }
    }
    // The bridge only flips the shape to Box; the direction is owned here so it
    // stays consistent with wing-scrape's strokeDir.
    if (brush->falloff_shape == FalloffShape::Box ||
        brush->falloff_shape == FalloffShape::RoundedBox) {
      brush->falloff_dir = brush->strokeDir;
    }
  }

  void execBrush(Mesh *m,
                 SculptBrushes brushType,
                 Vector<spatial::SpatialNode *> *nodes,
                 float3 origin,
                 float3 normal)
  {
    if (!preflightRaw(brushType))
      return;
    auto cmd = createCommand(brushType);

    // Enter/leave frozen-topology mode per dab (both calls early-out when
    // already in the target state, so this is cheap to re-check every dab).
    // Note: this is the C++ executor path only; the GPU dispatch in gpu_stroke
    // has its own neighbor handling and is unaffected.
    const BrushHooks *hooks = brushHooksFor(brushType);
    BrushHookCtx hookCtx{*this, m, nodes, brush, isFirstOfStep};
    if (nodes->size() > 0) {
      if (hooks && hooks->stepPreFreeze && isFirstOfStep) {
        hooks->stepPreFreeze(hookCtx);
      }
      if (brushNeedsLiveLinks(brushType) || keepTopoThawed) {
        if (m->topo_frozen) {
          m->thawTopo();
        }
      } else if (!m->topo_frozen) {
        m->freezeTopo();
      }
    }

    // Per-dab host pre-passes (brush_hooks.cc): ENHANCE's region fill and
    // FEATURE_ALIGN's cross-field update â€” the latter previously ran only on
    // the execProgram path. Topology is live for both (brushNeedsLiveLinks).
    if (hooks && hooks->dabPre && nodes->size() > 0) {
      hooks->dabPre(hookCtx);
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

    if (hooks && hooks->dabPost) {
      hooks->dabPost(hookCtx);
    }
  }

  /** Validate raw source semantics without publishing or mutating geometry. */
  bool preflightRaw(SculptBrushes type);
  bool preflightRawProgram(BrushProgram *program);
  bool supportsResolved(SculptBrushes type);
  bool supportsResolvedProgram(BrushProgram *program);
  bool createPreparedCommand(SculptBrushes type, brush_command &command);

  /** Checked CPU mesh entry with evaluated spherical node selection. */
  props::ScalarRegistrationResult applyResolvedDab(SculptBrushes brushType,
                                                   float3 center,
                                                   float3 normal,
                                                   bool validateOnly = false,
                                                   bool grabAdd = false);

  /** Preflight the whole program and restore scalar working state after execution. */
  props::ScalarRegistrationResult
  applyResolvedProgram(BrushProgram *program,
                       float3 center,
                       float3 normal,
                       bool validateOnly = false,
                       bool grabAdd = false,
                       dyntopo::DynTopoParams *topology = nullptr,
                       float topologyRadius = 0,
                       uint32_t topologySeed = 0);

  /** Run one hook phase over a program's commands, once per distinct hook fn
   * (BSMOOTH and FEATURE_ALIGN share the boundary-class refresh, which must
   * not run twice for a program listing both). */
  void
  runProgramHooks(BrushProgram *prog, BrushHookFn BrushHooks::*phase, BrushHookCtx &hc)
  {
    BrushHookFn seen[8] = {};
    int nseen = 0;
    for (auto &entry : prog->commands) {
      const BrushHooks *hooks = brushHooksFor(entry.type);
      BrushHookFn fn = hooks ? hooks->*phase : nullptr;
      if (!fn) {
        continue;
      }
      bool dup = false;
      for (int i = 0; i < nseen; i++) {
        dup = dup || seen[i] == fn;
      }
      if (dup) {
        continue;
      }
      if (nseen < 8) {
        seen[nseen++] = fn;
      }
      fn(hc);
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
    if (!preflightRawProgram(prog))
      return;
    if (!prog->commands.size())
      return;
    if (!prepareProgramDeclarations(prog))
      return;

    // Topology freeze/thaw is decided once for the whole dab: thaw if *any*
    // sub-command needs live disk links (a live-disk smooth), otherwise freeze
    // for the program's duration. Mixed programs (e.g. DRAW + SMOOTH) thaw,
    // which is harmless for the link-agnostic commands.
    if (nodes->size() > 0) {
      bool needsLive = false;
      for (auto &entry : prog->commands) {
        if (brushNeedsLiveLinks(entry.type))
          needsLive = true;
      }
      mesh::Mesh *m = (*nodes)[0]->data->m;
      // Stroke-start hooks (e.g. the boundary-class refresh) must precede the
      // freeze below â€” they need live links.
      if (isFirstOfStep) {
        BrushHookCtx hookCtx{*this, m, nodes, brush, isFirstOfStep};
        runProgramHooks(prog, &BrushHooks::stepPreFreeze, hookCtx);
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

    // Per-dab host pre-passes (brush_hooks.cc), once per dab rather than per
    // sub-command: ENHANCE's region fill, FEATURE_ALIGN's cross-field update.
    // Topology is live for these tools (brushNeedsLiveLinks).
    if (nodes->size() > 0) {
      BrushHookCtx hookCtx{*this, (*nodes)[0]->data->m, nodes, brush, isFirstOfStep};
      runProgramHooks(prog, &BrushHooks::dabPre, hookCtx);
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

      if (const BrushHooks *hooks = brushHooksFor(entry.type); hooks && hooks->dabPost) {
        BrushHookCtx hookCtx{*this,
                             nodes->size() ? (*nodes)[0]->data->m : nullptr,
                             nodes,
                             brush,
                             isFirstOfStep};
        hooks->dabPost(hookCtx);
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

    // The host hands over the brush radius; a box tip (Clay Strips) reaches
    // its corners at sqrt(2) of that across the surface, and an unrefined
    // corner shows as a coarse rim on every strip. Cover the whole footprint.
    if (brush && std::isfinite(radius)) {
      radius = brush->falloffFootprintRadius(radius);
    }

    // Locked bases (multires level meshes) never retopologize: a level mesh is
    // derived from the grid store, so changing its topology strands every
    // level's displacement and the host's grid map (mesh.h topoLocked).
    if (m->topoLocked) {
      return 0;
    }

    // Displacement-base coherence: tell the remesh ops the active stroke's gen so
    // the tangential smooth resamples the field it slides verts through. Keyed on
    // the attr actually existing (the brush's stroke-start pre-pass creates it),
    // so it can never claim a gen no command stamped.
    params->dispGen =
        m->v.attrs.has(mesh::AttrType::FLOAT3, ".brush.disp.vec") ? strokeGen : 0;

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
    // composing â€” never alias it across dabs.
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
      auto mlCK = combined.onCornerKill, spCK = sp->onCornerKill;
      combined.onCornerKill = [mlCK, spCK](int c) {
        if (mlCK)
          mlCK(c);
        if (spCK)
          spCK(c); // flags the skirt of the corner vert's leaf
      };
      cb = &combined;
    } else {
      cb = ml ? ml : sp;
    }

    // Round-0 seed: verts of the in-region leaves, so the dab is O(brush region)
    // rather than O(mesh). The caller owns the spatial query; dyntopo stays
    // spatial-free and just receives the set.
    Vector<spatial::SpatialNode *> &hit = dynTopoNodes_;
    hit.clear();
    tree->filterNodes(center, radius, hit);

    Vector<int> &seedVerts = dynTopoSeed_;
    seedVerts.clear();
    size_t nseed = 0;
    for (spatial::SpatialNode *n : hit) {
      nseed += n->unique_verts().size();
    }
    seedVerts.ensure_capacity(nseed);
    for (spatial::SpatialNode *n : hit) {
      for (int v : n->unique_verts()) {
        seedVerts.append(v);
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

  /** One unified brush dab â€” the single dab sequence shared by every client
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
    if (!preflightRawProgram(prog) || !prepareProgramDeclarations(prog)) {
      return -1;
    }
    int topoApplied = 0;
    if (params) {
      topoApplied = applyDynTopoDab(center, radius, params, seed);
    }
    // `radius` here is the node-filter radius only (the falloff uses
    // brush->radius). A from-orig grab-class program ignores the host's
    // drag-widened radius and takes the pinned region instead â€” the executor
    // owns that policy (see grabFilterNodes).
    float pinRadius = 0.0f;
    for (const BrushCommandEntry &e : prog->commands) {
      radius = std::fmax(radius, filterRadiusFloor(e.type));
      if (grabAnchoredTool(e.type)) {
        pinRadius = std::fmax(pinRadius, grabPinRadius(e.type));
      }
    }
    Vector<spatial::SpatialNode *> &nodes = dabNodes_;
    nodes.clear();
    if (pinRadius > 0.0f) {
      grabFilterNodes(center, pinRadius, radius, nodes);
    } else {
      tree->filterNodes(center, radius, nodes);
    }
    lastDabNodeCount = int(nodes.size());
    if (nodes.size() > 0) {
      execProgram(prog, &nodes, center, normal);
      if (!lastUniformValidationOk())
        return -1;
    }
    if (params && meshLog) {
      /* Per-dab seal: deactivate this dab's topo chunk NOW (finalizing its
         Created end-states) so the next dab's topo captures land in a fresh
         chunk. Without it one step-wide chunk captures an Existed vert's
         begin-body at its FIRST TOPO touch â€” mid-step for verts the brush
         deformed in earlier dabs â€” and that stale body, sitting at position
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
    if (!preflightRaw(brushType))
      return -1;
    mesh::Mesh *m = tree->m;
    int topoApplied = 0;
    if (params) {
      topoApplied = applyDynTopoDab(center, radius, params, seed);
    }
    // `radius` is the node-filter radius only; grab-class strokes take the
    // pinned region (see the BrushProgram overload).
    radius = std::fmax(radius, filterRadiusFloor(brushType));
    Vector<spatial::SpatialNode *> &nodes = dabNodes_;
    nodes.clear();
    if (grabAnchoredTool(brushType)) {
      grabFilterNodes(center, grabPinRadius(brushType), radius, nodes);
    } else {
      tree->filterNodes(center, radius, nodes);
    }
    lastDabNodeCount = int(nodes.size());
    if (nodes.size() > 0) {
      execBrush(m, brushType, &nodes, center, normal);
      if (!lastUniformValidationOk())
        return -1;
    }
    if (params && meshLog) {
      /* Per-dab seal: deactivate this dab's topo chunk NOW (finalizing its
         Created end-states) so the next dab's topo captures land in a fresh
         chunk. Without it one step-wide chunk captures an Existed vert's
         begin-body at its FIRST TOPO touch â€” mid-step for verts the brush
         deformed in earlier dabs â€” and that stale body, sitting at position
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
  void beginPreviewDab(float3 center, float radius);

  /** Add another region to the currently-open preview session (a mirror
   * image of the same driver tick) without resetting it â€” the whole group
   * rolls back together via one rollbackPreviewDab(). See
   * MeshLog::extendPreviewDab. */
  void extendPreviewDab(float3 center, float radius);

  /** Undo the most recent preview dab (topology pop-and-undo + position/attr
   * restore) without closing the step. See MeshLog::rollbackPreviewDab. */
  void rollbackPreviewDab();

  /** True if a preview snapshot is pending rollback (diagnostic / debug-app use). */
  bool previewActive() const
  {
    return meshLog && meshLog->previewActive();
  }

  /** Keep the pending preview dab's effect and drop its snapshot bookkeeping.
   * Call once at the true end of an Anchored/Drag Dot stroke (after the last
   * dab, instead of a paired rollback) so previewActive() doesn't leak into
   * the next stroke's step. See MeshLog::commitPreviewDab. */
  void commitPreviewDab();

  void beginStep(bool hasDyntopo);

  void endStep();
};

#include "brush_executor_templates.inl"


#include "brush_metadata.h"

} // namespace sculptcore::brush
