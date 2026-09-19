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

namespace sculptcore::brush {

// Falloff curve shapes selectable per brush. The three analytic kinds
// inline a closed form; `Curve` reads `Brush::falloff_curve` as a
// 256-entry LUT with linear interpolation. `t` is the normalized 0..1
// centerwise input â€” 1 at the brush center, 0 at the radius. Smoothstep
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
//   Spherical â€” euclidean radius (historical default, bit-identical).
//   Cube      â€” max(|dx|,|dy|,|dz|), the legacy SQUARE-brush metric.
//   Linear    â€” distance projected onto `falloff_dir` (stroke-line falloff).
//   Box       â€” oriented cuboid: max-norm in an orthonormal frame built from
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
//   Global     â€” uv = co.xy (world plane; stroke-independent, the test mode).
//   ViewPlane  â€” perspective-project co through renderMatrix (world -> clip),
//                NDC remapped to [0,1] across the viewport (screen-pinned).
//   ViewRepeat â€” ViewPlane scaled by `tex_repeat` (tiled across the view).
//   StrokeCurved â€” uv = (arc length along the stroke, lateral offset from it);
//                  reads the StrokePath ring buffer of recent dab centers.
//   Projected  â€” project (co - surfacePos) onto the brush-center tangent plane;
//                uv = its coordinates in a deterministic orthonormal basis built
//                from surfaceNo, normalized so the tile spans the brush circle
//                (centered on the dab, 0..1 across the diameter). The one mode
//                that actually consumes the surface normal arg threaded through
//                sampleBrushTex.
enum class TexCoordSpace : unsigned char {
  Global = 0,
  ViewPlane = 1,
  ViewRepeat = 2,
  StrokeCurved = 3,
  Projected = 4,
};
} // namespace sculptcore::brush

namespace litestl::binding {
template <> struct Binder<sculptcore::brush::TexCoordSpace> {
  static const BindingBase *bind();
};
} // namespace litestl::binding

