#pragma once

#include "prop_base.h"
#include "prop_enums.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/hash.h"
#include "litestl/util/vector.h"

#include <array>
#include <bit>
#include <cmath>

using namespace litestl;
namespace sculptcore::props {
enum class PropCurves {
  STEP = 0,
  LINEAR = 1,
  SMOOTHSTEP = 2,
  SHARP = 3,
  SHARPER = 4,
  SQRT = 5,
  BSPLINE = 6,
  GUASSIAN = 7,
  TABLE = 8,
};

namespace detail::curve {

struct CurveGenBase {
  PropCurves type;

  CurveGenBase() : type(PropCurves::LINEAR)
  {
  }

  CurveGenBase(PropCurves type_) : type(type_)
  {
  }

  virtual ~CurveGenBase() = default;

  virtual double evaluate(double f) = 0;

  /* Numeric helpers ported from path.ux curve1d_base.ts. Default bodies
   * are finite-difference / bisection over evaluate(); analytic kinds may
   * override for accuracy but are not required to. */
  virtual double derivative(double s)
  {
    const double df = 0.0001;
    if (s > 1.0 - df * 3) {
      return (evaluate(s) - evaluate(s - df)) / df;
    } else if (s < df * 3) {
      return (evaluate(s + df) - evaluate(s)) / df;
    }
    return (evaluate(s + df) - evaluate(s - df)) / (2.0 * df);
  }

  virtual double derivative2(double s)
  {
    const double df = 0.0001;
    if (s > 1.0 - df * 3) {
      return (derivative(s) - derivative(s - df)) / df;
    } else if (s < df * 3) {
      return (derivative(s + df) - derivative(s)) / df;
    }
    return (derivative(s + df) - derivative(s - df)) / (2.0 * df);
  }

  virtual double integrate(double s1, int quadSteps = 64)
  {
    double ret = 0.0;
    const double ds = s1 / double(quadSteps);
    double s = 0.0;
    for (int i = 0; i < quadSteps; i++, s += ds) {
      ret += evaluate(s) * ds;
    }
    return ret;
  }

  /* Invert evaluate(): find s such that evaluate(s) ~= y, bisection over
   * a coarse scan of [0,1]. */
  virtual double inverse(double y)
  {
    const int steps = 9;
    const double ds = 1.0 / double(steps);
    double s = 0.0;
    bool have_best = false;
    double best = 0.0;
    double ret = 0.0;

    for (int i = 0; i < steps; i++, s += ds) {
      double s1 = s, s2 = s + ds, mid = 0.0;

      for (int j = 0; j < 11; j++) {
        double y1 = evaluate(s1);
        double y2 = evaluate(s2);
        mid = (s1 + s2) * 0.5;

        if (std::fabs(y1 - y) < std::fabs(y2 - y)) {
          s2 = mid;
        } else {
          s1 = mid;
        }
      }

      double err = std::fabs(y - evaluate(mid));
      if (!have_best || err < best) {
        have_best = true;
        best = err;
        ret = mid;
      }
    }

    return ret;
  }

  virtual litestl::hash::HashInt hash()
  {
    return 0;
  }

  virtual bool operator==(const CurveGenBase &b)
  {
    return b.type == type;
  }
};

template <PropCurves type_> struct CurveGenSimple : public CurveGenBase {
  using HashInt = litestl::hash::HashInt;

  CurveGenSimple() : CurveGenBase(type_)
  {
  }

  HashInt hash() override
  {
    return HashInt(type_);
  }

  virtual double evaluate(double f) override
  {
    if constexpr (type_ == PropCurves::STEP) {
      return double(f > 0.5f);
    } else if constexpr (type_ == PropCurves::LINEAR) {
      return f;
    } else if constexpr (type_ == PropCurves::SMOOTHSTEP) {
      return f * f * (3.0 - 2.0 * f);
    } else if constexpr (type_ == PropCurves::SHARP) {
      return f * f * f;
    } else if constexpr (type_ == PropCurves::SHARPER) {
      return f * f * f * f * f;
    } else if constexpr (type_ == PropCurves::SQRT) {
      return std::sqrt(std::fabs(f));
    }

    return 0.0;
  }
};

struct CurveGenTable : CurveGenBase {
  util::Vector<double> table;
  using HashInt = litestl::hash::HashInt;

  CurveGenTable() : CurveGenBase(PropCurves::TABLE)
  {
  }

  HashInt hash() override
  {
    if (have_hash_) {
      return hash_;
    }

    hash_ = 0;

    for (int i = 0; i < table.size(); i++) {
      hash_ ^= std::bit_cast<HashInt, double>(table[i]);
      hash_ += i;
    }

    have_hash_ = true;
    return hash_;
  }

