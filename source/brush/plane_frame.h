#pragma once

/** Plane-brush frame policy (Blender's `calc_brush_plane` semantics).
 *
 * A `@planeFrame` kernel (plane.sbrush) projects onto the plane through
 * `ctx.surfacePos` along `ctx.surfaceNo`. By default both are whatever the
 * host passed for the dab — the raycast hit. This policy lets the executor
 * replace them per dab, at apply time, the way Blender's plane brushes do:
 * an area-averaged normal and/or centre gathered around the cursor, a fixed
 * view/axis normal, the stroke's first frame held for the whole stroke, and
 * the PLANE type's rolling-average stabilisation. Resolving in the executor
 * rather than the host is what keeps the batch path correct — dab k of an
 * accumulating stroke must see dab k-1's edits — and gives the
 * non-accumulating case the stroke-start data the executor already keeps
 * (`.brush.disp.*`, `.brush.orig.no`).
 *
 * The policy is sticky executor state like `nonAccum`: the host pushes it
 * before every stroke, including the default for brushes that are not plane
 * brushes. Only the CPU executors apply it — the WGSL path marshals the host
 * frame directly, so a GPU stroke gets the raw hit frame.
 *
 * Everything here is executor-agnostic (mesh and grids share it); the data
 * walk lives in each executor.
 */

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <algorithm>
#include <cmath>

namespace sculptcore::brush {

using litestl::math::float3;

/** Where the plane normal comes from. `Surface` is the hit normal the host
 * passed (today's behaviour); the rest are Blender's `sculpt_plane` items. */
enum class PlaneNormalMode : int {
  Surface = 0,
  Area = 1,
  View = 2,
  X = 3,
  Y = 4,
  Z = 5,
};

/** Which point the plane passes through (and where the falloff is centred):
 * the cursor, or the weighted area centre of the gathered verts. */
enum class PlaneCenterMode : int {
  Cursor = 0,
  Area = 1,
};

struct PlaneFramePolicy {
  PlaneNormalMode normal = PlaneNormalMode::Surface;
  PlaneCenterMode center = PlaneCenterMode::Cursor;
  /** Blender's `use_original_normal` / `use_original_plane`: after the
   * stroke's first primary dab, reuse that dab's normal / centre. */
  bool originalNormal = false;
  bool originalPlane = false;
  /** Gather radii as fractions of the brush radius (`normal_radius_factor`,
   * and the PLANE-only `area_radius_factor`; the host applies Blender's
   * "`area_radius_factor` only when > 0, else `normal_radius_factor`" rule). */
  float normalRadiusFactor = 1.0f;
  float areaRadiusFactor = 1.0f;
  /** PLANE-type rolling-average stabilisation weights (0 = off). Applied to
   * the AREA normal only, as Blender does. */
  float stabilizeNormal = 0.0f;
  float stabilizePlane = 0.0f;
  /** Object-space camera axis, surface -> eye, fixed for the stroke. Both the
   * `View` normal and the front/back bucket split read it. */
  float3 viewAxis{0.0f, 0.0f, 1.0f};

  /** False when the executor should leave the host frame alone entirely. */
  bool active() const
  {
    return normal != PlaneNormalMode::Surface || center != PlaneCenterMode::Cursor ||
           originalNormal || originalPlane;
  }
  bool stabilizes() const
  {
    return normal == PlaneNormalMode::Area &&
           (stabilizeNormal > 0.0f || stabilizePlane > 0.0f);
  }
};

/** Blender's `area_normal_calc_weight`: smoothstep toward the cursor,
 * clamped, over `p = 1 - d / r`. */
inline float planeFrameWeight(float distance, float radiusInv)
{
  const float p = 1.0f - distance * radiusInv;
  return std::clamp(3.0f * p * p - 2.0f * p * p * p, 0.0f, 1.0f);
}

/** Two-bucket accumulator for the area normal and centre
 * (`AreaNormalCenterData`): bucket 0 faces the view, bucket 1 is flipped
 * (`dot(viewAxis, n) <= 0`). Sums are resolved with Blender's rules — the
 * centre from the first non-empty bucket, else the cursor; the normal from
 * the first bucket whose sum normalises non-zero. */
struct PlaneFrameAccum {
  float3 cos[2]{};
  int countCo[2]{};
  float3 nos[2]{};
  int countNo[2]{};
  float3 cursor{};
  float3 viewAxis{0.0f, 0.0f, 1.0f};
  float rN = 0.0f, rC = 0.0f, rNinv = 0.0f, rCinv = 0.0f, rMax = 0.0f;

