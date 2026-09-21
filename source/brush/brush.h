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
#include <span>

#include "brush_configuration.h"
#include "named_uniform_store.h"
#include "props.h"
#include "texture_program.h"

#include "brush_types.h"

namespace sculptcore::brush {


struct Brush;
struct BrushMemberDescriptor {
  const char *name;
  props::Prop type;
  bool dynamic;
  props::Prop arrayElement = props::Prop::INVALID_TYPE;
  int arraySize = 0;
  void *(*address)(Brush &) = nullptr;
};

template <typename T> constexpr props::Prop brushMemberType()
{
  if constexpr (std::is_same_v<T, float>) {
    return props::Prop::FLOAT32;
  } else if constexpr (std::is_same_v<T, int>) {
    return props::Prop::INT32;
  } else if constexpr (std::is_same_v<T, bool>) {
    return props::Prop::BOOL;
  } else if constexpr (std::is_same_v<T, litestl::math::float2>) {
    return props::Prop::VEC2F;
  } else if constexpr (std::is_same_v<T, float3>) {
    return props::Prop::VEC3F;
  } else if constexpr (std::is_same_v<T, float4>) {
    return props::Prop::VEC4F;
  } else if constexpr (std::is_array_v<T>) {
    return props::Prop::ARRAYBUFFER;
  } else {
    return props::Prop::INVALID_TYPE;
  }
}

struct Brush {
  props::StructProp props;
  props::DeviceInputCtx deviceInputCtx;

  float strength = 1;
  float radius = 1;
  /* Fraction of the brush *diameter* between successive dabs along a stroke,
   * i.e. BrushStrokeDriver walks `spacing * 2 * radius` per dab. */
  float spacing = 0.25f;
  /* Plane-brush offset along surfaceNo as a fraction of radius â€” places the
   * projection plane above/below the stroke surface point for the clay-family
   * (plane) kernels. Unused by the non-plane brushes. */
  float planeoff = 0.0f;
  /* Autosmooth amount. Synced from the TS brush for prop parity / inheritance,
   * but not read C++-side yet: the chained SMOOTH command is built bridge-side
   * (buildBrushProgram reads the TS value and emits a SMOOTH BrushProgram entry
   * â€” see brush_executor.h). 0 disables the chained smooth. */
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
  // Per-axis half-extents (Ã—radius) for `FalloffShape::Box`, in the oriented
  // frame built from `falloff_dir` (extent[0] is along the stroke direction).
  // {1,1,1} makes Box an oriented cube. Unused by the other shapes.
  float3 falloff_extent{1, 1, 1};
  // Corner radius of `FalloffShape::RoundedBox` as a fraction of the tangent
  // extents: 1 rounds the rectangle into an ellipse (a plain radial falloff),
  // 0 leaves a hard-edged rectangle. Blender's `tip_roundness`. Unused by the
  // other shapes.
  float falloff_roundness = 1.0f;

  // Stroke tangent (current dab origin âˆ’ previous dab center), set host-side by
  // the executor each dab. Drives wing-scrape wing orientation and can feed the
  // oriented Box falloff via `falloff_dir`. Default +Z keeps it well-defined.
  float3 strokeDir{0, 0, 1};

  // When true, the host set `strokeDir` for this dab, so the executor must not
  // re-derive it from the shared stroke-path ring buffer â€” mirror-image dabs
  // supply their own reflected tangent. Reset per dab by the caller.
  bool strokeDirHostSet = false;

  // Wing-scrape: half-angle (radians) of each wing plane off the surface, plus
  // the two wing-plane normals (surfaceNo rotated Â±wingAngle about strokeDir),
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
  // to UV; Global and ViewRepeat fract-wrap it (Global's tile spans world
  // [-1,1]^2); `tex_repeat` is tiles per viewport height under ViewRepeat.
  int tex_width = 0;
  int tex_height = 0;
  litestl::util::Vector<float> tex_pixels;
  TexCoordSpace coord_space = TexCoordSpace::Global;
  float tex_repeat = 1.0f;

