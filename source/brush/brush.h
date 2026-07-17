#pragma once
#include "../props/prop_curve.h"
#include "../props/prop_dynamics.h"
#include "../props/prop_struct.h"
#include "litestl/binding/binding.h"
#include "litestl/math/vector.h"
#include "litestl/util/compiler_util.h"
#include "litestl/util/vector.h"

#include <array>
#include <cmath>
#include <limits>

#include "props.h"

namespace sculptcore::brush {

// Falloff curve shapes selectable per brush. The three analytic kinds
// inline a closed form; `Curve` reads `Brush::falloff_curve` as a
// 256-entry LUT with linear interpolation. `t` is the normalized 0..1
// centerwise input — 1 at the brush center, 0 at the radius. Smoothstep
// is the historical default and keeps regression dumps bit-identical
// when no `set_falloff` runs.
enum class FalloffKind : unsigned char {
  Smoothstep = 0,
  Linear = 1,
  Gaussian = 2,
  Curve = 3,
};

// Spatial metric mapping a vertex offset from the brush center to the
// normalized 0..1 distance fed into the falloff curve. This is the DSL
// plan's `Falloff` tagged-union discriminant, orthogonal to the curve
// shape (`FalloffKind`):
//   Spherical — euclidean radius (historical default, bit-identical).
//   Cube      — max(|dx|,|dy|,|dz|), the legacy SQUARE-brush metric.
//   Linear    — distance projected onto `falloff_dir` (stroke-line falloff).
//   Box       — oriented cuboid: max-norm in an orthonormal frame built from
//               `falloff_dir` (primary axis = stroke direction), with
//               independent per-axis half-extents in `falloff_extent`.
enum class FalloffShape : unsigned char {
  Spherical = 0,
  Cube = 1,
  Linear = 2,
  Box = 3,
};
} // namespace sculptcore::brush

namespace litestl::binding {
template <> struct Binder<sculptcore::brush::FalloffKind> {
  static const BindingBase *bind();
};
template <> struct Binder<sculptcore::brush::FalloffShape> {
  static const BindingBase *bind();
};
} // namespace litestl::binding

namespace sculptcore::brush {
using litestl::math::float3;
using litestl::math::float4;
using litestl::util::StrLiteral;

// Mapping from a world-space sample point to brush-texture UV. Orthogonal
// to the texture data itself; the discriminant rides in BrushUniforms so
// the WGSL `brush_sample_tex` mirrors the same branches.
//   Global     — uv = co.xy (world plane; stroke-independent, the test mode).
//   ViewPlane  — uv = (renderMatrix * co).xy (texture pinned to the view).
//   ViewRepeat — ViewPlane scaled by `tex_repeat` (tiled across the view).
//   StrokeCurved — uv = (arc length along the stroke, lateral offset from it);
//                  reads the StrokePath ring buffer of recent dab centers.
//   Projected  — project (co - surfacePos) onto the brush-center tangent plane;
//                uv = its coordinates in a deterministic orthonormal basis built
//                from surfaceNo. The one mode that actually consumes the surface
//                normal arg threaded through sampleBrushTex.
enum class TexCoordSpace : unsigned char {
  Global = 0,
  ViewPlane = 1,
  ViewRepeat = 2,
  StrokeCurved = 3,
  Projected = 4,
};

inline constexpr int kFalloffCurveSize = 256;

// One recorded stroke-dab center for the StrokePath ring buffer. `arclen` is
// the cumulative world-space distance from the first sample to this one, so a
// projection onto the polyline yields a monotonic curvilinear coordinate.
struct StrokeSample {
  float3 pos{0, 0, 0};
  float3 normal{0, 0, 1};
  float arclen = 0.0f;
};

// Capacity of the per-stroke StrokePath ring buffer. STROKE_CURVED projects
// onto at most this many recent dab centers; 64 covers a long stroke at the
// default spacing without an allocation. Mirrored as the WGSL storage-buffer
// length bound by the (future) dispatcher.
inline constexpr int kStrokePathMax = 64;

// Stable small ids for the float props the bridge configures across the TS
// boundary. Used instead of `util::string` prop-name args: the TS binding
// runtime can't marshal a JS string into a bound `util::string` parameter
// (no String copy-ctor / cstring path — it expects a pre-built String handle).
// The C++ side maps the id back to the prop name (string literals never cross
// the boundary). Mirror in sculptcore_bindings.ts (`BrushProp`).
enum class BrushProp : int {
  Strength = 0,
  Radius = 1,
  Autosmooth = 2,
  Planeoff = 3,
  Spacing = 4,
};
inline const char *brushPropName(int propId)
{
  switch (propId) {
  case 0:
    return "strength";
  case 1:
    return "radius";
  case 2:
    return "autosmooth";
  case 3:
    return "planeoff";
  case 4:
    return "spacing";
  default:
    return "";
  }
}

struct Brush {
  props::StructProp props;
  props::DeviceInputCtx deviceInputCtx;