  void begin(const float3 &cursor_, const float3 &viewAxis_, float rN_, float rC_)
  {
    cos[0] = cos[1] = float3(0.0f, 0.0f, 0.0f);
    nos[0] = nos[1] = float3(0.0f, 0.0f, 0.0f);
    countCo[0] = countCo[1] = countNo[0] = countNo[1] = 0;
    cursor = cursor_;
    viewAxis = viewAxis_;
    rN = std::fmax(rN_, 0.0f);
    rC = std::fmax(rC_, 0.0f);
    rNinv = rN > 0.0f ? 1.0f / rN : 0.0f;
    rCinv = rC > 0.0f ? 1.0f / rC : 0.0f;
    rMax = std::fmax(rN, rC);
  }

  /** One sample. `co`/`no` are the vintage the stroke reads (live, or
   * stroke-start when non-accumulating). Sphere metric from the cursor. */
  void add(const float3 &co, const float3 &no)
  {
    const float d = (co - cursor).length();
    if (d > rMax) {
      return;
    }
    const int flip = viewAxis.dot(no) <= 0.0f ? 1 : 0;
    if (d <= rC) {
      // `area_center_calc_weighted`: the sample pulled toward the cursor.
      cos[flip] += cursor + (co - cursor) * (1.0f - planeFrameWeight(d, rCinv));
      countCo[flip] += 1;
    }
    if (d <= rN) {
      nos[flip] += no * planeFrameWeight(d, rNinv);
      countNo[flip] += 1;
    }
  }

  /** Centre: mean of the first non-empty bucket, the cursor when both are. */
  float3 resolveCenter() const
  {
    for (int b = 0; b < 2; b++) {
      if (countCo[b] > 0) {
        return cos[b] * (1.0f / float(countCo[b]));
      }
    }
    return cursor;
  }

  /** Normal: the first bucket that normalises non-zero. False (and a zero
   * `out`) when neither does — the plane is degenerate and Blender's dab is a
   * no-op. */
  bool resolveNormal(float3 &out) const
  {
    for (int b = 0; b < 2; b++) {
      const float len = nos[b].length();
      if (len > 0.0f) {
        out = nos[b] * (1.0f / len);
        return true;
      }
    }
    out = float3(0.0f, 0.0f, 0.0f);
    return false;
  }
};

/** Port of Blender's `calc_stabilized_plane` (PLANE type): rolling averages
 * of up to `1 + w * 19` frames, the normal first lerped toward the previous
 * stabilised normal and the centre projected onto the previous stabilised
 * plane. With both weights at 0 the rings hold one entry and the output is
 * the input — an exact no-op. Per-stroke state; `reset()` at stroke start. */
struct PlaneStabilizer {
  static constexpr int kMaxRollingAverage = 20;

  litestl::util::Vector<float3> normals;
  litestl::util::Vector<float3> centers;
  int normalIndex = 0;
  int centerIndex = 0;
  bool first = true;
  float3 lastNormal{};
  float3 lastCenter{};

  void reset()
  {
    first = true;
  }

  void apply(float normalWeight,
             float centerWeight,
             const float3 &planeNormal,
             const float3 &planeCenter,
             float3 &outNormal,
             float3 &outCenter)
  {
    float3 newNormal, newCenter;
    if (first) {
      newNormal = planeNormal;
      newCenter = planeCenter;
      const int nN = int(1.0f + normalWeight * float(kMaxRollingAverage - 1));
      const int nC = int(1.0f + centerWeight * float(kMaxRollingAverage - 1));
      normals.resize(std::max(nN, 1));
      centers.resize(std::max(nC, 1));
      for (auto &n : normals) {
        n = planeNormal;
      }
      for (auto &c : centers) {
        c = planeCenter;
      }
      normalIndex = 0;
      centerIndex = 0;
      first = false;
    } else {
      // interpolate(a, b, t) = a * (1 - t) + b * t
      newNormal =
          (planeNormal * (1.0f - normalWeight) + lastNormal * normalWeight).normalized();
      // Projection of the new centre onto the last stabilised plane.
      const float side = lastNormal.dot(planeCenter) - lastNormal.dot(lastCenter);
      const float3 projected = planeCenter - lastNormal * side;
      newCenter = planeCenter * (1.0f - centerWeight) + projected * centerWeight;
    }

    normals[normalIndex] = newNormal;
    centers[centerIndex] = newCenter;
    normalIndex = (normalIndex + 1) % int(normals.size());
    centerIndex = (centerIndex + 1) % int(centers.size());

    float3 sumNormal(0.0f, 0.0f, 0.0f);
    for (const auto &n : normals) {
      sumNormal += n;
    }
    outNormal = sumNormal.normalized();

    // Reference plane through the new centre; keep the mean signed offset of
    // the stored centres relative to it.
    const float planeW = -outNormal.dot(newCenter);
    float totalSigned = 0.0f;
    for (const auto &c : centers) {
      totalSigned += outNormal.dot(c) - planeW;
    }
    const float avgSigned = totalSigned / float(centers.size());
    const float newSigned = outNormal.dot(newCenter) - planeW;
    outCenter = newCenter - outNormal * (newSigned - avgSigned);

    lastNormal = outNormal;
    lastCenter = outCenter;
  }
};

/** Per-stroke frame memory: the last primary-image frame (mirror images
 * reflect it; the original-normal/plane toggles hold it) and the
 * stabiliser rings. */
struct PlaneFrameState {
  bool hasPrimary = false;
  float3 primaryNormal{};
  float3 primaryCenter{};
  PlaneStabilizer stabilizer;