  /** Runtime-compiled texture script (texture-scripts T3.4). When bound it
   * takes precedence over the bitmap texture in sampleBrushTex; the script
   * does its own point mapping, so `coord_space` does not apply to it. Owned
   * (freed by clearTextureScript / ~Brush); Brush is never copied. */
  TextureProgram *texture_program = nullptr;
  // Live param slab (`texture_program->paramSlabSize` floats), seeded from
  // the program's defaults at bind and edited via setTextureParamAt /
  // setTextureRampAt. Read by sampleBrushTex every eval.
  litestl::util::Vector<float> texture_params;
  // Last setTextureScript compile error; empty after a successful bind.
  litestl::util::string texture_script_error;

  // Ring buffer of recent stroke-dab centers, driving STROKE_CURVED texture
  // mapping. Pushed host-side as dabs advance (CommandExecutor::execBrush),
  // reset at the start of each stroke (CommandExecutor::beginStep). Uniform-
  // resident on CPU; the WGSL emitter mirrors it as a storage buffer.
  StrokeSample strokePath[kStrokePathMax];
  int strokePathCount = 0;

  // Kelvinlet brush uniforms â€” LamÃ©-style material constants. Live on Brush
  // (rather than only on CommandCtx) because they're authored alongside
  // strength/radius. Defaults match the kelvinlet paper's "soft rubber".
  float mu = 1.0f;
  float nu = 0.4f;

  // Cutoff radius of an `@unbounded` brush field, as a multiple of `radius`.
  // The kernel's `unbounded_window()` smoothsteps the field to exactly zero
  // over [0.8R, R] with R = radius * unboundedExtent, and the host sizes the
  // spatial-node filter radius from the same R â€” that pairing is what keeps a
  // field with unbounded support from tearing on a leaf boundary. Read by TS
  // (sculptcore_ops) so the constant lives in exactly one place.
  float unboundedExtent = 8.0f;

  // Pinch amount (0..1), synced from the TS brush (brush.pinch). Read by the
  // pinch / sharp kernels as the `@static` uniform `pinch` to scale the
  // toward-axis pull. A plain member loadProps leaves untouched.
  float pinch = 0.0f;
  // Plane brush reach above (planeHeight) and below (planeDepth) the offset
  // plane, in radii; 0 disables that side. Read by the planebrush kernel as the
  // `@static` uniforms of the same names; the host hands them per dab (Blender
  // swaps them on an inverted stroke).
  float planeHeight = 1.0f;
  float planeDepth = 1.0f;
  // Rotate brush: cumulative angle (radians) about the anchor normal, host-set
  // per dab and reflected per symmetry image. Read by the rotate kernel as the
  // `@static` uniform `rotateAngle`.
  float rotateAngle = 0.0f;
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
  // Re-anchor corner UVs after tangential vertex motion (uv_reproject.h): the
  // executor reprojects each moved vertex's UVs on its pre-move 1-ring so
  // textures don't swim under smooth-family strokes. Host-set; not a uniform.
  bool reproject_uvs = false;

  // Cavity automasking (documentation/plans/2026-07-14-2007-cavity-automasking.md):
  // a per-vertex, per-stroke local-convexity factor multiplied into the effective
  // strength â€” distinct from the painted `mask`. Computed host-side (see
  // automask.h); `cavity_factor` scales it, `cavity_blur_steps` sets the BFS blur
  // radius, `cavity_inverted` masks concavities instead of convexities.
  bool automask_cavity = false;
  float cavity_factor = 1.0f;
  int cavity_blur_steps = 2;
  bool cavity_inverted = false;

