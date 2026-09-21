#pragma once
#include "litestl/binding/binding.h"
#include "litestl/math/vector.h"
#include "litestl/util/compiler_util.h"

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
//   RoundedBox - Blender's cube brush tip (`calc_brush_cube_distances`): a
//               rounded rectangle in the Box frame's tangent plane, extents
//               from `falloff_extent[0..1]`, corner radius `falloff_roundness`
//               (1 = an ellipse, 0 = a hard-edged rectangle); the falloff runs
//               only across the rounded margin, the interior is full strength.
//               The metric ignores the normal axis; `falloff_extent[2]` bounds
//               it as a hard cutoff instead.
enum class FalloffShape : unsigned char {
  Spherical = 0,
  Cube = 1,
  Linear = 2,
  Box = 3,
  RoundedBox = 4,
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
} // namespace sculptcore::brush
