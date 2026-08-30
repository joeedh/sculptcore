#pragma once

#include "accum_mode.h"
#include "automask.h"
#include "brush.h"
#include "brush_concepts.h"
#include "litestl/math/matrix.h"
#include "litestl/math/vector.h"
#include "mesh/mesh.h"
#include "meshlog/meshlog.h"
#include "spatial/node.h"

using namespace sculptcore::spatial;
using namespace litestl::math;
using namespace litestl::util;
using namespace sculptcore::mesh;

namespace sculptcore::brush {
using litestl::math::float2;
using litestl::math::float3;
using litestl::math::float4;
using litestl::math::mat4;

// === DSL attribute bindings (boundary-conditions wave) ===
//
// A kernel that declares `attr <domain> <type> <name>` fields opts those mesh
// attribute layers into the per-dab binding set. Codegen emits one
// BrushAttrManifestEntry per declared attr into BrushCommandDef::attrs; the
// executor resolves each to a live mesh layer (ensuring it exists) once per dab
// and exposes it through CommandCtxBase::boundAttr<T>().

// Which mesh element domain an attribute lives on (runtime mirror of the
// compiler-side sbrush::AttrDomain — kept separate so the runtime never depends
// on the DSL compiler headers).
enum class AttrElemDomain : int { Vertex, Face, Edge, Corner };
} // namespace sculptcore::brush

namespace litestl::binding {
template <> struct Binder<sculptcore::brush::AttrElemDomain> {
  static const BindingBase *bind();
};
} // namespace litestl::binding

namespace sculptcore::brush {

// Codegen-emitted descriptor of one attribute a kernel touches.
struct BrushAttrManifestEntry {
  string handle;    // DSL field name (the handle)
  string boundName; // fixed layer, or "" => handle
  mesh::AttrType type = mesh::AttrType::FLOAT;
  AttrElemDomain domain = AttrElemDomain::Vertex;
  // The executor must ensure the layer exists before binding it. True for every
  // attr entry — a read-only handle still needs storage to read from. It says
  // nothing about whether the kernel stores to the layer; that is kernelWrites.
  bool materialize = false;
  // The kernel body stores to this handle, inferred by codegen from the stage
  // bodies (sbrush::brushScanMemberWrites). This is the bit that decides where a
  // write has to land — a read-only handle like bsmooth's `vclass` is
  // host-pre-pass scratch and needs no write-back path at all.
  bool kernelWrites = false;
  // DSL `@use(<category>)`: the mesh::AttrUse bit a host retargets this handle
  // by (0 = untagged / engine-internal). Only meaningful when boundName is
  // empty — a fixed layer name is not retargetable.
  int use = 0;

  // Bound read-only so the TS bridge can enumerate a kernel's attr manifest by
  // index and retarget each retargetable handle at the active layer for its
  // `use` category. Returned by pointer from CommandExecutor::queriedAttrEntry,
  // never marshalled by value.
  static litestl::binding::types::Struct<BrushAttrManifestEntry> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<BrushAttrManifestEntry> *st = new types::Struct<BrushAttrManifestEntry>(
        "sculptcore::brush::BrushAttrManifestEntry", sizeof(BrushAttrManifestEntry));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, handle);
    BIND_STRUCT_MEMBER(st, boundName);
    BIND_STRUCT_MEMBER(st, type);
    BIND_STRUCT_MEMBER(st, domain);
    BIND_STRUCT_MEMBER(st, materialize);
    BIND_STRUCT_MEMBER(st, kernelWrites);
    BIND_STRUCT_MEMBER(st, use);
    return st;
  }
};

// Host-visible summary of one kernel's codegen-set policy bits, queried by tool
// id without building a stroke. Every field is derived from the kernel's
// BrushCommandDef / attr manifest, so a host never hardcodes per-brush
// conditionals. See documentation/plans/brushMetadataToTS-2026-07-28.md.
struct BrushDefFlags {
  bool needsCoPrev = false;
  bool accumulable = false;
  bool relaxesBase = false;
  bool grabModeCapable = false;
  bool unbounded = false;
  bool incremental = false;
  bool writesMask = false;
  bool writesColor = false;
  bool faceMode = false;
  bool readsVclass = false;