  // View-normal automasking (documentation/plans/2026-07-25-1138-view-normal-
  // automasking.md): fade verts whose normal turns edge-on to the camera, where
  // a dab otherwise tears the silhouette. `view_normal_limit` / `_falloff` are
  // radians (see automask.h ViewNormalParams); `cull_backfaces` also zeroes
  // away-facing geometry. Defaults off in the engine so existing headless
  // scenes â€” which never set `viewDir` â€” are unchanged; the app turns it on.
  bool automask_view_normal = false;
  bool cull_backfaces = false;
  float view_normal_limit = kViewNormalLimitDefault;
  float view_normal_falloff = kViewNormalFalloffDefault;

  // Unit eye->surface ray in object space, host-set per dab (mirrored under
  // symmetry). Only read by the view-normal automask.
  float3 viewDir{0, 0, -1};

  // Enhance-details brush (source/brush/enhance.h): `enhance_rings` is the outer
  // smoothing depth (low-pass cutoff / feature scale); `enhance_inner` is the
  // inner depth â€” 0 = classic unsharp (high-pass), >=1 = difference-of-smooths
  // band-pass (default, rejects mesh noise).
  int enhance_rings = 4;
  int enhance_inner = 1;

  // Grab-style ctx state (kelvinlet, future pose). `grabFrom` is the stroke
  // origin captured at the start of the dab; `grabTo` is the current cursor.
  float3 grabFrom{0, 0, 0};
  float3 grabTo{0, 0, 0};

  // Pose-brush cage. `poseCageRest` is sampled when the dab starts; the DSL
  // displaces each vertex by a weighted sum of `poseCageNow[i] - poseCageRest[i]`
  // with weights = 1 / (1 + |v.co - poseCageRest[i]|Â²). Four anchors is the
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

  bool replaceFalloffCurveChecked(util::Vector<float> &samples);
  bool replaceCavityCurveChecked(util::Vector<float> &samples);

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

  NamedUniformStore<float> namedFloats;
  NamedUniformStore<int32_t> namedInts;
  NamedUniformStore<bool> namedBools;

  props::PropError setNamedScalar(props::Prop type, int slot, double value);
  void setNamedFloat(int slot, float value);
  void setNamedInt(int slot, int32_t value);
  void setNamedBool(int slot, bool value);
  float getNamedFloat(int slot)
  {
    return namedFloats.get(slot);
  }
  int32_t getNamedInt(int slot)
  {
    return namedInts.get(slot);
  }
  bool getNamedBool(int slot)
  {
    return namedBools.get(slot);
  }

  /** Generated defaults and evaluated writes never change authored properties. */
  void ensureNamedFloatDefault(int slot, float value)
  {
    if (slot >= 0 && slot < kNamedUniformSlotLimit) {
      namedFloats.ensure(slot, value);
    }
  }
  void ensureNamedIntDefault(int slot, int32_t value)
  {
    if (slot >= 0 && slot < kNamedUniformSlotLimit) {
      namedInts.ensure(slot, value);
    }
  }
  void ensureNamedBoolDefault(int slot, bool value)
  {
    if (slot >= 0 && slot < kNamedUniformSlotLimit) {
      namedBools.ensure(slot, value);
    }
  }
  void setEvaluatedNamedFloat(int slot, float value)
  {
    if (slot >= 0 && slot < kNamedUniformSlotLimit) {
      namedFloats.set(slot, value);
    }
  }
  void setEvaluatedNamedInt(int slot, int32_t value)
  {
    if (slot >= 0 && slot < kNamedUniformSlotLimit) {
      namedInts.set(slot, value);
    }
  }
  void setEvaluatedNamedBool(int slot, bool value)
  {
    if (slot >= 0 && slot < kNamedUniformSlotLimit) {
      namedBools.set(slot, value);
    }
  }

