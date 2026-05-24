#include "prop_curve.h"

#include <bit>
#include <cmath>

using litestl::math::float2;

namespace sculptcore::props::detail::curve {

void CurveGenBSpline::add(double x, double y)
{
  points.append(ControlPoint{{float(x), float(y)}, 1});
}

void CurveGenBSpline::reset(bool empty)
{
  points.clear();
  if (!empty) {
    points.append(ControlPoint{{0.0f, 0.0f}, 1});
    points.append(ControlPoint{{1.0f, 1.0f}, 1});
  }
  updateKnots();
}

/* Build the extended control-point set: every point except the last, then
 * `deg` copies of the last point (mirrors path.ux updateKnots). Points are
 * sorted by x first. */
void CurveGenBSpline::updateKnots()
{
  /* insertion sort by x — point counts are tiny */
  for (int i = 1; i < points.size(); i++) {
    ControlPoint key = points[i];
    int j = i - 1;
    while (j >= 0 && points[j].co[0] > key.co[0]) {
      points[j + 1] = points[j];
      j--;
    }
    points[j + 1] = key;
  }

  ps_.clear();
  degOffset_ = -deg;

  if (points.size() < 2) {
    return;
  }

  for (int i = 0; i < points.size() - 1; i++) {
    ps_.append(points[i].co);
  }

  float2 last = points[points.size() - 1].co;
  for (int i = 0; i < deg; i++) {
    ps_.append(last);
  }
}

static double safe_inv(double n)
{
  return n == 0.0 ? 0.0 : 1.0 / n;
}

/* Cox-de Boor basis (recursive, no knot cache); ps_ x-coords act as knots. */
static double bas(const litestl::util::Vector<float2> &ps, double s, int i, int n)
{
  int len = ps.size();
  auto clampi = [&](int v) { return v < 0 ? 0 : (v > len - 1 ? len - 1 : v); };

  int kn = clampi(i + 1);
  int knn = clampi(i + n);
  int knn1 = clampi(i + n + 1);
  int ki = clampi(i);

  if (n == 0) {
    return (s >= ps[ki][0] && s < ps[kn][0]) ? 1.0 : 0.0;
  }

  double a = (s - ps[ki][0]) * safe_inv(ps[knn][0] - ps[ki][0] + 0.0001);
  double b = (ps[knn1][0] - s) * safe_inv(ps[knn1][0] - ps[kn][0] + 0.0001);

  return a * bas(ps, s, i, n - 1) + b * bas(ps, s, i + 1, n - 1);
}

double CurveGenBSpline::basis(double t, int i) const
{
  return bas(ps_, t, i + degOffset_, deg);
}

float2 CurveGenBSpline::evaluate2(double t) const
{
  t *= 0.9999999;

  double sumx = 0.0, sumy = 0.0, totbasis = 0.0;

  for (int i = 0; i < ps_.size(); i++) {
    double b = basis(t, i);
    sumx += b * ps_[i][0];
    sumy += b * ps_[i][1];
    totbasis += b;
  }

  if (totbasis != 0.0) {
    sumx /= totbasis;
    sumy /= totbasis;
  }

  return float2{float(sumx), float(sumy)};
}

/* Root-find the parameter whose x == t, return its y. Coarse scan for the
 * nearest sample, then bisection. */
double CurveGenBSpline::evaluateRootfind(double t) const
{
  double start_t = t;

  double xmin = ps_[0][0];
  double xmax = ps_[ps_.size() - 1][0];

  const int steps = 32;
  double s = xmin;
  double ds = (xmax - xmin) / double(steps - 1);

  double miny = 0.0, mins = s, mindx = 0.0;
  bool have = false;

  for (int i = 0; i < steps; i++, s += ds) {
    float2 p = evaluate2(s);
    double dx = std::fabs(double(p[0]) - start_t);
    if (!have || dx < mindx) {
      have = true;
      mindx = dx;
      miny = p[1];
      mins = s;
    }
  }

  double start = mins - ds;
  double end = mins + ds;
  s = mins;

  for (int i = 0; i < 16; i++) {
    float2 p = evaluate2(s);
    if (double(p[0]) > start_t) {
      end = s;
    } else {
      start = s;
    }
    s = (start + end) * 0.5;
    miny = p[1];
  }

  return miny;
}

double CurveGenBSpline::evaluate(double t)
{
  if (points.size() == 0) {
    return 0.0;
  }

  float2 a = points[0].co;
  float2 b = points[points.size() - 1].co;

  if (t <= a[0]) {
    return a[1];
  }
  if (t >= b[0]) {
    return b[1];
  }

  if (points.size() == 2) {
    double denom = double(b[0]) - double(a[0]);
    double tt = denom != 0.0 ? (t - a[0]) / denom : 0.0;
    return a[1] + (double(b[1]) - double(a[1])) * tt;
  }

  if (ps_.size() == 0) {
    updateKnots();
  }

  return evaluateRootfind(t);
}

void CurveGenBSpline::loadTemplate(SplineTemplate templ)
{
  struct Pt {
    double x, y;
  };

  reset(true);
  deg = 3;

  switch (templ) {
  case SplineTemplate::CONSTANT:
    add(1, 1);
    add(1, 1);
    break;
  case SplineTemplate::LINEAR:
    add(0, 0);
    add(1, 1);
    break;
  case SplineTemplate::SHARP:
    add(0, 0);
    add(0.9999, 0.0001);
    add(1, 1);
    break;
  case SplineTemplate::SQRT:
    add(0, 0);
    add(0.05, 0.25);
    add(0.15, 0.45);
    add(0.33, 0.65);
    add(1, 1);
    break;
  case SplineTemplate::SMOOTH:
    deg = 3;
    add(0, 0);
    add(1.0 / 3.0, 0);
    add(2.0 / 3.0, 1.0);
    add(1, 1);
    break;
  case SplineTemplate::SMOOTHER:
    deg = 6;
    add(0, 0);
    add(1.0 / 2.25, 0);
    add(2.0 / 3.0, 1.0);
    add(1, 1);
    break;
  case SplineTemplate::SHARPER:
    add(0, 0);
    add(0.3, 0.03);
    add(0.7, 0.065);
    add(0.9, 0.16);
    add(1, 1);
    break;
  case SplineTemplate::SPHERE:
    add(0, 0);
    add(0.01953, 0.23438);
    add(0.08203, 0.43359);
    add(0.18359, 0.625);
    add(0.35938, 0.81641);
    add(0.625, 0.97656);
    add(1, 1);
    break;
  case SplineTemplate::REVERSE_LINEAR:
    add(0, 1);
    add(1, 0);
    break;
  case SplineTemplate::GUASSIAN:
    deg = 5;
    add(0, 0);
    add(0.17969, 0.007);
    add(0.48958, 0.01172);
    add(0.77995, 0.99609);
    add(1, 1);
    break;
  }

  updateKnots();
}

CurveGenBSpline::HashInt CurveGenBSpline::hash()
{
  HashInt h = HashInt(PropCurves::BSPLINE);
  h ^= HashInt(deg) + 0x9e3779b9 + (h << 6) + (h >> 2);
  h ^= HashInt(points.size()) + 0x9e3779b9 + (h << 6) + (h >> 2);

  for (const ControlPoint &p : points) {
    /* quantize like path.ux calcHashKey to keep the hash stable under fp
     * noise; operator== does the exact recheck. */
    HashInt x = HashInt(std::llround(double(p.co[0]) * 1024.0));
    HashInt y = HashInt(std::llround(double(p.co[1]) * 1024.0));
    h ^= x + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= y + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= HashInt(p.tangent) + 0x9e3779b9 + (h << 6) + (h >> 2);
  }

  return h;
}

bool CurveGenBSpline::operator==(const CurveGenBase &b)
{
  if (b.type != PropCurves::BSPLINE) {
    return false;
  }

  const CurveGenBSpline *b2 = static_cast<const CurveGenBSpline *>(&b);

  if (b2->deg != deg || b2->points.size() != points.size()) {
    return false;
  }

  for (int i = 0; i < points.size(); i++) {
    float2 d = points[i].co - b2->points[i].co;
    if (std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1]) > 0.00001) {
      return false;
    }
    if (points[i].tangent != b2->points[i].tangent) {
      return false;
    }
  }

  return true;
}

} // namespace sculptcore::props::detail::curve
