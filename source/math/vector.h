#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace sculptcore::math {
template <typename T, int size> class Vec {
public:
  static const int size = size;

  Vec()
  {
  }

  Vec(T a, T b)
  {
    vec_[0] = a;
    vec_[1] = b;

    for (int i = 2; i < size; i++) {
      vec_[i] = 0.0f;
    }
  }

  Vec(T a, T b, T c)
  {
    vec_[0] = a;
    vec_[1] = b;
    vec_[2] = c;

    for (int i = 3; i < size; i++) {
      vec_[i] = 0.0f;
    }
  }

  Vec(T a, T b, T c, T d)
  {
    vec_[0] = a;
    vec_[1] = b;
    vec_[2] = c;
    vec_[3] = d;

    for (int i = 4; i < size; i++) {
      vec_[i] = 0.0f;
    }
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

  Vec &min(const Vec &b)
  {
    for (int i = 0; i < size; i++) {
      vec_[i] = std::min(vec_[i], b.vec_[i]);
    }

    return *this;
  }

  Vec &max(const Vec &b)
  {
    for (int i = 0; i < size; i++) {
      vec_[i] = std::max(vec_[i], b.vec_[i]);
    }

    return *this;
  }

  T dot(const Vec &b)
  {
    T sum = T(0);
    for (int i = 0; i < size; i++) {
      sum += vec_[i] * b.vec_[i];
    }

    return sum;
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

  Vec &interp(const Vec &b, double factor)
  {
    for (int i = 0; i < size; i++) {
      vec_[i] += T(double(b.vec_[i] - vec_[i]) * factor);
    }

    return *this;
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

#ifdef DEF_VECS
#undef DEF_VECS
#endif

#define DEF_VECS(type, name)                                                             \
  using name##1 = Vec<type, 1>;                                                          \
  using name##2 = Vec<type, 2>;                                                          \
  using name##3 = Vec<type, 3>;                                                          \
  using name##4 = Vec<type, 4>

DEF_VECS(float, float);
DEF_VECS(double, double);
DEF_VECS(int32_t, int);
DEF_VECS(uint32_t, uint);
DEF_VECS(int16_t, short);
DEF_VECS(uint16_t, ushort);
DEF_VECS(int8_t, char);
DEF_VECS(uint8_t, uchar);

#undef DEF_VECS
} // namespace sculptcore::math
