#pragma once

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>

namespace sculptcore::math {
template <typename T, int size> class Vec {
public:
  static const int size = size;

  Vec()
  {
  }

  Vec(T single)
  {
    for (int i = 0; i < size; i++) {
      vec_[i] = single;
    }
  }

  Vec(const T *value)
  {
    for (int i = 0; i < size; i++) {
      vec_[i] = value[i];
    }
  }

  Vec(const Vec &b)
  {
    for (int i = 0; i < size; i++) {
      vec_[i] = b.vec_[i];
    }
  }

  inline Vec &zero()
  {
    for (int i = 0; i < size; i++) {
      vec_[i] = T(0);
    }

    return *this;
  }

  inline T &operator[](int idx)
  {
    return vec_[idx];
  }

  inline T operator[](int idx) const
  {
    return vec_[idx];
  }

#ifdef VEC_OP_DEF
#undef VEC_OP_DEF
#endif

#define VEC_OP_DEF(op)                                                                   \
  inline Vec operator##op(const Vec &b)                                                  \
  {                                                                                      \
    Vec r;                                                                               \
    for (int i = 0; i < size; i++) {                                                     \
      r[i] = vec_[i] op b.vec_[i];                                                       \
    }                                                                                    \
    return r;                                                                            \
  }                                                                                      \
  inline Vec operator##op(T b)                                                           \
  {                                                                                      \
    Vec r;                                                                               \
    for (int i = 0; i < size; i++) {                                                     \
      r[i] = vec_[i] op b;                                                               \
    }                                                                                    \
    return r;                                                                            \
  }                                                                                      \
  inline Vec &operator##op##=(T b)                                                       \
  {                                                                                      \
    for (int i = 0; i < size; i++) {                                                     \
      vec_[i] op## = b;                                                                  \
    }                                                                                    \
    return *this;                                                                        \
  }                                                                                      \
  inline Vec &operator##op##=(const Vec b)                                               \
  {                                                                                      \
    for (int i = 0; i < size; i++) {                                                     \
      vec_[i] op## = b.vec_[i];                                                          \
    }                                                                                    \
    return *this;                                                                        \
  }

  VEC_OP_DEF(*)
  VEC_OP_DEF(/)
  VEC_OP_DEF(+)
  VEC_OP_DEF(-)

  T dot(const Vec &b)
  {
    T sum = T(0);
    for (int i = 0; i < size; i++) {
      sum += vec_[i] * b.vec_[i];
    }
  }

  T normalize()
  {
    T len = length();
    if (len > 0.00000001) {
      double mul = 1.0 / double(len);

      for (int i = 0; i < size; i++) {
        vec_[i] = T(double(vec_[i]) * mul);
      }
    } else {
      zero();
      len = 0.0;
    }

    return len;
  }

  T length()
  {
    return std::sqrt(dot(*this));
  }

  void interp(const Vec &b, double factor)
  {
    for (int i = 0; i < size; i++) {
      vec_[i] += T(double(b.vec_[i] - vec_[i]) * factor);
    }
  }

  operator T *()
  {
    return vec_;
  }

  void print()
  {
    printf("(");
    for (int i = 0; i < size; i++) {
      if (i > 0) {
        printf(" ");
      }
      printf("%.4f", vec_[i]);
    }
    printf(")");
  }

private:
  T vec_[size];
};

using float1 = Vec<float, 1>;
using float2 = Vec<float, 2>;
using float3 = Vec<float, 3>;
using float4 = Vec<float, 4>;

using int1 = Vec<int, 1>;
using int2 = Vec<int, 2>;
using int3 = Vec<int, 3>;
using int4 = Vec<int, 4>;


} // namespace sculptcore::math