  float strength = 1;
  float radius = 1;
  /* Fraction of `radius` between successive brush dabs along a stroke. */
  float spacing = 0.25f;
  /* Plane-brush offset along surfaceNo as a fraction of radius — places the
   * projection plane above/below the stroke surface point for the clay-family
   * (plane) kernels. Unused by the non-plane brushes. */
  float planeoff = 0.0f;
  /* Autosmooth amount. Synced from the TS brush for prop parity / inheritance,
   * but not read C++-side yet: the chained SMOOTH command is built bridge-side
   * (buildBrushProgram reads the TS value and emits a SMOOTH BrushProgram entry
   * — see brush_executor.h). 0 disables the chained smooth. */
  float autosmooth = 0.0f;
  /* Plane-brush active side: +1 pulls verts below the projection plane up onto
   * it (clay / fill), -1 pulls verts above it down onto it (scrape). Set per
   * tool by the bridge; read by the plane kernel as `ctx.brush.planeSide`. */
  float planeSide = 1.0f;
  bool invert = false;
  FalloffKind falloff_kind = FalloffKind::Smoothstep;
  FalloffShape falloff_shape = FalloffShape::Spherical;
  // Direction for `FalloffShape::Linear` and the primary axis of
  // `FalloffShape::Box` (expected normalized). Unused by the other shapes.
  // Default +Z keeps the value well-defined.
  float3 falloff_dir{0, 0, 1};
  // Per-axis half-extents (×radius) for `FalloffShape::Box`, in the oriented
  // frame built from `falloff_dir` (extent[0] is along the stroke direction).
  // {1,1,1} makes Box an oriented cube. Unused by the other shapes.
  float3 falloff_extent{1, 1, 1};

  // Stroke tangent (current dab origin − previous dab center), set host-side by
  // the executor each dab. Drives wing-scrape wing orientation and can feed the
  // oriented Box falloff via `falloff_dir`. Default +Z keeps it well-defined.
  float3 strokeDir{0, 0, 1};

  // Wing-scrape: half-angle (radians) of each wing plane off the surface, plus
  // the two wing-plane normals (surfaceNo rotated ±wingAngle about strokeDir),
  // recomputed per dab by the wingscrape kernel's host stage.
  float wingAngle = 0.3f;
  float3 wingNormalA{0, 0, 1};
  float3 wingNormalB{0, 0, 1};

  // Poly-group paint: the group id the polygroup kernel writes to faces under
  // the brush this stroke (read as the `activeGroup` uniform). Set host-side
  // per stroke; a plain member loadProps leaves untouched.
  int activeGroup = 1;

  // Color paint: the target color the color kernel lerps toward (read as the
  // `brushColor` uniform). Synced from the TS brush per stroke.
  float4 brushColor{1, 1, 1, 1};

  // Color paint blend mode (read as the `mixMode` uniform; see ColorMixModes /
  // color.sbrush). Synced from the TS brush per stroke. 0 = MIX (straight lerp).
  int mixMode = 0;

  // Brush texture (grayscale, row-major, `tex_width * tex_height` floats).
  // Empty means "no texture": `sampleTexBilinear` returns 1.0 so a kernel
  // multiplying by the sample is a no-op. `coord_space` maps a sample point
  // to UV; `tex_repeat` tiles the UV under `ViewRepeat`.
  int tex_width = 0;
  int tex_height = 0;
  litestl::util::Vector<float> tex_pixels;
  TexCoordSpace coord_space = TexCoordSpace::Global;
  float tex_repeat = 1.0f;

