#include "brush/brush.h"

namespace sculptcore::brush {

namespace {
// The stroke-aligned tangent frame the Box and RoundedBox metrics measure in:
// `tang` is `falloff_dir` (the stroke tangent) projected into the surface
// tangent plane, `lat` the in-plane perpendicular. A stroke running along the
// normal has no tangent, so any in-plane axis serves.
void boxFrame(float3 falloff_dir, float3 n, float3 &tang, float3 &lat)
{
  tang = falloff_dir - n * falloff_dir.dot(n);
  float tl = tang.length();
  if (tl < 1e-6f) {
    float3 ref = std::abs(n[2]) < 0.999f ? float3{0.0f, 0.0f, 1.0f} : float3{1.0f, 0.0f, 0.0f};
    tang = ref.cross(n);
    tl = tang.length();
  }
  tang = tang / tl;
  lat = n.cross(tang);
}
} // namespace

float Brush::falloffDist(float3 delta, float3 surfaceNo) const
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
      float3 tang, lat;
      boxFrame(falloff_dir, n, tang, lat);
      float dn = std::fabs(delta.dot(tang)) / falloff_extent[0];
      float d1 = std::fabs(delta.dot(lat)) / falloff_extent[1];
      float d2 = std::fabs(delta.dot(n)) / falloff_extent[2];
      float m = dn > d1 ? dn : d1;
      m = m > d2 ? m : d2;
      return m * inv_r;
    }
    case FalloffShape::RoundedBox: {
      // Blender's cube tip (calc_brush_cube_distances) in the Box frame's
      // tangent plane: the axis distances past the `1 - roundness` core are
      // the "excess", and the distance is its length over the corner radius.
      // Nothing inside the core has any falloff, the corners are quarter
      // ellipses of radius `roundness`, and roundness 1 is the plain radial
      // metric. The normal axis is not part of the metric (insideFalloff
      // bounds it by `falloff_extent[2]`).
      float3 n = surfaceNo.normalized();
      float3 tang, lat;
      boxFrame(falloff_dir, n, tang, lat);
      float q0 = std::fabs(delta.dot(tang)) * inv_r / falloff_extent[0];
      float q1 = std::fabs(delta.dot(lat)) * inv_r / falloff_extent[1];
      if (q0 > 1.0f || q1 > 1.0f) {
        return 1.0f;
      }
      float core = 1.0f - falloff_roundness;
      float e0 = q0 > core ? q0 - core : 0.0f;
      float e1 = q1 > core ? q1 - core : 0.0f;
      if (e0 == 0.0f && e1 == 0.0f) {
        return 0.0f;
      }
      if (falloff_roundness <= 0.0f) {
        return 1.0f;
      }
      float d = std::sqrt(e0 * e0 + e1 * e1) / falloff_roundness;
      return d < 1.0f ? d : 1.0f;
    }
    }
    return delta.length() * inv_r;
  }

float Brush::falloffSupportRadius(float r) const
  {
    if (falloff_shape == FalloffShape::Cube)
      return float(double(r) * std::sqrt(3.0));
    if (falloff_shape == FalloffShape::Box || falloff_shape == FalloffShape::RoundedBox)
      return float(double(r) * std::hypot(double(falloff_extent[0]),
                                          double(falloff_extent[1]),
                                          double(falloff_extent[2])));
    return r;
  }

float Brush::falloffFootprintRadius(float r) const
  {
    if (falloff_shape == FalloffShape::Cube)
      return float(double(r) * std::sqrt(2.0));
    if (falloff_shape == FalloffShape::Box || falloff_shape == FalloffShape::RoundedBox)
      return float(double(r) * std::hypot(double(falloff_extent[0]), double(falloff_extent[1])));
    return r;
  }

bool Brush::insideFalloff(float3 delta, float3 surfaceNo) const
  {
    if (!(radius > 0)) {
      return false;
    }
    if (falloff_shape == FalloffShape::RoundedBox) {
      // The tangent metric saturates at exactly 1 outside the rectangle, so
      // `<= 1` alone would admit the whole plane; the rectangle's own edge and
      // the normal-axis cutoff bound it instead.
      float3 n = surfaceNo.normalized();
      float3 tang, lat;
      boxFrame(falloff_dir, n, tang, lat);
      return std::fabs(delta.dot(tang)) <= radius * falloff_extent[0] &&
             std::fabs(delta.dot(lat)) <= radius * falloff_extent[1] &&
             std::fabs(delta.dot(n)) <= radius * falloff_extent[2];
    }
    return falloffDist(delta, surfaceNo) <= 1.0f &&
           (falloff_shape != FalloffShape::Linear || delta.length() <= radius);
  }

float Brush::falloffEval(float t) const
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

void Brush::rebakeFalloff()
  {
    props::detail::curve::bake_curve_lut(
        *falloffCurve.gen, falloff_curve.data(), kFalloffCurveSize);
  }

void Brush::setFalloffCurvePreset(CurvePreset p)
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

} // namespace sculptcore::brush