  bool operator==(const CurveGenBase &b) override
  {
    if (b.type != PropCurves::TABLE) {
      return false;
    }

    const CurveGenTable *b2 = static_cast<const CurveGenTable *>(&b);

    if (b2->table.size() != table.size()) {
      return false;
    }

    for (int i = 0; i < table.size(); i++) {
      if (table[i] != b2->table[i]) {
        return false;
      }
    }

    return true;
  }

  /* Linearly interpolates table. */
  virtual double evaluate(double f) override
  {
    double fi = f * double(table.size());

    fi = std::min(std::max(fi, 0.0), double(table.size() - 1));
    int i1 = int(fi), i2 = i1 + 1;

    if (i1 == table.size() - 1) {
      return table[i1];
    }

    double t = fi - std::floor(fi);
    return table[i1] + (table[i2] - table[i1]) * t;
  }

private:
  HashInt hash_;
  bool have_hash_ = false;
};

enum class SplineTemplate {
  CONSTANT = 0,
  LINEAR = 1,
  SHARP = 2,
  SQRT = 3,
  SMOOTH = 4,
  SMOOTHER = 5,
  SHARPER = 6,
  SPHERE = 7,
  REVERSE_LINEAR = 8,
  GUASSIAN = 9,
};

/* 1D curve as a 2D b-spline root-found on x (port of path.ux BSplineCurve).
 * The 2D-spline trick lets arbitrary control points define a function of x;
 * evaluate(t) root-finds the parameter whose x == t and returns y. The
 * path.ux hermite/basis caches are dropped — we bake to a LUT instead. */
struct CurveGenBSpline : CurveGenBase {
  using HashInt = litestl::hash::HashInt;

  struct ControlPoint {
    math::float2 co{0.0f, 0.0f};
    uint8_t tangent = 1; /* SMOOTH */
  };

  util::Vector<ControlPoint> points;
  int deg = 3;

  CurveGenBSpline() : CurveGenBase(PropCurves::BSPLINE)
  {
    points.append(ControlPoint{{0.0f, 0.0f}, 1});
    points.append(ControlPoint{{1.0f, 1.0f}, 1});
    updateKnots();
  }

  void add(double x, double y);
  void reset(bool empty = false);
  void loadTemplate(SplineTemplate templ);
  void updateKnots();

  double evaluate(double f) override;
  HashInt hash() override;
  bool operator==(const CurveGenBase &b) override;

private:
  /* Extended control-point set (control points + degree-fold of the last
   * point), rebuilt by updateKnots(). */
  util::Vector<math::float2> ps_;
  int degOffset_ = 0;

  double basis(double t, int i) const;
  math::float2 evaluate2(double t) const;
  double evaluateRootfind(double t) const;
};

/* Centered-bump gaussian (path.ux GuassianCurve form):
 *   height * exp(-(s-offset)^2 / (2*deviation^2)).
 * Defaults peak at s=1 (offset=1), so it reads as an edge-weighted bump
 * over the 0..1 domain. Note this is *not* the brush's analytic gaussian
 * (exp(-9*(1-t)^2)); the brush keeps that as its own fast path (Q3/Q4). */
struct CurveGenGuassian : CurveGenBase {
  using HashInt = litestl::hash::HashInt;

  double height = 1.0;
  double offset = 1.0;
  double deviation = 0.3;

  CurveGenGuassian() : CurveGenBase(PropCurves::GUASSIAN)
  {
  }

  double evaluate(double s) override
  {
    double d = s - offset;
    return height * std::exp(-(d * d) / (2.0 * deviation * deviation));
  }

  HashInt hash() override
  {
    HashInt h = HashInt(PropCurves::GUASSIAN);
    h ^= std::bit_cast<HashInt, double>(height) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::bit_cast<HashInt, double>(offset) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::bit_cast<HashInt, double>(deviation) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }

