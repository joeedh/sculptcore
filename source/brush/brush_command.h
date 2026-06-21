#pragma once

#include "accum_mode.h"
#include "brush.h"
#include "brush_concepts.h"
#include "mesh/mesh.h"
#include "meshlog/meshlog.h"
#include "litestl/math/matrix.h"
#include "litestl/math/vector.h"
#include "spatial/node.h"

using namespace sculptcore::spatial;
using namespace litestl::math;
using namespace litestl::util;
using namespace sculptcore::mesh;

namespace sculptcore::brush {
using litestl::math::float2;
using litestl::math::float3;
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

// Codegen-emitted descriptor of one attribute a kernel touches.
struct BrushAttrManifestEntry {
  string handle;                                 // DSL field name (the handle)
  string boundName;                              // fixed layer, or "" => handle
  mesh::AttrType type = mesh::AttrType::FLOAT;
  AttrElemDomain domain = AttrElemDomain::Vertex;
  bool write = false;                            // writes => ensure materialized
};

// Codegen-emitted descriptor of one `uniform` a kernel declares. The executor
// uses the active brush's manifest to register props, apply device dynamics in
// loadProps, and validate dynamic bindings before a stroke. See
// documentation/plans/sbrush-dynamic-uniforms.md.
struct BrushUniformManifestEntry {
  string name;             // DSL uniform field name
  bool isFloat = false;    // scalar float — the only dynamic-capable kind
  bool dynamic = false;    // may be driven by device dynamics (float, non-@static)
  float def = 0.0f;        // authored default (DSL `= <n>`), Wave 1
  bool hasRange = false;   // DSL `@range(min, max)` present, Wave 1
  float rangeMin = 0.0f;
  float rangeMax = 0.0f;

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
  void clear() { items.clear(); }
  const mesh::AttrRef *find(const char *handle) const
  {
    for (const auto &b : items) {
      if (b.handle == string(handle)) return &b.ref;
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

  // Non-accumulate cache (see plans/nonAccumMode.md). When a stroke runs in
  // non-accumulate mode, `origCo`/`origGen` are the `.brush.orig.*` TEMP vertex
  // attrs that snapshot each vert's stroke-start position; a vert's cached pos
  // is valid iff origGen[v] == strokeGen. Null in accumulate mode.
  mesh::AttrData<litestl::math::float3> *origCo = nullptr;
  mesh::AttrData<int> *origGen = nullptr;
  uint32_t strokeGen = 0;

  // Grab-class symmetry first-touch stamp (#35): `dabGen` is the `.brush.dab.gen`
  // TEMP attr, a vert counts as written this dab iff dabGen[v] == curDabGen.
  // Drives the AccumOrigGrab re-base/add choice. Null/0 outside a grab dab.
  mesh::AttrData<int> *dabGen = nullptr;
  uint32_t curDabGen = 0;

  // Fetch a bound non-bool attribute's data by kernel handle. Returns nullptr
  // when unbound (an optional layer that was absent); write kernels always
  // declare their target, so it's non-null there.
  template <typename T> mesh::AttrData<T> *boundAttr(const char *handle) const
  {
    if (!attrBindings) return nullptr;
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
  Brush &brush;
  spatial::SpatialNode &node;
  TYPES &executor;

  CommandCtx(const CommandCtxBase &base,
             spatial::SpatialNode &node,
             TYPES &executor,
             Brush &brush)
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
  template <class AccMode> auto vertexIter(spatial::SpatialNode &node)
  {
    return executor.template makeVertexIter<AccMode>(node);
  }
  auto faceIter(spatial::SpatialNode &node) { return executor.makeFaceIter(node); }
  float strength(float3 co)
  {
    float t = 1.0f - std::min(brush.falloffDist(co - surfacePos), 1.0f);
    float s = brush.strength * brush.falloffEval(t);
    return brush.invert ? -s : s;
  }

  // Sample the brush texture at world point `co` with surface normal `no`,
  // mapping to UV per `brush.coord_space`. The `no` arg is unused — every mode
  // (including Projected) reads the brush-center ctx surfaceNo so the C++ and
  // WGSL paths key off the same value; the arg is kept for DSL signature parity
  // with the generated kernel. Returns 1.0 with no texture bound (kernels
  // multiply by this unconditionally).
  float sampleBrushTex(float3 co, float3 no)
  {
    (void)no;
    float2 uv;
    switch (brush.coord_space) {
    case TexCoordSpace::Global:
      uv = float2{co[0], co[1]};
      break;
    case TexCoordSpace::ViewPlane: {
      float3 p = renderMatrix * co;
      uv = float2{p[0], p[1]};
      break;
    }
    case TexCoordSpace::ViewRepeat: {
      float3 p = renderMatrix * co;
      uv = float2{p[0] * brush.tex_repeat, p[1] * brush.tex_repeat};
      break;
    }
    case TexCoordSpace::StrokeCurved:
      uv = brush.sampleStrokeUV(co);
      break;
    case TexCoordSpace::Projected: {
      // Project onto the tangent plane at the brush center. The reference
      // axis pick (and thus the basis) must match the WGSL branch bit-for-bit
      // in structure, so both flip on the same |n.z| < 0.999 test against the
      // shared ctx surfaceNo.
      float3 n = surfaceNo.normalized();
      float3 ref = std::abs(n[2]) < 0.999f ? float3{0.0f, 0.0f, 1.0f}
                                           : float3{1.0f, 0.0f, 0.0f};
      float3 t1 = ref.cross(n).normalized();
      float3 t2 = n.cross(t1);
      float3 rel = co - surfacePos;
      uv = float2{rel.dot(t1), rel.dot(t2)};
      break;
    }
    }
    return brush.sampleTexBilinear(uv);
  }
};

enum _BrushFlags { None = 0, Serial = 1 << 0 };
MAKE_FLAGS_CLASS(BrushFlags, _BrushFlags, int);

template <typename CTX> struct BrushCommandDef {
  // Optional `host` stage from the DSL — runs once per dab on CPU before
  // any per-node work. Used to mutate ctx state / populate query buffers
  // that the per-vertex stage then reads. Never lowered to GPU backends.
  std::function<void(CommandCtxBase &, Brush &)> execHost;
  std::function<void(CommandCtxBase &, std::span<SpatialNode *>)> execPre;
  std::function<void(CTX &)> exec;
  std::function<void(CommandCtxBase &, std::span<SpatialNode *>)> execPost;
  BrushFlags flags = BrushFlags::None;
  // Set by codegen for brushes that use for_neighbor: the executor snapshots
  // the mesh's vertex positions into ctx.co_prev before the per-node loop.
  bool needsCoPrev = false;
  // Set by codegen: true for local deformation brushes (neither @global nor
  // @paint), which are eligible for non-accumulate mode. See
  // plans/nonAccumMode.md.
  bool accumulable = false;
  // Set by the executor (not codegen) for grab-class brushes (grab / kelvinlet):
  // they always deform from the stroke-start position with the AccumOrigGrab
  // policy + a fixed region, independent of the ACCUMULATE flag (#35). Drives the
  // `.brush.orig.*` + `.brush.dab.gen` stamps even when `accumulable` is false
  // (kelvinlet is @global).
  bool grabMode = false;
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