  void reset()
  {
    hasPrimary = false;
    stabilizer.reset();
  }
};

/** Resolve one primary dab's frame from the policy, the incoming host frame,
 * the (optional) area query result and the stroke state. Shared by the mesh
 * and grid executors; the caller has already run the area query when
 * `needsAreaQuery()` said so. Returns false when the dab must be skipped
 * (AREA normal requested and the gather found nothing usable). Updates
 * `state` with the frame it resolved. */
struct PlaneFrameResolve {
  const PlaneFramePolicy &policy;
  PlaneFrameState &state;

  /** Whether a primary dab needs the area gather at all. */
  bool needsAreaQuery() const
  {
    const bool first = !state.hasPrimary;
    const bool needNormal = !(policy.originalNormal && !first);
    const bool needCenter = !(policy.originalPlane && !first);
    return (needNormal && policy.normal == PlaneNormalMode::Area) ||
           (needCenter && policy.center == PlaneCenterMode::Area);
  }

  bool primary(const float3 &hitNormal,
               const float3 &cursor,
               const PlaneFrameAccum *accum,
               float3 &outNormal,
               float3 &outCenter)
  {
    const bool first = !state.hasPrimary;
    const bool needNormal = !(policy.originalNormal && !first);
    const bool needCenter = !(policy.originalPlane && !first);

    float3 normal = state.primaryNormal;
    if (needNormal) {
      switch (policy.normal) {
      case PlaneNormalMode::Surface:
        normal = hitNormal;
        break;
      case PlaneNormalMode::Area:
        if (!accum || !accum->resolveNormal(normal)) {
          return false;
        }
        break;
      case PlaneNormalMode::View:
        normal = policy.viewAxis;
        break;
      case PlaneNormalMode::X:
        normal = float3(1.0f, 0.0f, 0.0f);
        break;
      case PlaneNormalMode::Y:
        normal = float3(0.0f, 1.0f, 0.0f);
        break;
      case PlaneNormalMode::Z:
        normal = float3(0.0f, 0.0f, 1.0f);
        break;
      }
    }
    float3 center = state.primaryCenter;
    if (needCenter) {
      center = (policy.center == PlaneCenterMode::Area && accum) ? accum->resolveCenter()
                                                                 : cursor;
    }
    if (policy.stabilizes()) {
      float3 sn, sc;
      state.stabilizer.apply(
          policy.stabilizeNormal, policy.stabilizePlane, normal, center, sn, sc);
      normal = sn;
      center = sc;
    }
    state.primaryNormal = normal;
    state.primaryCenter = center;
    state.hasPrimary = true;
    outNormal = normal;
    outCenter = center;
    return true;
  }

  /** A mirror image takes the reflected primary frame — no gather (Blender:
   * `symmetry_flip` of `sculpt_normal` / `last_center`). False when no
   * primary frame exists yet, in which case the caller resolves it as a
   * primary. */
  bool mirror(const float3 &sign, float3 &outNormal, float3 &outCenter) const
  {
    if (!state.hasPrimary) {
      return false;
    }
    outNormal = state.primaryNormal * sign;
    outCenter = state.primaryCenter * sign;
    return true;
  }
};

} // namespace sculptcore::brush