  static litestl::binding::types::Struct<BrushDefFlags> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<BrushDefFlags> *st = new types::Struct<BrushDefFlags>(
        "sculptcore::brush::BrushDefFlags", sizeof(BrushDefFlags));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, needsCoPrev);
    BIND_STRUCT_MEMBER(st, accumulable);
    BIND_STRUCT_MEMBER(st, relaxesBase);
    BIND_STRUCT_MEMBER(st, grabModeCapable);
    BIND_STRUCT_MEMBER(st, unbounded);
    BIND_STRUCT_MEMBER(st, incremental);
    BIND_STRUCT_MEMBER(st, writesMask);
    BIND_STRUCT_MEMBER(st, writesColor);
    BIND_STRUCT_MEMBER(st, faceMode);
    BIND_STRUCT_MEMBER(st, readsVclass);
    return st;
  }
};

// Codegen-emitted descriptor of one `uniform` a kernel declares. The executor
// uses the active brush's manifest to register props, apply device dynamics in
// loadProps, and validate dynamic bindings before a stroke. See
// documentation/plans/sbrush-dynamic-uniforms.md.
struct BrushUniformManifestEntry {
  string name;           // DSL uniform field name
  bool isFloat = false;  // scalar float — the only dynamic-capable kind
  bool dynamic = false;  // may be driven by device dynamics (float, non-@static)
  float def = 0.0f;      // authored default (DSL `= <n>`), Wave 1
  bool hasRange = false; // DSL `@range(min, max)` present, Wave 1
  float rangeMin = 0.0f;
  float rangeMax = 0.0f;
  // Extra-kernel store uniforms: Brush.namedFloats index, set via
  // setNamedFloat(slot, v). -1 = backed by a Brush member (the default).
  int storeSlot = -1;

  // Bound read-only so the TS bridge (Wave 5) can enumerate the active brush's
  // manifest by index and read each entry's name/range/dynamic flag. Returned
  // by pointer from CommandExecutor::queriedUniformEntry — never marshalled by
  // value, so a copy constructor is unneeded.
  static litestl::binding::types::Struct<BrushUniformManifestEntry> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<BrushUniformManifestEntry> *st =
        new types::Struct<BrushUniformManifestEntry>(
            "sculptcore::brush::BrushUniformManifestEntry",
            sizeof(BrushUniformManifestEntry));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, name);
    BIND_STRUCT_MEMBER(st, isFloat);
    BIND_STRUCT_MEMBER(st, dynamic);
    BIND_STRUCT_MEMBER(st, def);
    BIND_STRUCT_MEMBER(st, hasRange);
    BIND_STRUCT_MEMBER(st, rangeMin);
    BIND_STRUCT_MEMBER(st, rangeMax);
    BIND_STRUCT_MEMBER(st, storeSlot);
    return st;
  }
};

// A resolved binding: a kernel handle -> the live mesh AttrRef for this dab.
struct BrushAttrBinding {
  string handle;
  mesh::AttrRef ref;
};

struct BrushAttrBindings {
  Vector<BrushAttrBinding> items;
  void clear()
  {
    items.clear();
  }
  const mesh::AttrRef *find(const char *handle) const
  {
    for (const auto &b : items) {
      if (b.handle == string(handle))
        return &b.ref;
    }
    return nullptr;
  }
};

struct CommandCtxBase {
  float2 mouse;
  float3 mousePos;
  float3 surfacePos; // pos of vertex at center of brush dot
  float3 surfaceNo;  // normal of vertex at center of brush dot
  float3 mouseDir;
  mat4 renderMatrix;

  bool isFirstOfStep = false;

  meshlog::MeshLog *meshLog = nullptr;
  mesh::Mesh *m = nullptr;

  // Pre-dab snapshot of the whole mesh's vertex positions, owned by the
  // executor and populated before the parallel per-node loop when a brush
  // needs it (see BrushCommandDef::needsCoPrev). for_neighbor reads neighbor
  // positions from here so smoothing is Jacobi — order-independent and
  // race-free across the parallel node loop, and bit-modulo-fp identical to
  // the GPU kernel (which reads its own co_prev binding).
  litestl::util::Vector<litestl::math::float3> *co_prev = nullptr;