  // Ring buffer of recent stroke-dab centers, driving STROKE_CURVED texture
  // mapping. Pushed host-side as dabs advance (CommandExecutor::execBrush),
  // reset at the start of each stroke (CommandExecutor::beginStep). Uniform-
  // resident on CPU; the WGSL emitter mirrors it as a storage buffer.
  StrokeSample strokePath[kStrokePathMax];
  int strokePathCount = 0;

  // Kelvinlet brush uniforms — Lamé-style material constants. Live on Brush
  // (rather than only on CommandCtx) because they're authored alongside
  // strength/radius. Defaults match the kelvinlet paper's "soft rubber".
  float mu = 1.0f;
  float nu = 0.4f;

  // Pinch amount (0..1), synced from the TS brush (brush.pinch). Read by the
  // pinch / sharp kernels as the `@static` uniform `pinch` to scale the
  // toward-axis pull. A plain member loadProps leaves untouched.
  float pinch = 0.0f;
  // Smooth projection factor (0..1), synced from the TS brush (brush.smoothProj).
  // Read by bsmooth as `@static` uniform `projection`: the fraction of each
  // smoothing step's normal component removed, so the surface slides tangentially
  // instead of shrinking (volume preservation).
  float projection = 0.0f;
  // Rake amount (0..1), synced from the TS brush (brush.rake). Read by the
  // feature-align smooth kernel as the `@static` uniform `rake`: how strongly
  // edges aligned with the per-vertex cross field are up-weighted, biasing the
  // smooth so edge flow follows feature/curvature directions (topology rake).
  float rake = 0.0f;

  // Cavity automasking (documentation/plans/2026-07-14-2007-cavity-automasking.md):
  // a per-vertex, per-stroke local-convexity factor multiplied into the effective
  // strength — distinct from the painted `mask`. Computed host-side (see
  // automask.h); `cavity_factor` scales it, `cavity_blur_steps` sets the BFS blur
  // radius, `cavity_inverted` masks concavities instead of convexities.
  bool automask_cavity = false;
  float cavity_factor = 1.0f;
  int cavity_blur_steps = 2;
  bool cavity_inverted = false;

  // Enhance-details brush (source/brush/enhance.h): `enhance_rings` is the outer
  // smoothing depth (low-pass cutoff / feature scale); `enhance_inner` is the
  // inner depth — 0 = classic unsharp (high-pass), >=1 = difference-of-smooths
  // band-pass (default, rejects mesh noise).
  int enhance_rings = 4;
  int enhance_inner = 1;

  // Grab-style ctx state (kelvinlet, future pose). `grabFrom` is the stroke
  // origin captured at the start of the dab; `grabTo` is the current cursor.
  float3 grabFrom{0, 0, 0};
  float3 grabTo{0, 0, 0};

  // Pose-brush cage. `poseCageRest` is sampled when the dab starts; the DSL
  // displaces each vertex by a weighted sum of `poseCageNow[i] - poseCageRest[i]`
  // with weights = 1 / (1 + |v.co - poseCageRest[i]|²). Four anchors is the
  // tightest fit that still gives a smooth pose without needing a loop in
  // the DSL (Wave 4b has no `for`).
  float3 poseCageRest[4] = {};
  float3 poseCageNow[4] = {};

  // Authoring source of truth for the `Curve` falloff. Defaults to a
  // smoothstep so a brush switched to `Curve` without ever calling
  // `setFalloffCurvePreset` still produces the historical falloff shape.
  props::detail::curve::CurveGen falloffCurve{props::PropCurves::SMOOTHSTEP};

  // Baked output of `falloffCurve`, consulted when
  // `falloff_kind == FalloffKind::Curve`. Regenerated by `rebakeFalloff()`.
  std::array<float, kFalloffCurveSize> falloff_curve = [] {
    props::detail::curve::CurveGenSimple<props::PropCurves::SMOOTHSTEP> ss;
    return props::detail::curve::bake_curve_lut<kFalloffCurveSize>(ss);
  }();

  // expose falloffCurveSize to TS bridge
  float falloffCurveSize = kFalloffCurveSize;
  void setFalloffCurveEntry(int i, float f)
  {
    falloff_curve[i] = f;
  }

