// Stroke-aligned Box falloff (FalloffShape::Box). Brush::falloffDist builds an
// oriented cuboid whose axis 0 is the stroke tangent (`falloff_dir`) projected
// into the surface tangent plane, axis 1 the in-plane perpendicular, axis 2 the
// surface normal. This asserts the frame actually follows the stroke direction
// (and rotates with it) and that the along-normal axis is governed by
// falloff_extent[2] — the fix for the box not aligning to the stroke.
#include "test_util.h"

#include "brush/brush.h"
#include "litestl/util/alloc.h"

#include <cmath>

test_init;

using namespace sculptcore::brush;
using namespace litestl::math;

int main()
{
  // A default-constructed Brush registers permanent prop descriptors; mark them
  // so the leak tracker doesn't flag them. falloffDist itself allocates nothing.
  litestl::alloc::PermanentGuard guard;

  Brush b;
  b.radius = 1.0f;
  b.falloff_shape = FalloffShape::Box;
  // Long along the stroke (axis 0), narrow perpendicular (axis 1), tall along
  // the surface normal (axis 2) so the normal never limits the dab.
  b.falloff_extent = float3{2.0f, 0.5f, 1000.0f};

  const float3 nrm{0.0f, 0.0f, 1.0f}; // flat surface, normal +Z

  // Stroke along +X: a point one unit *along* the stroke is inside
  // (2.0 half-extent), one unit *perpendicular* is outside (0.5 half-extent).
  b.falloff_dir = float3{1.0f, 0.0f, 0.0f};
  float alongX = b.falloffDist(float3{1.0f, 0.0f, 0.0f}, nrm);
  float perpX = b.falloffDist(float3{0.0f, 1.0f, 0.0f}, nrm);
  test_assert(std::fabs(alongX - 0.5f) < 1e-5f); // 1 / extent[0] = 0.5
  test_assert(std::fabs(perpX - 2.0f) < 1e-5f);  // 1 / extent[1] = 2.0
  test_assert(alongX < 1.0f && perpX > 1.0f);

  // Rotate the stroke to +Y: the long axis must follow, so now the +Y offset is
  // inside and the +X offset is outside — the box rotated with the stroke.
  b.falloff_dir = float3{0.0f, 1.0f, 0.0f};
  float alongY = b.falloffDist(float3{0.0f, 1.0f, 0.0f}, nrm);
  float perpY = b.falloffDist(float3{1.0f, 0.0f, 0.0f}, nrm);
  test_assert(std::fabs(alongY - 0.5f) < 1e-5f);
  test_assert(std::fabs(perpY - 2.0f) < 1e-5f);
  test_assert(alongY < 1.0f && perpY > 1.0f);

  // Along the surface normal the dab is effectively unbounded (extent[2] huge),
  // so a point well off the tangent plane is still inside.
  float alongN = b.falloffDist(float3{0.0f, 0.0f, 5.0f}, nrm);
  test_assert(alongN < 1e-2f);

  // A tilted surface: the frame is built from the *surface* normal, not world Z.
  // A point offset along the in-plane stroke tangent is inside; a point along
  // the surface normal is tall (extent[2] huge), proving the frame tracks it.
  float3 tn = float3{1.0f, 0.0f, 1.0f}.normalized(); // 45° tilted normal
  b.falloff_dir = float3{1.0f, 0.0f, 0.0f};          // stroke roughly +X
  // Project +X onto the tangent plane of tn, unit-length, as the kernel does.
  float3 tang = (b.falloff_dir - tn * b.falloff_dir.dot(tn)).normalized();
  float alongT = b.falloffDist(tang * 1.0f, tn);
  float alongTiltN = b.falloffDist(tn * 5.0f, tn);
  test_assert(std::fabs(alongT - 0.5f) < 1e-5f); // in-plane tangent, extent[0]=2
  test_assert(alongTiltN < 1e-2f);               // normal axis, extent[2] huge

  b.falloff_shape = FalloffShape::Spherical;
  test_assert(b.insideFalloff(float3(1, 0, 0), nrm));
  test_assert(b.falloffFootprintRadius(1.5f) == 1.5f);
  test_assert(!b.insideFalloff(float3(std::nextafter(1.0f, 2.0f), 0, 0), nrm));
  b.falloff_shape = FalloffShape::Cube;
  test_assert(b.insideFalloff(float3(.9f, .9f, .9f), nrm));
  test_assert(b.falloffSupportRadius(1) >= float3(.9f, .9f, .9f).length());
  test_assert(std::fabs(b.falloffFootprintRadius(1) - std::sqrt(2.0f)) < 1e-5f);
  b.falloff_shape = FalloffShape::Linear;
  b.falloff_dir = float3(1, 0, 0);
  test_assert(b.insideFalloff(float3(0, .9f, 0), nrm));
  test_assert(!b.insideFalloff(float3(0, 1.01f, 0), nrm));
  b.radius = 0;
  test_assert(!b.insideFalloff(float3(0.0f), nrm));

  // RoundedBox: Blender's cube tip. Extents (1, 0.5, 1) with the stroke along
  // +X, roundness 0.4: the core |q| <= 0.6 has no falloff at all, the margin
  // ramps 0..1 linearly along an axis, and the corners are rounded, so the
  // rectangle's corner is outside the shape.
  b.radius = 1.0f;
  b.falloff_shape = FalloffShape::RoundedBox;
  b.falloff_dir = float3(1, 0, 0);
  b.falloff_extent = float3{1.0f, 0.5f, 1.0f};
  b.falloff_roundness = 0.4f;
  test_assert(b.falloffDist(float3(0.5f, 0.0f, 0.0f), nrm) == 0.0f); // inside the core
  test_assert(b.falloffDist(float3(0.0f, 0.25f, 0.0f), nrm) == 0.0f); // q1 = 0.5, in the core
  test_assert(std::fabs(b.falloffDist(float3(0.8f, 0.0f, 0.0f), nrm) - 0.5f) < 1e-5f); // mid-margin
  // The rectangle edge (1 - 0.6 vs 0.4 differ in the last float bit).
  test_assert(b.falloffDist(float3(1.0f, 0.0f, 0.0f), nrm) > 1.0f - 1e-5f);
  test_assert(b.falloffDist(float3(1.1f, 0.0f, 0.0f), nrm) == 1.0f); // saturates outside
  // Corner: excess (0.4, 0.4), length / roundness = sqrt(2) > 1, clamped.
  test_assert(b.falloffDist(float3(1.0f, 0.5f, 0.0f), nrm) == 1.0f);
  // Diagonal inside the corner radius: excess (0.2, 0.2) -> 0.707.
  test_assert(std::fabs(b.falloffDist(float3(0.8f, 0.4f, 0.0f), nrm) - std::sqrt(0.08f) / 0.4f) <
              1e-5f);
  // The normal axis is not part of the metric but bounds support.
  test_assert(b.falloffDist(float3(0.0f, 0.0f, 0.9f), nrm) == 0.0f);
  test_assert(b.insideFalloff(float3(0.0f, 0.0f, 0.9f), nrm));
  test_assert(!b.insideFalloff(float3(0.0f, 0.0f, 1.1f), nrm));
  test_assert(b.insideFalloff(float3(1.0f, 0.5f, 0.0f), nrm));
  test_assert(!b.insideFalloff(float3(1.0f, 0.51f, 0.0f), nrm));
  test_assert(b.falloffSupportRadius(1) >= float3(1.0f, 0.5f, 1.0f).length());
  // The footprint (what a topology pass must cover) is the tangent-plane
  // corner, not the enclosing sphere; a sphere falloff keeps the radius.
  test_assert(std::fabs(b.falloffFootprintRadius(2.0f) - 2.0f * std::sqrt(1.25f)) < 1e-5f);
  test_assert(b.falloffFootprintRadius(2.0f) < b.falloffSupportRadius(2.0f));
  // Roundness 1 is the plain radial (elliptical) metric: sqrt(q0^2 + q1^2).
  b.falloff_roundness = 1.0f;
  test_assert(std::fabs(b.falloffDist(float3(0.6f, 0.0f, 0.0f), nrm) - 0.6f) < 1e-5f);
  test_assert(std::fabs(b.falloffDist(float3(0.3f, 0.2f, 0.0f), nrm) - 0.5f) < 1e-5f);
  // Roundness 0 is a hard-edged rectangle: full strength up to the edge.
  b.falloff_roundness = 0.0f;
  test_assert(b.falloffDist(float3(0.99f, 0.49f, 0.0f), nrm) == 0.0f);
  test_assert(b.falloffDist(float3(1.01f, 0.0f, 0.0f), nrm) == 1.0f);
  // The frame follows the stroke: with the stroke along +Y the long axis is Y.
  b.falloff_roundness = 0.4f;
  b.falloff_dir = float3(0, 1, 0);
  test_assert(b.falloffDist(float3(0.0f, 0.8f, 0.0f), nrm) < 1.0f);
  test_assert(b.falloffDist(float3(0.8f, 0.0f, 0.0f), nrm) == 1.0f);

  return test_end();
}