  /** Describe native member storage and semantic dynamics eligibility. */
  static std::span<const BrushMemberDescriptor> builtinPropDescriptorSpan()
  {
#define BRUSH_MEMBER(member, eligible)                                                   \
  [] {                                                                                   \
    static_assert(brushMemberType<decltype(member)>() != props::Prop::INVALID_TYPE);     \
    return BrushMemberDescriptor{                                                        \
        #member,                                                                         \
        brushMemberType<decltype(member)>(),                                             \
        eligible,                                                                        \
        std::is_array_v<decltype(member)>                                                \
            ? brushMemberType<std::remove_extent_t<decltype(member)>>()                  \
            : props::Prop::INVALID_TYPE,                                                 \
        int(std::extent_v<decltype(member)>),                                            \
        [](Brush &brush) -> void * { return &brush.member; }};                           \
  }()
    static const BrushMemberDescriptor members[] = {
        BRUSH_MEMBER(strength, true),
        BRUSH_MEMBER(radius, true),
        BRUSH_MEMBER(spacing, true),
        BRUSH_MEMBER(planeoff, true),
        BRUSH_MEMBER(planeSide, false),
        BRUSH_MEMBER(autosmooth, true),
        BRUSH_MEMBER(invert, true),
        BRUSH_MEMBER(strokeDir, false),
        BRUSH_MEMBER(wingAngle, true),
        BRUSH_MEMBER(wingNormalA, false),
        BRUSH_MEMBER(wingNormalB, false),
        BRUSH_MEMBER(activeGroup, false),
        BRUSH_MEMBER(brushColor, false),
        BRUSH_MEMBER(mixMode, false),
        BRUSH_MEMBER(mu, true),
        BRUSH_MEMBER(nu, true),
        BRUSH_MEMBER(unboundedExtent, true),
        BRUSH_MEMBER(pinch, true),
        BRUSH_MEMBER(planeHeight, false),
        BRUSH_MEMBER(planeDepth, false),
        BRUSH_MEMBER(rotateAngle, false),
        BRUSH_MEMBER(projection, true),
        BRUSH_MEMBER(rake, true),
        BRUSH_MEMBER(grabFrom, false),
        BRUSH_MEMBER(grabTo, false),
        BRUSH_MEMBER(poseCageRest, false),
        BRUSH_MEMBER(poseCageNow, false),
        BRUSH_MEMBER(automask_cavity, false),
        BRUSH_MEMBER(cavity_factor, false),
        BRUSH_MEMBER(cavity_blur_steps, false),
        BRUSH_MEMBER(cavity_inverted, false),
        BRUSH_MEMBER(cavity_use_curve, false),
        BRUSH_MEMBER(automask_view_normal, false),
        BRUSH_MEMBER(cull_backfaces, false),
        BRUSH_MEMBER(view_normal_limit, false),
        BRUSH_MEMBER(view_normal_falloff, false),
        BRUSH_MEMBER(enhance_rings, false),
        BRUSH_MEMBER(enhance_inner, false),
    };
#undef BRUSH_MEMBER
    return {members, sizeof(members) / sizeof(members[0])};
  }

  static void builtinPropDescriptors(litestl::util::Vector<BrushMemberDescriptor> &out)
  {
    for (const auto &member : builtinPropDescriptorSpan()) {
      out.append(member);
    }
  }

  static void builtinPropNames(litestl::util::Vector<litestl::util::string> &out)
  {
    litestl::util::Vector<BrushMemberDescriptor> descriptors;
    builtinPropDescriptors(descriptors);
    for (const auto &descriptor : descriptors) {
      out.append(litestl::util::string(descriptor.name));
    }
  }

  static litestl::binding::types::Struct<Brush> *defineBindings();
  // --- Property inheritance (Stage 4, bounded) ---------------------------
  // Link this brush's props to a parent default (e.g. a category-default
  // Brush's `props`) so any property this brush does not define locally
  // resolves from the parent. Takes the parent's `StructProp` (not a `Brush*`)
  // so `Brush::defineBindings` never references its own type â€” a self-reference
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