  // Cavity-automask curve LUT (size must equal automask.h kCavityCurveSize = 256;
  // a static_assert in the executor pins the two together). Reshapes the linear
  // cavity factor when `cavity_use_curve` is set; the TS bridge bakes it entry by
  // entry via setCavityCurveEntry, exactly like the falloff curve. Defaults to a
  // straight ramp (identity), so enabling the curve without authoring one is a
  // no-op. Only consulted when cavity_use_curve is set, so the array default is
  // fine for the common (curve-off) case.
  static constexpr int kCavityCurveLutSize = 256;
  bool cavity_use_curve = false;
  std::array<float, kCavityCurveLutSize> cavity_curve = [] {
    std::array<float, kCavityCurveLutSize> a{};
    for (int i = 0; i < kCavityCurveLutSize; i++) {
      a[i] = float(i) / float(kCavityCurveLutSize - 1);
    }
    return a;
  }();
  float cavityCurveSize = kCavityCurveLutSize;
  void setCavityCurveEntry(int i, float f)
  {
    if (i >= 0 && i < kCavityCurveLutSize) {
      cavity_curve[i] = f;
    }
  }

  static litestl::binding::types::Struct<Brush> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<Brush> *st =
        new types::Struct<Brush>("sculptcore::brush::Brush", sizeof(Brush));

    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);

    BIND_STRUCT_MEMBER(st, falloffCurveSize);
    BIND_STRUCT_MEMBER(st, falloff_shape);
    BIND_STRUCT_MEMBER(st, falloff_kind);
    BIND_STRUCT_MEMBER(st, strength);
    BIND_STRUCT_MEMBER(st, radius);
    BIND_STRUCT_MEMBER(st, spacing);
    BIND_STRUCT_MEMBER(st, planeoff);
    BIND_STRUCT_MEMBER(st, autosmooth);
    BIND_STRUCT_MEMBER(st, invert);
    BIND_STRUCT_MEMBER(st, mu);
    BIND_STRUCT_MEMBER(st, nu);
    BIND_STRUCT_MEMBER(st, pinch);
    BIND_STRUCT_MEMBER(st, projection);
    BIND_STRUCT_MEMBER(st, rake);
    BIND_STRUCT_MEMBER(st, automask_cavity);
    BIND_STRUCT_MEMBER(st, cavity_factor);
    BIND_STRUCT_MEMBER(st, cavity_blur_steps);
    BIND_STRUCT_MEMBER(st, cavity_inverted);
    BIND_STRUCT_MEMBER(st, cavity_use_curve);
    BIND_STRUCT_MEMBER(st, cavityCurveSize);
    BIND_STRUCT_MEMBER(st, enhance_rings);
    BIND_STRUCT_MEMBER(st, enhance_inner);
    BIND_STRUCT_MEMBER(st, grabFrom);
    BIND_STRUCT_MEMBER(st, grabTo);
    BIND_STRUCT_MEMBER(st, falloff_dir);
    BIND_STRUCT_MEMBER(st, falloff_extent);
    BIND_STRUCT_MEMBER(st, planeSide);
    BIND_STRUCT_MEMBER(st, strokeDir);
    BIND_STRUCT_MEMBER(st, wingAngle);
    BIND_STRUCT_MEMBER(st, wingNormalA);
    BIND_STRUCT_MEMBER(st, wingNormalB);
    BIND_STRUCT_MEMBER(st, activeGroup);
    BIND_STRUCT_MEMBER(st, brushColor);
    BIND_STRUCT_MEMBER(st, mixMode);
    BIND_STRUCT_MEMBER(st, props);
    BIND_STRUCT_METHOD(st, setFalloffCurveEntry, MARGS("i", "f"));
    BIND_STRUCT_METHOD(st, setCavityCurveEntry, MARGS("i", "f"));
    BIND_STRUCT_METHOD(st, loadProps, MARGS());
    BIND_STRUCT_METHOD(st, writeProps, MARGS());
    BIND_STRUCT_METHOD(st, pushDeviceInput, MARGS("type", "value"));
    BIND_STRUCT_METHOD(st, clearDeviceInputs, MARGS());
    BIND_STRUCT_METHOD(st, clearPropDynamics, MARGS("propId"));
    BIND_STRUCT_METHOD(
        st, addPropDynamic, MARGS("propId", "deviceType", "mixMode", "mixFactor"));
    BIND_STRUCT_METHOD(
        st, setPropDynamicSample, MARGS("propId", "deviceType", "i", "n", "value"));
    // Name-keyed dynamics for any registered float uniform (custom kernel
    // uniforms the BrushProp ids can't reach). The bridge enumerates the
    // uniform manifest and routes its configure calls through these.
    BIND_STRUCT_METHOD(st, clearPropDynamicsByName, MARGS("name"));
    BIND_STRUCT_METHOD(
        st, addPropDynamicByName, MARGS("name", "deviceType", "mixMode", "mixFactor"));
    BIND_STRUCT_METHOD(
        st, setPropDynamicSampleByName, MARGS("name", "deviceType", "i", "n", "value"));
    BIND_STRUCT_METHOD(st, setPropsParent, MARGS("parentProps"));
    BIND_STRUCT_METHOD(st, clearPropsParent, MARGS());

