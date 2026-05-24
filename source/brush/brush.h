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
using litestl::util::StrLiteral;
using litestl::math::float3;

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
enum class FalloffShape : unsigned char {
  Spherical = 0,
  Cube = 1,
  Linear = 2,
};

// Mapping from a world-space sample point to brush-texture UV. Orthogonal
// to the texture data itself; the discriminant rides in BrushUniforms so
// the WGSL `brush_sample_tex` mirrors the same branches.
//   Global     — uv = co.xy (world plane; stroke-independent, the test mode).
//   ViewPlane  — uv = (renderMatrix * co).xy (texture pinned to the view).
//   ViewRepeat — ViewPlane scaled by `tex_repeat` (tiled across the view).
//   StrokeCurved — uv = (arc length along the stroke, lateral offset from it);
//                  reads the StrokePath ring buffer of recent dab centers.
enum class TexCoordSpace : unsigned char {
  Global = 0,
  ViewPlane = 1,
  ViewRepeat = 2,
  StrokeCurved = 3,
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

struct Brush {
  props::StructProp props;
  props::DeviceInputCtx deviceInputCtx;

  float strength = 1;
  float radius = 1;
  /* Fraction of `radius` between successive brush dabs along a stroke. */
  float spacing = 0.25f;
  bool invert = false;
  FalloffKind falloff_kind = FalloffKind::Smoothstep;
  FalloffShape falloff_shape = FalloffShape::Spherical;
  // Direction for `FalloffShape::Linear` (expected normalized). Unused by
  // the other shapes. Default +Z keeps the value well-defined.
  float3 falloff_dir{0, 0, 1};

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

  static litestl::binding::types::Struct<Brush> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<Brush> *st =
        new types::Struct<Brush>("sculptcore::brush::Brush", sizeof(Brush));

    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);

    BIND_STRUCT_MEMBER(st, strength);
    BIND_STRUCT_MEMBER(st, radius);
    BIND_STRUCT_MEMBER(st, spacing);
    BIND_STRUCT_MEMBER(st, invert);
    BIND_STRUCT_MEMBER(st, mu);
    BIND_STRUCT_MEMBER(st, nu);
    BIND_STRUCT_MEMBER(st, grabFrom);
    BIND_STRUCT_MEMBER(st, grabTo);
    BIND_STRUCT_MEMBER(st, props);
    BIND_STRUCT_METHOD(st, loadProps, MARGS());
    BIND_STRUCT_METHOD(st, writeProps, MARGS());

    return st;
  }

  Brush() : props(&structDef_)
  {
    structDef_.Float32("strength", "strength");
    structDef_.Float32("radius", "radius");
    structDef_.Float32("spacing", "spacing");
    structDef_.Bool("invert", "invert");
    structDef_.Float32("mu", "mu");
    structDef_.Float32("nu", "nu");
  }

  void loadProps()
  {
    strength = props.lookupValue<float>("strength", 1.0);
    radius = props.lookupValue<float>("radius", 1.0);
    spacing = props.lookupValue<float>("spacing", 0.25);
    invert = props.lookupValue<bool>("invert", false);
    mu = props.lookupValue<float>("mu", 1.0);
    nu = props.lookupValue<float>("nu", 0.4);
  }

  void writeProps()
  {
    props.setValue<float>("strength", strength);
    props.setValue<float>("radius", radius);
    props.setValue<float>("spacing", spacing);
    props.setValue<bool>("invert", invert);
    props.setValue<float>("mu", mu);
    props.setValue<float>("nu", nu);
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
      if (i0 >= kFalloffCurveSize - 1) return falloff_curve[kFalloffCurveSize - 1];
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

    auto clampi = [](int v, int lo, int hi) {
      return v < lo ? lo : (v > hi ? hi : v);
    };
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
  void resetStrokePath() { strokePathCount = 0; }

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
        bestArc = strokePath[i].arclen +
                  (strokePath[i + 1].arclen - strokePath[i].arclen) * t;
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
