#include "brush/brush.h"

namespace sculptcore::brush {

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

float Brush::falloffSupportRadius(float r) const
  {
    if (falloff_shape == FalloffShape::Cube)
      return float(double(r) * std::sqrt(3.0));
    if (falloff_shape == FalloffShape::Box)
      return float(double(r) * std::hypot(double(falloff_extent[0]),
                                          double(falloff_extent[1]),
                                          double(falloff_extent[2])));
    return r;
  }

bool Brush::insideFalloff(float3 delta, float3 surfaceNo) const
  {
    return radius > 0 && falloffDist(delta, surfaceNo) <= 1.0f &&
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
