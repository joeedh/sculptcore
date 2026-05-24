#include "prop_curve.h"

namespace sculptcore::props::detail::curve {

void bake_curve_lut(CurveGenBase &curve, float *out, int n)
{
  if (n <= 0) {
    return;
  }
  if (n == 1) {
    out[0] = float(curve.evaluate(0.0));
    return;
  }

  for (int i = 0; i < n; i++) {
    double t = double(i) / double(n - 1);
    out[i] = float(curve.evaluate(t));
  }
}

} // namespace sculptcore::props::detail::curve