  // Raw native inspection, including unsupported legacy FLOAT64 stacks.
  // Configuration uses checked local ownership and eligibility separately.
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
    if (p->type == props::Prop::INT32) {
      return &static_cast<props::Int32Prop *>(base)->dynamics;
    }
    if (p->type == props::Prop::BOOL) {
      return &static_cast<props::BoolProp *>(base)->dynamics;
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

  // Legacy signatures delegate to the checked configuration boundary.
  void clearPropDynamicsByName(util::string name);
  void
  addPropDynamicByName(util::string name, int deviceType, int mixMode, float mixFactor);
  void setPropDynamicSampleByName(
      util::string name, int deviceType, int i, int n, float value);

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

  BrushScalarResult readCommonScalarChecked(int propId, int scalarType, bool evaluate)
  {
    return readScalarChecked(brushPropName(propId), scalarType, evaluate);
  }

  int writeCommonScalarChecked(int propId, int scalarType, double value)
  {
    return writeScalarChecked(brushPropName(propId), scalarType, value);
  }

  int configureCommonDynamicChecked(
      int propId, int scalarType, int device, int mode, float factor)
  {
    return configureDynamicChecked(
        brushPropName(propId), scalarType, device, mode, factor);
  }

  int enableCommonDynamicChecked(int propId, int scalarType, int device, int enabled)
  {
    return enableDynamicChecked(brushPropName(propId), scalarType, device, enabled);
  }

  int moveCommonDynamicChecked(int propId, int scalarType, int device, int index)
  {
    return moveDynamicChecked(brushPropName(propId), scalarType, device, index);
  }

  int clearCommonDynamicsChecked(int propId, int scalarType)
  {
    return clearDynamicsChecked(brushPropName(propId), scalarType);
  }

  // Reflection requires non-const vector references. These wrappers only read
  // caller arrays; the native configuration implementation accepts const refs.
  int replaceCommonDynamicTableChecked(int propId,
                                       int scalarType,
                                       int device,
                                       util::Vector<float> &samples)
  {
    return replaceDynamicTableChecked(brushPropName(propId), scalarType, device, samples);
  }

  int setCommonDynamicSampleChecked(
      int propId, int scalarType, int device, int index, int count, float value)
  {
    return setDynamicSampleChecked(
        brushPropName(propId), scalarType, device, index, count, value);
  }

  int replaceCommonDynamicsChecked(int propId,
                                   int scalarType,
                                   util::Vector<int> &devices,
                                   util::Vector<int> &modes,
                                   util::Vector<float> &factors,
                                   util::Vector<int> &enabled,
                                   util::Vector<int> &offsets,
                                   util::Vector<float> &samples)
  {
    return replaceDynamicsChecked(brushPropName(propId),
                                  scalarType,
                                  devices,
                                  modes,
                                  factors,
                                  enabled,
                                  offsets,
                                  samples);
  }

  int replaceCommonResponseDynamicsChecked(int propId,
                                           int scalarType,
                                           util::Vector<int> &devices,
                                           util::Vector<int> &modes,
                                           util::Vector<float> &factors,
                                           util::Vector<int> &enabled,
                                           util::Vector<int> &offsets,
                                           util::Vector<float> &samples,
                                           util::Vector<int> &kinds,
                                           util::Vector<double> &parameters)
  {
    return replaceResponseDynamicsChecked(brushPropName(propId),
                                          scalarType,
                                          devices,
                                          modes,
                                          factors,
                                          enabled,
                                          offsets,
                                          samples,
                                          kinds,
                                          parameters);
  }

  BrushScalarResult readScalarChecked(util::string name, int scalarType, bool evaluate);
  int writeScalarChecked(util::string name, int scalarType, double value);
  int configureDynamicChecked(
      util::string name, int scalarType, int device, int mode, float factor);
  int enableDynamicChecked(util::string name, int scalarType, int device, int enabled);
  int moveDynamicChecked(util::string name, int scalarType, int device, int index);
  int clearDynamicsChecked(util::string name, int scalarType);
  int replaceDynamicTableChecked(util::string name,
                                 int scalarType,
                                 int device,
                                 const util::Vector<float> &samples);
  int setDynamicSampleChecked(
      util::string name, int scalarType, int device, int index, int count, float value);
  int replaceDynamicsChecked(util::string name,
                             int scalarType,
                             const util::Vector<int> &devices,
                             const util::Vector<int> &modes,
                             const util::Vector<float> &factors,
                             const util::Vector<int> &enabled,
                             const util::Vector<int> &offsets,
                             const util::Vector<float> &samples);
  int replaceResponseDynamicsChecked(util::string name,
                                     int scalarType,
                                     const util::Vector<int> &devices,
                                     const util::Vector<int> &modes,
                                     const util::Vector<float> &factors,
                                     const util::Vector<int> &enabled,
                                     const util::Vector<int> &offsets,
                                     const util::Vector<float> &samples,
                                     const util::Vector<int> &kinds,
                                     const util::Vector<double> &parameters);
  uint64_t configurationGeneration() const
  {
    return configurationGeneration_;
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
    // active brush's generated registerProps â€” see sbrush-dynamic-uniforms.
  }

  ~Brush()
  {
    clearTextureScript();
  }

  // Resolve the authored property values into the cached scalar members the
  // kernels read. The no-arg form applies this brush's device-dynamics stack
  // (`deviceInputCtx`); with no devices configured / no inputs pushed it is a
  // bit-identical no-op, so it is safe to always route through it. Loads only
  // the fixed common props â€” the active kernel's scalar uniforms are resolved
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
    invert = props.lookupValue<bool>("invert", false, ctx);
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

  /** Publish only values authored by a dab driver; other fields may already be evaluated.
   */
  void writeDabProps()
  {
    props.setValue<float>("strength", strength);
    props.setValue<float>("radius", radius);
    props.setValue<bool>("invert", invert);
  }

  // Normalized 0..1+ distance from the brush center for a vertex offset
  // `delta = co - surfacePos`, per the active `falloff_shape`. `surfaceNo` is
  // the brush-center surface normal, used only by Box to orient its frame.
  // Feeds the curve via `t = 1 - dist` inside support. WGSL mirrors this in
  // `brush_falloff_dist`; changing one without the other breaks the
  // CPU/GPU bit-equality contract.
  float falloffDist(float3 delta, float3 surfaceNo) const;

  /** Finite enclosing sphere for ordinary brush falloff support. */
  float falloffSupportRadius(float r) const;

  /** Linear falloff varies along one axis but remains within the brush sphere. */
  bool insideFalloff(float3 delta, float3 surfaceNo) const;

  // Evaluate the active falloff curve at normalized centerwise `t`
  // (1 at center, 0 at radius). Source of truth for the C++ side; the
  // WGSL emitter mirrors the same branches in `brush_falloff`.
  // The Gaussian width (9 in the exponent) hits exp(-9) ~= 1.2e-4 at
  // the edge; strength() clips outside support. The Curve branch does
  // clamped linear interpolation over `falloff_curve` â€” N-1 segments,
  // index N-1 read directly when t lands exactly at 1.
  float falloffEval(float t) const;

  // Bilinear sample of the brush texture at UV `uv` (clamped to edge).
  // Returns 1.0 when no texture is bound so callers can multiply
  // unconditionally. WGSL mirrors this with a clamped textureSampleLevel;
  // the float math here is the CPU source of truth.
  float sampleTexBilinear(litestl::math::float2 uv) const;

  // Bind a grayscale brush texture (row-major `width * height` floats,
  // copied). The marshal-safe bridge seam: bound Vector args cross the
  // boundary, flat pixel pointers don't. Bad dims or a size mismatch clears.
  void setTexture(int width, int height, litestl::util::Vector<float> &pixels);

  void clearTexture();

  /** Compile `source` as a texture script and bind the program (replacing any
   * prior one). On failure the brush is left with no program and the message
   * lands in `texture_script_error`. `@const` params are frozen into the
   * compiled code â€” to change one, edit the source and rebind (milliseconds
   * under tcc); the param setters below refuse them. */
  bool setTextureScriptSource(litestl::util::stringref source,
                              litestl::util::stringref filename);

  // Marshal-safe wrapper: the binding runtime can't pass a host string into a
  // util::string arg (see BrushProp), so script source crosses as a char
  // Vector. NUL-terminated locally â€” stringref has no (ptr, size) ctor.
  bool setTextureScript(litestl::util::Vector<char> &source);

  void clearTextureScript();

  int textureParamCount();

  TextureProgramParam *queriedTextureParamEntry(int i);

  // Set a scalar param by manifest index, clamped to its @range. False for
  // ramps, @const params, or an unbound/invalid index.
  bool setTextureParamAt(int i, float value);

  // Overwrite a ramp param's LUT â€” exactly kTexRampSize samples.
  bool setTextureRampAt(int i, litestl::util::Vector<float> &lut);

  /** Evaluate the bound texture program at one point, outside any stroke.
   *
   * Returns 0 with no program bound. The map context is null, so a program
   * whose `usesMap` is set sees `mapPoint()` as identity rather than a real
   * render matrix â€” check `textureUsesMap()` and drive such a program through
   * a stroke instead.
   */
  float evalTextureAt(float px, float py, float pz, float nx, float ny, float nz);

  bool textureUsesMap();

  // C++-side convenience (unbound â€” strings can't cross the boundary).
  int textureParamIndex(const char *name);

  // Drop all recorded stroke samples â€” called at the start of each stroke so
  // STROKE_CURVED arc lengths are measured from the stroke's first dab.
  void resetStrokePath();

  // Append a dab center to the StrokePath, accumulating arc length from the
  // previous sample. Once full, the oldest sample is dropped (true ring) so
  // arc length keeps growing along a long stroke without unbounded storage.
  void pushStrokeSample(float3 pos, float3 normal);

  // Project `co` onto the StrokePath polyline and return UV for STROKE_CURVED:
  // uv.x = arc length at the nearest point along the stroke, uv.y = the
  // (unsigned) lateral distance from the centerline. With no path recorded the
  // origin is returned. WGSL mirrors this in `brush_stroke_uv`.
  litestl::math::float2 sampleStrokeUV(float3 co) const;

  // Overwrite `falloff_curve` with a named preset. `inverse` flips the
  // smoothstep shape (full strength at the edge, zero at the center) â€”
  // useful for verifying the LUT actually drives the kernel rather
  // than being shadowed by the analytic dispatch.
  enum class CurvePreset { Smoothstep, Linear, Inverse, Gaussian };

  // Re-sample `falloffCurve` (the authoring curve) into `falloff_curve`
  // (the baked LUT). Call after mutating `falloffCurve`.
  void rebakeFalloff();

  // Construct the authoring `CurveGen` for a named preset, then rebake.
  // Inverse maps to a reverse-linear b-spline (1-t); Gaussian maps to a
  // centered-bump `CurveGenGuassian` parameterized to match the brush's
  // analytic edge gaussian exp(-9(1-t)^2) (offset=1, 1/(2ÏƒÂ²)=9).
  void setFalloffCurvePreset(CurvePreset p);

private:
  props::PropError
  accessStaticScalar(props::Property *property, double &value, bool write, bool &handled);
  uint64_t configurationGeneration_ = 0;
  props::PropError checkedScalarTarget(util::string name,
                                       int scalarType,
                                       bool writable,
                                       bool dynamic,
                                       props::Property *&property);
  props::PropError checkedDynamicsTarget(util::string name,
                                         int scalarType,
                                         bool repair,
                                         props::Dynamics *&dynamics);
  int commitDynamics(props::Dynamics &target, const props::Dynamics &candidate);
  props::StructDef structDef_;
};
} // namespace sculptcore::brush
