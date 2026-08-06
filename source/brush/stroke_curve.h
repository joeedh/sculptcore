#pragma once

/**
 * Stroke geometry math for the C++ brush stroke driver: centripetal
 * Catmull-Rom -> cubic Bezier, sub-cubic extraction, and an arc-length walk
 * that emits evenly-spaced parameters with carry across segments.
 *
 * Direct port of the TypeScript `scripts/util/stroke_math.ts`; EPS and the
 * chord count must stay identical to it or the two drivers diverge.
 * Everything is double-precision: the walk carry accumulates across a whole
 * stroke and float drift makes parity flaky.
 */

#include "math/vector.h"
#include "util/vector.h"

#include <cmath>

namespace sculptcore::brush::curve {

using litestl::util::Vector;

template <int N> using Point = litestl::math::Vec<double, N>;

/** A cubic Bezier as its four control points. */
template <int N> struct Cubic {
  Point<N> p[4];

  Point<N> &operator[](int i)
  {
    return p[i];
  }
  const Point<N> &operator[](int i) const
  {
    return p[i];
  }
};

constexpr double EPS = 1e-7;

template <int N> inline double dist(const Point<N> &a, const Point<N> &b)
{
  double sum = 0.0;
  for (int i = 0; i < N; i++) {
    double d = a[i] - b[i];
    sum += d * d;
  }
  return std::sqrt(sum);
}

template <int N>
inline Point<N> lerpV(const Point<N> &a, const Point<N> &b, double t)
{
  Point<N> out;
  for (int i = 0; i < N; i++) {
    out[i] = a[i] + (b[i] - a[i]) * t;
  }
  return out;
}

/**
 * Catmull-Rom segment between P1 and P2 -> cubic Bezier control points.
 *
 * `alpha` selects the knot parameterization: 0 = uniform, 0.5 = centripetal,
 * 1 = chordal. Endpoint tangents become one-sided when a neighbor coincides
 * with its endpoint, which is how callers clamp the ends.
 *
 * `span` (the EPS-floored P1..P2 knot interval) is the divisor everywhere P1P2
 * appears, not the raw `t12`: a pointer that reports the same position twice
 * makes the segment itself degenerate, and dividing 0/0 there returned a NaN
 * curve — which poisons the caller's walk carry and kills the rest of the
 * stroke. Where the segment is non-degenerate the two are the same value.
 */
template <int N>
inline Cubic<N> crToBezier(const Point<N> &P0,
                           const Point<N> &P1,
                           const Point<N> &P2,
                           const Point<N> &P3,
                           double alpha = 0.5)
{
  const double t01 = std::pow(dist(P0, P1), alpha);
  const double t12 = std::pow(dist(P1, P2), alpha);
  const double t23 = std::pow(dist(P2, P3), alpha);

  const double span = t12 > EPS ? t12 : EPS;

  Point<N> m1;
  if (t01 < EPS) {
    for (int i = 0; i < N; i++) {
      m1[i] = (P2[i] - P1[i]) / span;
    }
  } else {
    for (int i = 0; i < N; i++) {
      m1[i] = (P2[i] - P1[i]) / span - (P2[i] - P0[i]) / (t01 + span) +
              (P1[i] - P0[i]) / t01;
    }
  }

  Point<N> m2;
  if (t23 < EPS) {
    for (int i = 0; i < N; i++) {
      m2[i] = (P2[i] - P1[i]) / span;
    }
  } else {
    for (int i = 0; i < N; i++) {
      m2[i] = (P3[i] - P2[i]) / t23 - (P3[i] - P1[i]) / (span + t23) +
              (P2[i] - P1[i]) / span;
    }
  }

  Cubic<N> out;
  out[0] = P1;
  out[3] = P2;
  for (int i = 0; i < N; i++) {
    out[1][i] = P1[i] + m1[i] * (span / 3.0);
    out[2][i] = P2[i] + m2[i] * (-span / 3.0);
  }
  return out;
}

template <int N> inline Point<N> evalCubic(const Cubic<N> &B, double s)
{
  const double mt = 1.0 - s;
  const double a = mt * mt * mt;
  const double b = 3.0 * mt * mt * s;
  const double c = 3.0 * mt * s * s;
  const double d = s * s * s;

  Point<N> out;
  for (int i = 0; i < N; i++) {
    out[i] = a * B[0][i] + b * B[1][i] + c * B[2][i] + d * B[3][i];
  }
  return out;
}

template <int N> struct SplitResult {
  Cubic<N> left;
  Cubic<N> right;
};

/** Split a cubic at `t` into its left ([0,t]) and right ([t,1]) sub-cubics. */
template <int N> inline SplitResult<N> deCasteljau(const Cubic<N> &B, double t)
{
  Point<N> a = lerpV(B[0], B[1], t);
  Point<N> b = lerpV(B[1], B[2], t);
  Point<N> c = lerpV(B[2], B[3], t);
  Point<N> d = lerpV(a, b, t);
  Point<N> e = lerpV(b, c, t);
  Point<N> f = lerpV(d, e, t);

  SplitResult<N> out;
  out.left[0] = B[0];
  out.left[1] = a;
  out.left[2] = d;
  out.left[3] = f;
  out.right[0] = f;
  out.right[1] = e;
  out.right[2] = c;
  out.right[3] = B[3];
  return out;
}

/** Extract the sub-cubic over [t0, t1] within [0,1] as its own cubic Bezier. */
template <int N> inline Cubic<N> subCubic(const Cubic<N> &B, double t0, double t1)
{
  t0 = std::min(std::max(t0, 0.0), 1.0);
  t1 = std::min(std::max(t1, 0.0), 1.0);

  if (t1 <= EPS) {
    Cubic<N> out;
    for (int i = 0; i < 4; i++) {
      out[i] = B[0];
    }
    return out;
  }

  Cubic<N> head = deCasteljau(B, t1).left;
  return deCasteljau(head, t0 / t1).right;
}

struct WalkResult {
  /** parameter values in [0,1] at each evenly-spaced (by arc length) sample */
  Vector<double> ts;
  /** leftover arc length to carry into the next segment's walk */
  double carryOut = 0.0;
};

/**
 * Walk a cubic by arc length, emitting a parameter every `spacingDist` units.
 * `carryIn` is the leftover distance from the previous segment so the cadence
 * is continuous across abutting segments; arc length is approximated by `fine`
 * straight chords with linear parameter interpolation inside a chord.
 */
template <int N>
inline WalkResult
arcLengthWalk(const Cubic<N> &B, double spacingDist, double carryIn = 0.0, int fine = 32)
{
  WalkResult res;

  if (spacingDist <= EPS) {
    res.carryOut = carryIn;
    return res;
  }

  double acc = carryIn;
  Point<N> prev = evalCubic(B, 0.0);

  for (int k = 1; k <= fine; k++) {
    const double sA = double(k - 1) / double(fine);
    const double sB = double(k) / double(fine);
    Point<N> cur = evalCubic(B, sB);
    const double seg = dist(prev, cur);
    prev = cur;

    if (seg <= EPS) {
      continue;
    }

    double local = 0.0;
    while (acc + seg * (1.0 - local) >= spacingDist) {
      const double need = spacingDist - acc;
      local += need / seg;
      res.ts.append(sA + (sB - sA) * local);
      acc = 0.0;
    }
    acc += seg * (1.0 - local);
  }

  res.carryOut = acc;
  return res;
}

} // namespace sculptcore::brush::curve