  // Resolved DSL attribute bindings for this dab (owned by the executor),
  // looked up by handle in generated kernels via boundAttr<T>().
  const BrushAttrBindings *attrBindings = nullptr;

  // Displacement base (the `.brush.disp.*` TEMP attrs, see
  // plans/2026-07-26-0909-brush-displacement-base-attribute.md): `dispVec`
  // accumulates what the brush has moved each vert this stroke, so the
  // stroke-start base is `co - disp` and advects with the surface under dyntopo
  // instead of being pinned to a stale snapshot. Valid iff dispGen[v] ==
  // strokeGen; null when no from-base consumer is active.
  mesh::AttrData<litestl::math::float3> *dispVec = nullptr;
  mesh::AttrData<int> *dispGen = nullptr;
  uint32_t strokeGen = 0;

  // Stroke-start NORMAL snapshot (`.brush.orig.no`), stamped at first touch
  // alongside dispVec and sharing its generation, only for kernels that opt in
  // via BrushCommandDef::needsOrigNormals (no consumer yet — the view-normal
  // mask evaluates live normals dynamically); null otherwise.
  mesh::AttrData<litestl::math::float3> *origNo = nullptr;

  // Grab-class symmetry first-touch stamp (#35): `dabGen` is the `.brush.dab.gen`
  // TEMP attr, a vert counts as written this dab iff dabGen[v] == curDabGen.
  // Drives the AccumOrigGrab re-base/add choice. Null/0 outside a grab dab.
  mesh::AttrData<int> *dabGen = nullptr;
  uint32_t curDabGen = 0;

  // Automasking (automask.h), two independent contributors:
  //  - Cavity is CACHED: `automaskFactor` is the `.brush.automask.cavity` TEMP
  //    attr, filled host-side once per vertex per stroke (keyed by strokeGen via
  //    `.brush.automask.gen`) — the BFS ring-blur is too expensive per dab, and
  //    freezing it at first contact keeps the mask from chasing the deforming
  //    surface. Null / automaskEnabled=false when inactive.
  //  - View-normal is DYNAMIC: `viewNormal` holds this dab's resolved params
  //    (the acting symmetry image's own reflected ray); strength() evaluates
  //    viewNormalFactor against the vertex's LIVE normal on every call, so
  //    there is no per-vertex ray history to go stale.
  mesh::AttrData<float> *automaskFactor = nullptr;
  bool automaskEnabled = false;
  ViewNormalParams viewNormal;

  // Fetch a bound non-bool attribute's data by kernel handle. Returns nullptr
  // when unbound (an optional layer that was absent); write kernels always
  // declare their target, so it's non-null there.
  template <typename T> mesh::AttrData<T> *boundAttr(const char *handle) const
  {
    if (!attrBindings)
      return nullptr;
    const mesh::AttrRef *ref = attrBindings->find(handle);
    return ref ? static_cast<mesh::AttrData<T> *>(ref->data) : nullptr;
  }

  // Resolved AttrRef for a bound DSL attribute handle (the BrushAttrLayerOverride
  // target), or null when unbound. Undo-capture codegen snapshots through this so
  // it records the layer actually written, not the declared name.
  const mesh::AttrRef *boundAttrRef(const char *handle) const
  {
    return attrBindings ? attrBindings->find(handle) : nullptr;
  }

  CommandCtxBase() = default;
  CommandCtxBase(const CommandCtxBase &) = default;
  CommandCtxBase(CommandCtxBase &&) = default;
  CommandCtxBase &operator=(const CommandCtxBase &) = default;
  CommandCtxBase &operator=(CommandCtxBase &&) = default;
};

