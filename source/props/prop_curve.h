#pragma once

#include "prop_enums.h"
#include "prop_types.h"

#include "math/bspline.h"
#include "util/alloc.h"
#include "util/array.h"
#include "util/hash.h"

#include <bit>
#include <cmath>
#include <utility>

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
using sculptcore::util::Array;

struct CurveGenBase {
  PropCurves type;

  CurveGenBase(PropCurves type_) : type(type_)
  {
  }

  virtual double evaluate(double f)
  {
    return f;
  }

  virtual sculptcore::hash::HashInt hash()
  {
    return 0;
  }

  virtual bool operator==(const CurveGenBase &b)
  {
    return b.type == type;
  }
};

template <PropCurves type_> struct CurveGenSimple : public CurveGenBase {
  using HashInt = sculptcore::hash::HashInt;

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
  Array<double> table;
  using HashInt = sculptcore::hash::HashInt;

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

struct CurveGenBSpline : CurveGenBase {
  CurveGenBSpline() : CurveGenBase(PropCurves::BSPLINE)
  {
  }
};

struct CurveGenGuassian : CurveGenBase {
  CurveGenGuassian() : CurveGenBase(PropCurves::GUASSIAN)
  {
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

  CurveGen &operator=(CurveGen &&b)
  {
    gen = b.gen;
    b.gen = nullptr;

    return *this;
  }

  PropCurves type()
  {
    return gen->type;
  }

  double evaluate(double f)
  {
    return gen->evaluate(f);
  }

  sculptcore::hash::HashInt hash()
  {
    return gen->hash();
  }
};

} // namespace detail::curve

struct CurveGenProp
    : public detail::PropBase<CurveGenProp, detail::curve::CurveGenBase *> {
  using PropBase = detail::PropBase<CurveGenProp, detail::curve::CurveGenBase *>;
  using CurveGenBase = detail::curve::CurveGenBase;

  double min = 0.0, max = 1.0;
  bool clamp = false;

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
    CurveGenBase *curve = PropBase::get();

    if (curve) {
      f = curve->evaluate(f);
    }

    if (clamp) {
      f = std::min(std::max(f, max), min);
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

namespace sculptcore::hash {
inline HashInt hash(sculptcore::props::detail::curve::CurveGen &curve)
{
  return curve.hash();
}
} // namespace sculptcore::hash