  bool operator==(const CurveGenBase &b) override
  {
    if (b.type != PropCurves::GUASSIAN) {
      return false;
    }
    const CurveGenGuassian *b2 = static_cast<const CurveGenGuassian *>(&b);
    return height == b2->height && offset == b2->offset && deviation == b2->deviation;
  }
};

template <typename Func> void type_dispatch(Func func, PropCurves type)
{
  if (type == PropCurves::STEP) {
    func.template operator()<CurveGenSimple<PropCurves::STEP>>();
  } else if (type == PropCurves::LINEAR) {
    func.template operator()<CurveGenSimple<PropCurves::LINEAR>>();
  } else if (type == PropCurves::SMOOTHSTEP) {
    func.template operator()<CurveGenSimple<PropCurves::SMOOTHSTEP>>();
  } else if (type == PropCurves::SHARP) {
    func.template operator()<CurveGenSimple<PropCurves::SHARP>>();
  } else if (type == PropCurves::SHARPER) {
    func.template operator()<CurveGenSimple<PropCurves::SHARPER>>();
  } else if (type == PropCurves::SQRT) {
    func.template operator()<CurveGenSimple<PropCurves::SQRT>>();
  } else if (type == PropCurves::BSPLINE) {
    func.template operator()<CurveGenBSpline>();
  } else if (type == PropCurves::GUASSIAN) {
    func.template operator()<CurveGenGuassian>();
  } else if (type == PropCurves::TABLE) {
    func.template operator()<CurveGenTable>();
  }
}

struct CurveGen {
  CurveGenBase *gen;

  ~CurveGen()
  {
    if (!gen) {
      return;
    }

    type_dispatch(
        [&]<typename CurveGenType>() {
          alloc::Delete<CurveGenType>(static_cast<CurveGenType *>(gen));
        },
        gen->type);
  }

  CurveGen()
  {
    gen = static_cast<CurveGenBase *>(alloc::New<CurveGenSimple<PropCurves::LINEAR>>(
        "CurveGenSimple<PropCurves::LINEAR>"));
  }

  CurveGen(PropCurves type)
  {
    type_dispatch(
        [&]<typename CurveGenType>() {
          gen = static_cast<CurveGenBase *>(alloc::New<CurveGenType>("CurveGen"));
        },
        type);
  }

  CurveGen(const CurveGen &b)
  {
    type_dispatch(
        [&]<typename CurveGenType>() {
          CurveGenType *gen2 = static_cast<CurveGenType *>(b.gen);
          gen = static_cast<CurveGenBase *>(alloc::New<CurveGenType>("CurveGen", *gen2));
        },
        b.gen->type);
  }

  bool operator==(const CurveGen &b)
  {
    return gen->operator==(*b.gen);
  }

  CurveGen(CurveGen &&b)
  {
    gen = b.gen;
    b.gen = nullptr;
  }

  DEFAULT_MOVE_ASSIGNMENT(CurveGen)

  PropCurves type()
  {
    return gen->type;
  }

  double evaluate(double f)
  {
    return gen->evaluate(f);
  }

  litestl::hash::HashInt hash()
  {
    return gen->hash();
  }
};

/* Sample a curve into a flat LUT for GPU/branchless consumers. Samples at
 * t_i = i/(n-1), storing float(curve.evaluate(t_i)). Deterministic and
 * host-only: the WGSL LUT-fetch reads this same baked buffer, so the two
 * paths stay bit-identical by construction. */
void bake_curve_lut(CurveGenBase &curve, float *out, int n);

template <int N> std::array<float, N> bake_curve_lut(CurveGenBase &curve)
{
  std::array<float, N> lut{};
  bake_curve_lut(curve, lut.data(), N);
  return lut;
}

} // namespace detail::curve

struct CurveGenProp
    : public detail::PropBase<CurveGenProp, detail::curve::CurveGenBase *> {
  using PropBase = detail::PropBase<CurveGenProp, detail::curve::CurveGenBase *>;
  using CurveGenBase = detail::curve::CurveGenBase;

  double min = 0.0, max = 1.0;
  bool clamp = false;

  CurveGenProp() : PropBase(Prop::CURVE2D)
  {
    //
  }
  CurveGenBase **internal_value() override
  {
    return &internal_data_;
  }

  size_t sizeOf() const override
  {
    return sizeof(void *);
  }

  double evaluate(double f)
  {
    CurveGenBase *const curve = PropBase::get();

    if (curve) {
      f = curve->evaluate(f);
    }

    if (clamp) {
      f = std::min(std::max(f, min), max);
    }

    return f;
  }

  CurveGenProp &Min(double f)
  {
    min = f;
    return *this;
  }

  CurveGenProp &Max(double f)
  {
    max = f;
    return *this;
  }

  CurveGenProp &Clamp(bool state)
  {
    clamp = state;
    return *this;
  }

private:
  CurveGenBase *internal_data_ = nullptr;
};

}; // namespace sculptcore::props

namespace litestl::hash {
inline HashInt hash(sculptcore::props::detail::curve::CurveGen &curve)
{
  return curve.hash();
}
} // namespace litestl::hash