template <CommandTypes TYPES> struct CommandCtx : public CommandCtxBase {
  /** The executor type, reachable from the CTX parameter of BrushCommandDef
   * (which needs its node_type for the Pre/Post signatures). */
  using types = TYPES;
  using node_type = typename TYPES::node_type;

  Brush &brush;
  node_type &node;
  TYPES &executor;

  CommandCtx(const CommandCtxBase &base, node_type &node, TYPES &executor, Brush &brush)
      : CommandCtxBase(base), node(node), executor(executor), brush(brush)
  {
  }
  CommandCtx(const CommandCtx &b)
      : CommandCtxBase(b), node(b.node), executor(b.executor), brush(b.brush)
  {
  }

  // Per-call iterator factories. The vertex iterator is parameterized by the
  // AccumMode policy so a generated kernel reads stroke-start positions
  // (AccumOrig) or live positions (AccumLive) with no per-vertex branch.
  template <class AccMode> auto vertexIter(node_type &node)
  {
    return executor.template makeVertexIter<AccMode>(node);
  }
  auto faceIter(node_type &node)
  {
    return executor.makeFaceIter(node);
  }
  /** Spatial + scalar term only: slider x distance falloff x brush texture,
   * invert-signed. The per-vertex masking factors live in automasks()/masks() —
   * a kernel that wants them multiplies one of those in. Kernels with unbounded
   * support (whose field is its own falloff) must not call this at all; they
   * multiply sampleBrushTex() explicitly to keep texture parity. */
  float strength(float3 co)
  {
    float t = 1.0f - std::min(brush.falloffDist(co - surfacePos, surfaceNo), 1.0f);
    float s = brush.strength * brush.falloffEval(t);
    // Node iteration hands kernels every vert of every overlapping leaf, so
    // out-of-radius verts land here with s == 0 — skip the texture eval they
    // would zero anyway (runtime texture programs make it the dominant cost).
    if (s == 0.0f) {
      return 0.0f;
    }
    s *= sampleBrushTex(co, surfaceNo);
    return brush.invert ? -s : s;
  }

  /** The *automatic* masks — cavity automask x view-normal — which a kernel has
   * no other way to reach. `v` is the mesh vertex index, threaded from the kernel
   * loop by the intrinsic's `$v` placeholder; face stages pass -1, where both
   * factors are identity. */
  float automasks(int v)
  {
    float s = 1.0f;
    if (automaskEnabled && automaskFactor && v >= 0) {
      s *= (*automaskFactor)[v];
    }
    // The live normal comes from the executor (domain seam): the mesh
    // executor reads ctx.m->v.no, the grids executor its domain normals.
    if (viewNormal.enabled && v >= 0) {
      if (const float3 *n = executor.liveVertNoPtr(*this, v)) {
        s *= viewNormalFactor(*n, viewNormal);
      }
    }
    return s;
  }

  /** Every mask: automasks() x the painted mask. What deforming and painting
   * kernels want. `mask` is the kernel's own live value, threaded by the
   * intrinsic's `$vm` placeholder, so a kernel that writes v.mask still sees
   * its local copy rather than stale storage. Face stages pass 0. */
  float masks(int v, float mask)
  {
    return automasks(v) * (1.0f - mask);
  }

  /** C1 cutoff window for an `@unbounded` field: 1 inside 0.8R, smoothstepped
   * to exactly 0 at R = radius * unboundedExtent. The host filters spatial
   * nodes against the same R, so the field vanishes before the region boundary
   * and no seam can form on a leaf edge. Extent <= 0 disables it. */
  float unboundedWindow(float3 co)
  {
    float R = brush.radius * brush.unboundedExtent;
    if (!(R > 0.0f)) {
      return 1.0f;
    }
    float d = (co - surfacePos).length();
    float t = std::clamp((R - d) / (0.2f * R), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  }

  /** Sample the brush texture at world point `co` with surface normal `no`. A
   * runtime texture program takes precedence and consumes `no` directly (its
   * eval does its own point mapping — `coord_space` is bitmap-path-only). The
   * bitmap path maps co to UV per `brush.coord_space`; there `no` is unused —
   * every mode (including Projected) reads the brush-center ctx surfaceNo so
   * the C++ and WGSL paths key off the same value; the arg is kept for DSL
   * signature parity with the generated kernel. Returns 1.0 with nothing
   * bound (kernels multiply by this unconditionally). */
  float sampleBrushTex(float3 co, float3 no)
  {
    if (const TextureProgram *tp = brush.texture_program) {
      const float P[3] = {co[0], co[1], co[2]};
      const float N[3] = {no[0], no[1], no[2]};
      const float *params =
          brush.texture_params.size() > 0 ? brush.texture_params.data() : nullptr;
      if (tp->usesMap) {
        // TexEvalCtx.map_matrix shares mat4's flat layout (texture_eval.h).
        TexEvalCtx tctx;
        const float *src = &renderMatrix[0][0];
        for (int i = 0; i < 16; i++) {
          tctx.map_matrix[i] = src[i];
        }
        return tp->eval(P, N, params, &tctx);
      }
      return tp->eval(P, N, params, nullptr);
    }
    float2 uv;
    switch (brush.coord_space) {
    case TexCoordSpace::Global:
      // The bitmap square spans world [-1, 1]^2 (the host bake domain) and
      // tiles beyond it — unbounded sources must keep sampling past the
      // baked square instead of clamp-streaking at its edge.
      uv = float2{fractf(co[0] * 0.5f + 0.5f), fractf(co[1] * 0.5f + 0.5f)};
      break;
    case TexCoordSpace::ViewPlane: {
      uv = sampleViewUv(co);
      break;
    }
    case TexCoordSpace::ViewRepeat: {
      // Aspect-correct x into viewport-height units so tiles stay square,
      // then tile: tex_repeat is tiles per viewport height.
      uv = sampleViewUv(co);
      uv = float2{fractf(uv[0] * viewAspect() * brush.tex_repeat),
                  fractf(uv[1] * brush.tex_repeat)};
      break;
    }
    case TexCoordSpace::StrokeCurved:
      uv = brush.sampleStrokeUV(co);
      break;
    case TexCoordSpace::Projected: {
      // Project onto the tangent plane at the brush center, normalized so
      // the texture tile spans the brush circle (uv 0..1 across the
      // diameter, centered on the dab). The reference axis pick (and thus
      // the basis) must match the WGSL branch bit-for-bit in structure, so
      // both flip on the same |n.z| < 0.999 test against the shared ctx
      // surfaceNo.
      float3 n = surfaceNo.normalized();
      float3 ref =
          std::abs(n[2]) < 0.999f ? float3{0.0f, 0.0f, 1.0f} : float3{1.0f, 0.0f, 0.0f};
      float3 t1 = ref.cross(n).normalized();
      float3 t2 = n.cross(t1);
      float3 rel = co - surfacePos;
      float inv_d = 1.0f / (brush.radius > 1e-6f ? 2.0f * brush.radius : 1.0f);
      uv = float2{rel.dot(t1) * inv_d + 0.5f, rel.dot(t2) * inv_d + 0.5f};
      break;
    }
    }
    return brush.sampleTexBilinear(uv);
  }

  // View-pinned texture UV: perspective-project `co` through renderMatrix
  // (world -> clip) and remap NDC to [0,1] across the viewport. The w divide
  // is what makes a perspective view sample sensibly; kept in lockstep with
  // the WGSL emitter's brush_sample_tex.
  float2 sampleViewUv(float3 co) const
  {
    float4 p = renderMatrix * float4{co[0], co[1], co[2], 1.0f};
    float w = std::abs(p[3]) > 1e-6f ? p[3] : 1.0f;
    return float2{p[0] / w * 0.5f + 0.5f, p[1] / w * 0.5f + 0.5f};
  }

  // Repeat-wrap into [0, 1); identical formula to WGSL fract() so backends
  // agree bitwise.
  static float fractf(float x)
  {
    return x - std::floor(x);
  }

  // Viewport width/height from renderMatrix (world -> clip): its x/y rows
  // are the projection diagonal times unit view rows, so |row1|/|row0| is
  // w/h for perspective and ortho alike. 1.0 for identity/degenerate.
  float viewAspect()
  {
    const float *m = &renderMatrix[0][0];
    float r0 = float3{m[0], m[1], m[2]}.length();
    float r1 = float3{m[4], m[5], m[6]}.length();
    return (r0 > 1e-12f && r1 > 1e-12f) ? r1 / r0 : 1.0f;
  }
};

enum _BrushFlags { None = 0, Serial = 1 << 0 };
MAKE_FLAGS_CLASS(BrushFlags, _BrushFlags, int);

template <typename CTX> struct BrushCommandDef {
  /** The executor's spatial unit (SpatialNode for the mesh path, the grid
   * executor's leaf node for the grids path) — the Pre/Post span type. */
  using node_type = typename CTX::node_type;

  // Optional `host` stage from the DSL — runs once per dab on CPU before
  // any per-node work. Used to mutate ctx state / populate query buffers
  // that the per-vertex stage then reads. Never lowered to GPU backends.
  std::function<void(CommandCtxBase &, Brush &)> execHost;
  std::function<void(CommandCtxBase &, std::span<node_type *>)> execPre;
  std::function<void(CTX &)> exec;
  std::function<void(CommandCtxBase &, std::span<node_type *>)> execPost;
  BrushFlags flags = BrushFlags::None;
  // Set by codegen for brushes that use for_neighbor: the executor snapshots
  // the mesh's vertex positions into ctx.co_prev before the per-node loop.
  bool needsCoPrev = false;
  // Set by codegen: true unless the kernel is @paint (writes an attribute) or
  // @unbounded (anchored field), for which from-base re-derivation is
  // meaningless. Eligible for non-accumulate mode; see plans/nonAccumMode.md.
  bool accumulable = false;
  // Set by codegen from `@relaxation`: the kernel relaxes the field it edits
  // toward a neighborhood mean instead of displacing it, so it must not
  // contribute to `.brush.disp.vec`. The executor keeps it on AccumLive even in a
  // non-accumulate stroke — reading and writing the live surface is exactly "move
  // co, leave disp alone". Hosts also read it as "inverting this diverges".
  bool relaxesBase = false;
  // Set by codegen from `@grabmode`, meaning the kernel supports running the grab
  // policy. The stroke, not the kernel, decides whether it actually runs; that
  // decision is made in `CommandExecutor::anchoredGrab`.
  bool grabModeCapable = false;
  // Set by codegen from `@unbounded`: the field carries no distance falloff of
  // its own, only `unboundedWindow`'s cutoff at R = radius * unboundedExtent.
  // The executor floors the node-filter radius at R (`filterRadiusFloor`).
  bool unbounded = false;
  // Set by codegen from `@incremental`: a stage input is a per-dab delta rather
  // than an absolute stroke quantity, so the host must feed a step-since-last-dab
  // rather than an anchor-relative drag. Implies !accumulable, but the converse
  // does not hold — @paint and @unbounded also clear accumulable.
  bool incremental = false;
  // Set by codegen when the kernel declares a `face` stage: it is dispatched
  // per-face rather than per-vertex.
  bool faceMode = false;
  // Set by codegen when a stage assigns `v.mask`. `mask` is a builtin Vertex
  // field, not an `attr`, so it leaves no entry in `attrs` to key off.
  bool writesMask = false;
  // Set by the executor: this stroke deforms from the stroke-start position with
  // the AccumOrigGrab policy + a fixed region, independent of the ACCUMULATE flag
  // (#35). Drives the `.brush.disp.*` + `.brush.dab.gen` stamps even when
  // `accumulable` is false. True iff `grabModeCapable && anchoredGrab`.
  bool grabMode = false;
  // Opt-in: also snapshot each vert's stroke-start NORMAL into `.brush.orig.no`
  // (alongside the disp stamp, extended over each region leaf's skirt so the
  // capture stays ahead of the spatial halo normal refresh). No kernel consumes
  // it yet; default off.
  bool needsOrigNormals = false;
  // Attribute layers this kernel reads/writes, emitted by codegen. The executor
  // resolves these to live mesh layers and binds them before the per-node loop.
  Vector<BrushAttrManifestEntry> attrs;
  // Scalar uniforms this kernel declares, emitted by codegen. The executor
  // registers these as props, applies device dynamics in loadProps, and
  // validates dynamic bindings before a stroke.
  Vector<BrushUniformManifestEntry> uniforms;
  // Generated per-brush prop wiring (see sbrush-dynamic-uniforms plan).
  // registerProps registers this kernel's scalar-float uniforms as props with
  // their authored defaults (idempotent — guarded by name); loadUniformProps
  // resolves them each dab (applying device dynamics) into the cached Brush
  // members the kernel reads. The fixed common props live on Brush directly.
  std::function<void(props::StructDef &)> registerProps;
  std::function<void(Brush &, props::DeviceInputCtx *)> loadUniformProps;
};

} // namespace sculptcore::brush