namespace sculptcore::brush {

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

// View-normal automask defaults (radians): fade to nothing at 90Â° off head-on,
// over a 25Â° ramp â€” so full strength holds until 65Â°. Live here rather than in
// automask.h so brush.h needn't pull mesh.h in; automask.h reads them back.
inline constexpr float kViewNormalLimitDefault = 1.5707964f;
inline constexpr float kViewNormalFalloffDefault = 0.43633232f;

// Stable small ids for the common scalar props the bridge configures across the TS
// boundary. Used instead of `util::string` prop-name args: the TS binding
// runtime can't marshal a JS string into a bound `util::string` parameter
// (no String copy-ctor / cstring path â€” it expects a pre-built String handle).
// The C++ side maps the id back to the prop name (string literals never cross
// the boundary). Mirror in sculptcore_bindings.ts (`BrushProp`).
enum class BrushProp : int {
  Strength = 0,
  Radius = 1,
  Autosmooth = 2,
  Planeoff = 3,
  Spacing = 4,
  Invert = 5,
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
  case 5:
    return "invert";
  default:
    return "";
  }
}

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
    BIND_STRUCT_MEMBER(st, unboundedExtent);
    BIND_STRUCT_MEMBER(st, pinch);
    BIND_STRUCT_MEMBER(st, projection);
    BIND_STRUCT_MEMBER(st, rake);
    BIND_STRUCT_MEMBER(st, reproject_uvs);
    BIND_STRUCT_MEMBER(st, automask_cavity);
    BIND_STRUCT_MEMBER(st, cavity_factor);
    BIND_STRUCT_MEMBER(st, cavity_blur_steps);
    BIND_STRUCT_MEMBER(st, cavity_inverted);
    BIND_STRUCT_MEMBER(st, cavity_use_curve);
    BIND_STRUCT_MEMBER(st, cavityCurveSize);
    BIND_STRUCT_MEMBER(st, automask_view_normal);
    BIND_STRUCT_MEMBER(st, cull_backfaces);
    BIND_STRUCT_MEMBER(st, view_normal_limit);
    BIND_STRUCT_MEMBER(st, view_normal_falloff);
    BIND_STRUCT_MEMBER(st, viewDir);
    BIND_STRUCT_MEMBER(st, enhance_rings);
    BIND_STRUCT_MEMBER(st, enhance_inner);
    BIND_STRUCT_MEMBER(st, grabFrom);
    BIND_STRUCT_MEMBER(st, grabTo);
    BIND_STRUCT_MEMBER(st, falloff_dir);
    BIND_STRUCT_MEMBER(st, falloff_extent);
    BIND_STRUCT_MEMBER(st, planeSide);
    BIND_STRUCT_MEMBER(st, strokeDir);
    BIND_STRUCT_MEMBER(st, strokeDirHostSet);
    BIND_STRUCT_MEMBER(st, wingAngle);
    BIND_STRUCT_MEMBER(st, wingNormalA);
    BIND_STRUCT_MEMBER(st, wingNormalB);
    BIND_STRUCT_MEMBER(st, activeGroup);
    BIND_STRUCT_MEMBER(st, brushColor);
    BIND_STRUCT_MEMBER(st, mixMode);
    BIND_STRUCT_MEMBER(st, tex_width);
    BIND_STRUCT_MEMBER(st, tex_height);
    BIND_STRUCT_MEMBER(st, coord_space);
    BIND_STRUCT_MEMBER(st, tex_repeat);
    BIND_STRUCT_MEMBER(st, props);
    BIND_STRUCT_METHOD(st, setNamedFloat, MARGS("slot", "value"));
    BIND_STRUCT_METHOD(st, getNamedFloat, MARGS("slot"));
    BIND_STRUCT_METHOD(st, setFalloffCurveEntry, MARGS("i", "f"));
    BIND_STRUCT_METHOD(st, setCavityCurveEntry, MARGS("i", "f"));
    BIND_STRUCT_METHOD(st, replaceFalloffCurveChecked, MARGS("samples"));
    BIND_STRUCT_METHOD(st, replaceCavityCurveChecked, MARGS("samples"));
    BIND_STRUCT_METHOD(st, setTexture, MARGS("width", "height", "pixels"));
    BIND_STRUCT_METHOD(st, clearTexture, MARGS());
    BIND_STRUCT_MEMBER(st, texture_script_error);
    BIND_STRUCT_METHOD(st, setTextureScript, MARGS("source"));
    BIND_STRUCT_METHOD(st, clearTextureScript, MARGS());
    BIND_STRUCT_METHOD(st, textureParamCount, MARGS());
    BIND_STRUCT_METHOD(st, queriedTextureParamEntry, MARGS("i"));
    BIND_STRUCT_METHOD(st, setTextureParamAt, MARGS("i", "value"));
    BIND_STRUCT_METHOD(st, setTextureRampAt, MARGS("i", "lut"));
    BIND_STRUCT_METHOD(st, evalTextureAt, MARGS("px", "py", "pz", "nx", "ny", "nz"));
    BIND_STRUCT_METHOD(st, textureUsesMap, MARGS());
    BIND_STRUCT_METHOD(st, loadProps, MARGS());
    BIND_STRUCT_METHOD(st, writeProps, MARGS());
    BIND_STRUCT_METHOD(st, writeDabProps, MARGS());
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
    BIND_STRUCT_METHOD(st,
                       replaceCommonResponseDynamicsChecked,
                       MARGS("propId",
                             "scalarType",
                             "devices",
                             "modes",
                             "factors",
                             "enabled",
                             "offsets",
                             "samples",
                             "kinds",
                             "parameters"));
    BIND_STRUCT_METHOD(st, configurationGeneration, MARGS());
    BIND_STRUCT_METHOD(
        st, readCommonScalarChecked, MARGS("propId", "scalarType", "evaluate"));
    BIND_STRUCT_METHOD(
        st, writeCommonScalarChecked, MARGS("propId", "scalarType", "value"));
    BIND_STRUCT_METHOD(st,
                       configureCommonDynamicChecked,
                       MARGS("propId", "scalarType", "device", "mode", "factor"));
    BIND_STRUCT_METHOD(st,
                       enableCommonDynamicChecked,
                       MARGS("propId", "scalarType", "device", "enabled"));
    BIND_STRUCT_METHOD(
        st, moveCommonDynamicChecked, MARGS("propId", "scalarType", "device", "index"));
    BIND_STRUCT_METHOD(st, clearCommonDynamicsChecked, MARGS("propId", "scalarType"));
    BIND_STRUCT_METHOD(st,
                       replaceCommonDynamicTableChecked,
                       MARGS("propId", "scalarType", "device", "samples"));
    BIND_STRUCT_METHOD(
        st,
        setCommonDynamicSampleChecked,
        MARGS("propId", "scalarType", "device", "index", "count", "value"));
    BIND_STRUCT_METHOD(st,
                       replaceCommonDynamicsChecked,
                       MARGS("propId",
                             "scalarType",
                             "devices",
                             "modes",
                             "factors",
                             "enabled",
                             "offsets",
                             "samples"));

    return st;
  }

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
  float falloffDist(float3 delta, float3 surfaceNo) const
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
      // Stroke-aligned oriented cuboid: axis 0 = stroke tangent (`falloff_dir`)
      // projected into the surface tangent plane, axis 1 = in-plane
      // perpendicular, axis 2 = surface normal; extents from `falloff_extent`.
      float3 n = surfaceNo.normalized();
      float3 tang = falloff_dir - n * falloff_dir.dot(n);
      float tl = tang.length();
      if (tl < 1e-6f) {
        // Stroke ~parallel to the normal: pick any in-plane axis.
        float3 ref =
            std::abs(n[2]) < 0.999f ? float3{0.0f, 0.0f, 1.0f} : float3{1.0f, 0.0f, 0.0f};
        tang = ref.cross(n);
        tl = tang.length();
      }
      tang = tang / tl;
      float3 lat = n.cross(tang);
      float dn = std::fabs(delta.dot(tang)) / falloff_extent[0];
      float d1 = std::fabs(delta.dot(lat)) / falloff_extent[1];
      float d2 = std::fabs(delta.dot(n)) / falloff_extent[2];
      float m = dn > d1 ? dn : d1;
      m = m > d2 ? m : d2;
      return m * inv_r;
    }
    }
    return delta.length() * inv_r;
  }

  /** Finite enclosing sphere for ordinary brush falloff support. */
  float falloffSupportRadius(float r) const
  {
    if (falloff_shape == FalloffShape::Cube)
      return float(double(r) * std::sqrt(3.0));
    if (falloff_shape == FalloffShape::Box)
      return float(double(r) * std::hypot(double(falloff_extent[0]),
                                          double(falloff_extent[1]),
                                          double(falloff_extent[2])));
    return r;
  }

  /** Linear falloff varies along one axis but remains within the brush sphere. */
  bool insideFalloff(float3 delta, float3 surfaceNo) const
  {
    return radius > 0 && falloffDist(delta, surfaceNo) <= 1.0f &&
           (falloff_shape != FalloffShape::Linear || delta.length() <= radius);
  }

  // Evaluate the active falloff curve at normalized centerwise `t`
  // (1 at center, 0 at radius). Source of truth for the C++ side; the
  // WGSL emitter mirrors the same branches in `brush_falloff`.
  // The Gaussian width (9 in the exponent) hits exp(-9) ~= 1.2e-4 at
  // the edge; strength() clips outside support. The Curve branch does
  // clamped linear interpolation over `falloff_curve` â€” N-1 segments,
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

  // Bind a grayscale brush texture (row-major `width * height` floats,
  // copied). The marshal-safe bridge seam: bound Vector args cross the
  // boundary, flat pixel pointers don't. Bad dims or a size mismatch clears.
  void setTexture(int width, int height, litestl::util::Vector<float> &pixels)
  {
    if (width <= 0 || height <= 0 || pixels.size() != size_t(width) * size_t(height)) {
      clearTexture();
      return;
    }
    tex_width = width;
    tex_height = height;
    tex_pixels.clear();
    for (float p : pixels) {
      tex_pixels.append(p);
    }
  }

  void clearTexture()
  {
    tex_width = 0;
    tex_height = 0;
    tex_pixels.clear();
  }

  /** Compile `source` as a texture script and bind the program (replacing any
   * prior one). On failure the brush is left with no program and the message
   * lands in `texture_script_error`. `@const` params are frozen into the
   * compiled code â€” to change one, edit the source and rebind (milliseconds
   * under tcc); the param setters below refuse them. */
  bool setTextureScriptSource(litestl::util::stringref source,
                              litestl::util::stringref filename)
  {
    clearTextureScript();
    TextureProgram *p = compileTextureScript(source, filename, texture_script_error);
    if (!p) {
      return false;
    }
    texture_program = p;
    for (float f : p->defaults) {
      texture_params.append(f);
    }
    return true;
  }

  // Marshal-safe wrapper: the binding runtime can't pass a host string into a
  // util::string arg (see BrushProp), so script source crosses as a char
  // Vector. NUL-terminated locally â€” stringref has no (ptr, size) ctor.
  bool setTextureScript(litestl::util::Vector<char> &source)
  {
    litestl::util::Vector<char> buf;
    for (char c : source) {
      buf.append(c);
    }
    buf.append('\0');
    return setTextureScriptSource(buf.data(), "<script>");
  }

  void clearTextureScript()
  {
    if (texture_program) {
      freeTextureProgram(texture_program);
      texture_program = nullptr;
    }
    texture_params.clear();
    texture_script_error = litestl::util::string("");
  }

  int textureParamCount()
  {
    return texture_program ? (int)texture_program->params.size() : 0;
  }

  TextureProgramParam *queriedTextureParamEntry(int i)
  {
    if (!texture_program || i < 0 || i >= (int)texture_program->params.size()) {
      return nullptr;
    }
    return &texture_program->params[i];
  }

  // Set a scalar param by manifest index, clamped to its @range. False for
  // ramps, @const params, or an unbound/invalid index.
  bool setTextureParamAt(int i, float value)
  {
    TextureProgramParam *p = queriedTextureParamEntry(i);
    if (!p || p->isRamp || p->isConst || p->offset < 0) {
      return false;
    }
    if (p->hasRange) {
      value =
          value < p->rangeMin ? p->rangeMin : (value > p->rangeMax ? p->rangeMax : value);
    }
    texture_params[p->offset] = value;
    return true;
  }

  // Overwrite a ramp param's LUT â€” exactly kTexRampSize samples.
  bool setTextureRampAt(int i, litestl::util::Vector<float> &lut)
  {
    TextureProgramParam *p = queriedTextureParamEntry(i);
    if (!p || !p->isRamp || p->offset < 0 || (int)lut.size() != kTexRampSize) {
      return false;
    }
    for (int k = 0; k < kTexRampSize; k++) {
      texture_params[p->offset + k] = lut[k];
    }
    return true;
  }

  /** Evaluate the bound texture program at one point, outside any stroke.
   *
   * Returns 0 with no program bound. The map context is null, so a program
   * whose `usesMap` is set sees `mapPoint()` as identity rather than a real
   * render matrix â€” check `textureUsesMap()` and drive such a program through
   * a stroke instead.
   */
  float evalTextureAt(float px, float py, float pz, float nx, float ny, float nz)
  {
    if (!texture_program || !texture_program->eval) {
      return 0.0f;
    }
    const float P[3] = {px, py, pz};
    const float N[3] = {nx, ny, nz};
    const float *params = texture_params.size() > 0 ? texture_params.data() : nullptr;
    return texture_program->eval(P, N, params, nullptr);
  }

  bool textureUsesMap()
  {
    return texture_program ? texture_program->usesMap : false;
  }

  // C++-side convenience (unbound â€” strings can't cross the boundary).
  int textureParamIndex(const char *name)
  {
    for (int i = 0; i < textureParamCount(); i++) {
      if (texture_program->params[i].name == litestl::util::string(name)) {
        return i;
      }
    }
    return -1;
  }

  // Drop all recorded stroke samples â€” called at the start of each stroke so
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
  // smoothstep shape (full strength at the edge, zero at the center) â€”
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
  // analytic edge gaussian exp(-9(1-t)^2) (offset=1, 1/(2ÏƒÂ²)=9).
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
