#include "test_util.h"

#include <array>
#include <cmath>
#include <cstdio>

#include "props/prop_curve.h"

test_init;

/* The shared test_assert macro can't fail the run (it resets retval to 0),
 * so track failures locally and return nonzero from main. */
static int failures = 0;
#define curve_assert(expr)                                                               \
  do {                                                                                   \
    if (!(expr)) {                                                                       \
      failures++;                                                                        \
      fprintf(stderr, "curve_assert failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__);   \
      fflush(stderr);                                                                    \
    }                                                                                    \
  } while (0)

static bool approx(double a, double b, double eps = 1e-9)
{
  double d = a - b;
  return (d < 0 ? -d : d) <= eps;
}

int main()
{
  using namespace sculptcore::props;
  using namespace sculptcore::props::detail::curve;

  /* Analytic kinds: known values. */
  {
    CurveGenSimple<PropCurves::LINEAR> lin;
    curve_assert(approx(lin.evaluate(0.0), 0.0));
    curve_assert(approx(lin.evaluate(0.5), 0.5));
    curve_assert(approx(lin.evaluate(1.0), 1.0));

    CurveGenSimple<PropCurves::STEP> step;
    curve_assert(approx(step.evaluate(0.25), 0.0));
    curve_assert(approx(step.evaluate(0.75), 1.0));

    CurveGenSimple<PropCurves::SMOOTHSTEP> ss;
    curve_assert(approx(ss.evaluate(0.0), 0.0));
    curve_assert(approx(ss.evaluate(0.5), 0.5));
    curve_assert(approx(ss.evaluate(1.0), 1.0));

    CurveGenSimple<PropCurves::SHARP> sharp;
    curve_assert(approx(sharp.evaluate(0.5), 0.125)); /* f^3 */

    CurveGenSimple<PropCurves::SQRT> sq;
    curve_assert(approx(sq.evaluate(0.25), 0.5));
  }

  /* Gaussian: peak at offset, symmetric about offset. */
  {
    CurveGenGuassian g;
    g.offset = 0.5;
    g.height = 2.0;
    g.deviation = 0.2;
    curve_assert(approx(g.evaluate(0.5), 2.0)); /* peak == height */
    /* symmetry */
    curve_assert(approx(g.evaluate(0.3), g.evaluate(0.7), 1e-12));
    curve_assert(g.evaluate(0.5) > g.evaluate(0.4));
    curve_assert(g.evaluate(0.4) > g.evaluate(0.2));
  }

  /* derivative / integrate sanity. */
  {
    CurveGenSimple<PropCurves::LINEAR> lin;
    curve_assert(approx(lin.derivative(0.5), 1.0, 1e-3)); /* d/ds(s) == 1 */
    curve_assert(approx(lin.integrate(1.0), 0.5, 1e-2));  /* ∫₀¹ s ds == 0.5 */

    CurveGenSimple<PropCurves::SMOOTHSTEP> ss;
    /* smoothstep derivative is 0 at endpoints, max at 0.5 (= 1.5). */
    curve_assert(approx(ss.derivative(0.5), 1.5, 1e-2));
    curve_assert(ss.derivative(0.5) > ss.derivative(0.05));
    /* ∫₀¹ smoothstep == 0.5 by symmetry. */
    curve_assert(approx(ss.integrate(1.0), 0.5, 1e-2));
    /* inverse: smoothstep(0.5)==0.5, so inverse(0.5)≈0.5. */
    curve_assert(approx(ss.inverse(0.5), 0.5, 1e-2));
  }

  /* bake_curve_lut of smoothstep must match brush.h's baked LUT
   * bit-for-bit. Both sample evaluate() at i/(N-1) and cast to float, so
   * the comparison locks the bake against the brush placeholder. */
  {
    constexpr int N = 256;
    CurveGenSimple<PropCurves::SMOOTHSTEP> ss;
    std::array<float, N> lut = bake_curve_lut<N>(ss);

    for (int i = 0; i < N; i++) {
      double t = double(i) / double(N - 1);
      float golden = float(t * t * (3.0 - 2.0 * t));
      curve_assert(lut[i] == golden); /* bit-for-bit */
    }
    curve_assert(lut[0] == 0.0f);
    curve_assert(lut[N - 1] == 1.0f);
  }

  /* B-spline: LINEAR template (2 points) must be ~identity; endpoints exact;
   * monotone increasing. */
  {
    CurveGenBSpline bs;
    bs.loadTemplate(SplineTemplate::LINEAR);
    curve_assert(approx(bs.evaluate(0.0), 0.0, 1e-4));
    curve_assert(approx(bs.evaluate(1.0), 1.0, 1e-4));
    for (int i = 0; i <= 10; i++) {
      double t = double(i) / 10.0;
      curve_assert(approx(bs.evaluate(t), t, 1e-4));
    }
  }

  /* B-spline SMOOTH template: clamped endpoints, monotone, stays in [0,1]. */
  {
    CurveGenBSpline bs;
    bs.loadTemplate(SplineTemplate::SMOOTH);
    curve_assert(approx(bs.evaluate(0.0), 0.0, 1e-2));
    curve_assert(approx(bs.evaluate(1.0), 1.0, 1e-2));
    double prev = -1.0;
    bool monotone = true;
    for (int i = 0; i <= 20; i++) {
      double t = double(i) / 20.0;
      double y = bs.evaluate(t);
      curve_assert(y >= -0.01 && y <= 1.01);
      if (y < prev - 1e-3) {
        monotone = false;
      }
      prev = y;
    }
    curve_assert(monotone);
  }

  /* REVERSE_LINEAR: 1 at t=0, 0 at t=1. */
  {
    CurveGenBSpline bs;
    bs.loadTemplate(SplineTemplate::REVERSE_LINEAR);
    curve_assert(approx(bs.evaluate(0.0), 1.0, 1e-4));
    curve_assert(approx(bs.evaluate(1.0), 0.0, 1e-4));
    curve_assert(bs.evaluate(0.25) > bs.evaluate(0.75));
  }

  if (failures) {
    fprintf(stderr, "%d curve assertions failed\n", failures);
  }
  return test_end() || failures;
}