    return st;
  }

  // --- Property inheritance (Stage 4, bounded) ---------------------------
  // Link this brush's props to a parent default (e.g. a category-default
  // Brush's `props`) so any property this brush does not define locally
  // resolves from the parent. Takes the parent's `StructProp` (not a `Brush*`)
  // so `Brush::defineBindings` never references its own type — a self-reference
  // would re-enter defineBindings infinitely at init.
  void setPropsParent(props::StructProp *parentProps)
  {
    if (parentProps) {
      props::resolveStruct(*parentProps, props);
    }
  }
  void clearPropsParent()
  {
    if (props.struct_def) {
      props.struct_def->parent = nullptr;
    }
  }

  // --- Device (pen) dynamics configuration (Stage 3) ---------------------
  // The bridge configures a property's dynamics once per stroke, then pushes
  // device samples each dab; loadProps() applies them via the prop's Dynamics.

  // Resolve a float property's device-dynamics stack by name, or null. This is
  // the name-keyed core; any registered float uniform (common or per-kernel) is
  // reachable, not just the 5 `BrushProp` ids.
  props::Dynamics *propDynamics(util::string name)
  {
    if (!props.struct_def) {
      return nullptr;
    }
    props::Property *p = props.struct_def->lookup(name);
    if (!p) {
      return nullptr;
    }
    props::detail::PropBaseType *base = static_cast<props::detail::PropBaseType *>(p);
    if (p->type == props::Prop::FLOAT32) {
      return &static_cast<props::Float32Prop *>(base)->dynamics;
    }
    if (p->type == props::Prop::FLOAT64) {
      return &static_cast<props::Float64Prop *>(base)->dynamics;
    }
    return nullptr;
  }
  // Back-compat int-keyed overload for the bridge's fixed common props.
  props::Dynamics *propDynamics(int propId)
  {
    return propDynamics(util::string(brushPropName(propId)));
  }

  // Per-dab device samples. The bridge currently pushes pressure/tilt/twist; the
  // computed DeviceTypes (speed/angle/curvature) are reserved but not yet pushed.
  void pushDeviceInput(int type, float value)
  {
    deviceInputCtx.push(type, value);
  }
  void clearDeviceInputs()
  {
    deviceInputCtx.clear();
  }

  // Drop all device layers from a property's dynamics (reconfigure per stroke).
  void clearPropDynamicsByName(util::string name)
  {
    props::Dynamics *dyn = propDynamics(name);
    if (dyn) {
      dyn->devices.clear();
    }
  }
  // Add a device layer (identity curve) to a property; fill its response curve
  // with setPropDynamicSampleByName.
  void
  addPropDynamicByName(util::string name, int deviceType, int mixMode, float mixFactor)
  {
    props::Dynamics *dyn = propDynamics(name);
    if (!dyn) {
      return;
    }
    props::DynamicDevice dev;
    dev.type = static_cast<props::DeviceType>(deviceType);
    dev.mixMode = static_cast<litestl::math::BasicMix>(mixMode);
    dev.mixFactor = mixFactor;
    dyn->devices.append(std::move(dev));
  }
  // Set sample `i` of an `n`-entry response curve for the (name, deviceType)
  // device layer — the baked form of the TS channel's Curve1D.
  void
  setPropDynamicSampleByName(util::string name, int deviceType, int i, int n, float value)
  {
    props::Dynamics *dyn = propDynamics(name);
    if (!dyn) {
      return;
    }
    for (auto &dev : dyn->devices) {
      if ((int)dev.type == deviceType) {
        if (n > 0 && int(dev.curveTable.size()) != n) {
          dev.curveTable.resize(n);
        }
        if (i >= 0 && i < int(dev.curveTable.size())) {
          dev.curveTable[i] = value;
        }
        return;
      }
    }
  }

  // Int-keyed wrappers for the bridge's fixed common props (BrushProp ids).
  void clearPropDynamics(int propId)
  {
    clearPropDynamicsByName(brushPropName(propId));
  }
  void addPropDynamic(int propId, int deviceType, int mixMode, float mixFactor)
  {
    addPropDynamicByName(brushPropName(propId), deviceType, mixMode, mixFactor);
  }
  void setPropDynamicSample(int propId, int deviceType, int i, int n, float value)
  {
    setPropDynamicSampleByName(brushPropName(propId), deviceType, i, n, value);
  }

  Brush() : props(&structDef_)
  {
    structDef_.Float32("strength", "strength");
    structDef_.Float32("radius", "radius");
    structDef_.Float32("spacing", "spacing");
    structDef_.Float32("planeoff", "planeoff");
    structDef_.Float32("autosmooth", "autosmooth");
    structDef_.Bool("invert", "invert");
    // Per-kernel scalar uniforms (mu/nu/...) are registered on demand by the
    // active brush's generated registerProps — see sbrush-dynamic-uniforms.
  }

  // Resolve the authored property values into the cached scalar members the
  // kernels read. The no-arg form applies this brush's device-dynamics stack
  // (`deviceInputCtx`); with no devices configured / no inputs pushed it is a
  // bit-identical no-op, so it is safe to always route through it. Loads only
  // the fixed common props — the active kernel's scalar uniforms are resolved
  // by its generated loadUniformProps (see sbrush-dynamic-uniforms plan).
  void loadProps()
  {
    loadCommonProps(&deviceInputCtx);
  }

  void loadCommonProps(props::DeviceInputCtx *ctx)
  {
    strength = props.lookupValue<float>("strength", 1.0, ctx);
    radius = props.lookupValue<float>("radius", 1.0, ctx);
    spacing = props.lookupValue<float>("spacing", 0.25, ctx);
    planeoff = props.lookupValue<float>("planeoff", 0.0, ctx);
    autosmooth = props.lookupValue<float>("autosmooth", 0.0, ctx);
    invert = props.lookupValue<bool>("invert", false);
  }

  void writeProps()
  {
    props.setValue<float>("strength", strength);
    props.setValue<float>("radius", radius);
    props.setValue<float>("spacing", spacing);
    props.setValue<float>("planeoff", planeoff);
    props.setValue<float>("autosmooth", autosmooth);
    props.setValue<bool>("invert", invert);
  }

  // Normalized 0..1+ distance from the brush center for a vertex offset
  // `delta = co - surfacePos`, per the active `falloff_shape`. Feeds the
  // curve via `t = 1 - min(dist, 1)`. WGSL mirrors this in
  // `brush_falloff_dist`; changing one without the other breaks the
  // CPU/GPU bit-equality contract.
  float falloffDist(float3 delta) const
  {
    float inv_r = 1.0f / radius;
    switch (falloff_shape) {
    case FalloffShape::Spherical:
      return delta.length() * inv_r;
    case FalloffShape::Cube: {
      float ax = std::fabs(delta[0]);
      float ay = std::fabs(delta[1]);
      float az = std::fabs(delta[2]);
      float m = ax > ay ? ax : ay;
      m = m > az ? m : az;
      return m * inv_r;
    }
    case FalloffShape::Linear:
      return std::fabs(delta.dot(falloff_dir)) * inv_r;
    case FalloffShape::Box: {
      // Oriented cuboid. Build an orthonormal frame whose primary axis is
      // `falloff_dir` (the stroke direction); project `delta` onto it, divide
      // each component by the matching per-axis extent, take the max-norm.
      // The reference-axis pick mirrors sampleBrushTex and the WGSL branch
      // bit-for-bit (same |n.z| < 0.999 test) to keep CPU/GPU equal.
      float3 n = falloff_dir.normalized();
      float3 ref =
          std::abs(n[2]) < 0.999f ? float3{0.0f, 0.0f, 1.0f} : float3{1.0f, 0.0f, 0.0f};
      float3 t1 = ref.cross(n).normalized();
      float3 t2 = n.cross(t1);
      float dn = std::fabs(delta.dot(n)) / falloff_extent[0];
      float d1 = std::fabs(delta.dot(t1)) / falloff_extent[1];
      float d2 = std::fabs(delta.dot(t2)) / falloff_extent[2];
      float m = dn > d1 ? dn : d1;
      m = m > d2 ? m : d2;
      return m * inv_r;
    }
    }
    return delta.length() * inv_r;
  }

  // Evaluate the active falloff curve at normalized centerwise `t`
  // (1 at center, 0 at radius). Source of truth for the C++ side; the
  // WGSL emitter mirrors the same branches in `brush_falloff`.
  // The Gaussian width (9 in the exponent) hits exp(-9) ~= 1.2e-4 at
  // the edge, so no hard cutoff is needed. The Curve branch does
  // clamped linear interpolation over `falloff_curve` — N-1 segments,
  // index N-1 read directly when t lands exactly at 1.
  float falloffEval(float t) const
  {
    switch (falloff_kind) {
    case FalloffKind::Smoothstep:
      return t * t * (3.0f - 2.0f * t);
    case FalloffKind::Linear:
      return t;
    case FalloffKind::Gaussian: {
      float u = 1.0f - t;
      return std::exp(-9.0f * u * u);
    }
    case FalloffKind::Curve: {
      float clamped = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
      float scaled = clamped * (float)(kFalloffCurveSize - 1);
      int i0 = (int)scaled;
      if (i0 >= kFalloffCurveSize - 1)
        return falloff_curve[kFalloffCurveSize - 1];
      float frac = scaled - (float)i0;
      return falloff_curve[i0] * (1.0f - frac) + falloff_curve[i0 + 1] * frac;
    }
    }
    return t;
  }

  // Bilinear sample of the brush texture at UV `uv` (clamped to edge).
  // Returns 1.0 when no texture is bound so callers can multiply
  // unconditionally. WGSL mirrors this with a clamped textureSampleLevel;
  // the float math here is the CPU source of truth.
  float sampleTexBilinear(litestl::math::float2 uv) const
  {
    if (tex_width <= 0 || tex_height <= 0 || tex_pixels.size() == 0) {
      return 1.0f;
    }

    // Texel-space coords with half-texel offset; clamp to edge.
    float fx = uv[0] * (float)tex_width - 0.5f;
    float fy = uv[1] * (float)tex_height - 0.5f;

    int x0 = (int)std::floor(fx);
    int y0 = (int)std::floor(fy);
    float tx = fx - (float)x0;
    float ty = fy - (float)y0;

    auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    int x0c = clampi(x0, 0, tex_width - 1);
    int y0c = clampi(y0, 0, tex_height - 1);
    int x1c = clampi(x0 + 1, 0, tex_width - 1);
    int y1c = clampi(y0 + 1, 0, tex_height - 1);

    float p00 = tex_pixels[y0c * tex_width + x0c];
    float p10 = tex_pixels[y0c * tex_width + x1c];
    float p01 = tex_pixels[y1c * tex_width + x0c];
    float p11 = tex_pixels[y1c * tex_width + x1c];

    float a = p00 * (1.0f - tx) + p10 * tx;
    float b = p01 * (1.0f - tx) + p11 * tx;
    return a * (1.0f - ty) + b * ty;
  }

  // Drop all recorded stroke samples — called at the start of each stroke so
  // STROKE_CURVED arc lengths are measured from the stroke's first dab.
  void resetStrokePath()
  {
    strokePathCount = 0;
  }

  // Append a dab center to the StrokePath, accumulating arc length from the
  // previous sample. Once full, the oldest sample is dropped (true ring) so
  // arc length keeps growing along a long stroke without unbounded storage.
  void pushStrokeSample(float3 pos, float3 normal)
  {
    float arclen = 0.0f;
    if (strokePathCount > 0) {
      const StrokeSample &prev = strokePath[strokePathCount - 1];
      arclen = prev.arclen + (pos - prev.pos).length();
    }
    if (strokePathCount < kStrokePathMax) {
      strokePath[strokePathCount++] = StrokeSample{pos, normal, arclen};
    } else {
      for (int i = 1; i < kStrokePathMax; i++) {
        strokePath[i - 1] = strokePath[i];
      }
      strokePath[kStrokePathMax - 1] = StrokeSample{pos, normal, arclen};
    }
  }

  // Project `co` onto the StrokePath polyline and return UV for STROKE_CURVED:
  // uv.x = arc length at the nearest point along the stroke, uv.y = the
  // (unsigned) lateral distance from the centerline. With no path recorded the
  // origin is returned. WGSL mirrors this in `brush_stroke_uv`.
  litestl::math::float2 sampleStrokeUV(float3 co) const
  {
    if (strokePathCount == 0) {
      return litestl::math::float2{0.0f, 0.0f};
    }
    if (strokePathCount == 1) {
      return litestl::math::float2{strokePath[0].arclen,
                                   (co - strokePath[0].pos).length()};
    }

    float bestDist = std::numeric_limits<float>::max();
    float bestArc = 0.0f;
    float bestLat = 0.0f;
    for (int i = 0; i + 1 < strokePathCount; i++) {
      float3 a = strokePath[i].pos;
      float3 ab = strokePath[i + 1].pos - a;
      float len2 = ab.dot(ab);
      float t = len2 > 0.0f ? (co - a).dot(ab) / len2 : 0.0f;
      t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
      float3 d = co - (a + ab * t);
      float dist = d.length();
      if (dist < bestDist) {
        bestDist = dist;
        bestArc =
            strokePath[i].arclen + (strokePath[i + 1].arclen - strokePath[i].arclen) * t;
        bestLat = dist;
      }
    }
    return litestl::math::float2{bestArc, bestLat};
  }

  // Overwrite `falloff_curve` with a named preset. `inverse` flips the
  // smoothstep shape (full strength at the edge, zero at the center) —
  // useful for verifying the LUT actually drives the kernel rather
  // than being shadowed by the analytic dispatch.
  enum class CurvePreset { Smoothstep, Linear, Inverse, Gaussian };

  // Re-sample `falloffCurve` (the authoring curve) into `falloff_curve`
  // (the baked LUT). Call after mutating `falloffCurve`.
  void rebakeFalloff()
  {
    props::detail::curve::bake_curve_lut(
        *falloffCurve.gen, falloff_curve.data(), kFalloffCurveSize);
  }

  // Construct the authoring `CurveGen` for a named preset, then rebake.
  // Inverse maps to a reverse-linear b-spline (1-t); Gaussian maps to a
  // centered-bump `CurveGenGuassian` parameterized to match the brush's
  // analytic edge gaussian exp(-9(1-t)^2) (offset=1, 1/(2σ²)=9).
  void setFalloffCurvePreset(CurvePreset p)
  {
    using namespace props::detail::curve;

    switch (p) {
    case CurvePreset::Smoothstep:
      falloffCurve = CurveGen(props::PropCurves::SMOOTHSTEP);
      break;
    case CurvePreset::Linear:
      falloffCurve = CurveGen(props::PropCurves::LINEAR);
      break;
    case CurvePreset::Inverse: {
      falloffCurve = CurveGen(props::PropCurves::BSPLINE);
      static_cast<CurveGenBSpline *>(falloffCurve.gen)
          ->loadTemplate(SplineTemplate::REVERSE_LINEAR);
      break;
    }
    case CurvePreset::Gaussian: {
      falloffCurve = CurveGen(props::PropCurves::GUASSIAN);
      CurveGenGuassian *g = static_cast<CurveGenGuassian *>(falloffCurve.gen);
      g->height = 1.0;
      g->offset = 1.0;
      g->deviation = 1.0 / std::sqrt(18.0);
      break;
    }
    }

    rebakeFalloff();
  }

private:
  props::StructDef structDef_;
};
} // namespace sculptcore::brush
