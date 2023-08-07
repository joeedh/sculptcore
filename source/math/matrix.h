#pragma once

#include "eigen/include/eigen3/Eigen/Core"
#include "eigen/include/eigen3/Eigen/LU"
#include "math/vector.h"

namespace sculptcore::math {
template <typename Float, int N> struct Matrix {
  using Vector = Vec<Float, N>;
  using Vec3 = Vec<Float, 3>;
  using Vec4 = Vec<float, 4>;
  using EigenMatrix = Eigen::Matrix<Float, N, N>;

  Matrix()
  {
  }

  Matrix(const Float *data)
  {
    Float *mat = &mat_[0][0];
    for (int i = 0; i < N * N; i++, mat++, data++) {
      *mat = *data;
    }
  }

  Matrix(const Matrix &b) : mat_(b.mat_)
  {
  }

  Matrix(const EigenMatrix &b)
  {
    /* Use assignment operator. */
    *this = b;
  }

  operator Float *() const
  {
    return &mat_[0][0];
  }

  inline Matrix &identity()
  {
    zero();

    for (int i = 0; i < N; i++) {
      mat_[i][i] = 1.0f;
    }
    return *this;
  }

  static inline constexpr Matrix Identity()
  {
    Matrix r;
    r.identity();
    return r;
  }

  inline Matrix &zero()
  {
    for (int i = 0; i < N; i++) {
      mat_[i].zero();
    }

    return *this;
  }

  Matrix &transpose()
  {
    for (int i = 0; i < N; i++) {
      for (int j = 0; j < i; j++) {
        std::swap(mat_[i][j], mat_[j][i]);
      }
    }

    return *this;
  }

  Matrix &invert()
  {
#if 0
    EigenMatrix *m = reinterpret_cast < EigenMatrix *>(this);
    *m = m->inverse();
    return *this;
#else
    EigenMatrix m(&mat_[0][0]);
    m = m.inverse();
    *this = m;
#endif

    return *this;
  }

  template <int M> inline Vec<Float, M> operator*(const Vec<Float, M> v) const
  {
    Vec<Float, M> r;

    constexpr int n = N < M ? N : M;

    for (int i = 0; i < n; i++) {
      Float sum = Float(0);

      for (int j = 0; j < n; j++) {
        sum += v[j] * mat_[j][i];
      }

      if constexpr (N == 4 && M == 3) {
        sum += mat_[3][i];
      }

      r[i] = sum;
    }

    return r;
  }

  Matrix operator*(const Matrix &b) const
  {
    Matrix r;

    for (int i = 0; i < N; i++) {
      for (int j = 0; j < N; j++) {
        Float sum = Float(0);
        for (int k = 0; k < N; j++) {
          sum += mat_[j][k] * b.mat_[k][j];
        }
        r.mat_[i][j] = sum;
      }
    }

    return r;
  }

  Matrix &operator*=(const Matrix &b)
  {
    Matrix r = *this * b;
    *this = r;
    return r;
  }

  Matrix &operator=(const Matrix &b)
  {
    for (int i = 0; i < N; i++) {
      for (int j = 0; j < N; j++) {
        mat_[i][j] = b.mat_[i][j];
      }
    }
  }

  operator EigenMatrix() const
  {
    return EigenMatrix(&mat_[0][0]);
  }

  Matrix &operator=(const EigenMatrix &b)
  {
    for (int i = 0; i < N; i++) {
      for (int j = 0; j < N; j++) {
        mat_[i][j] = b(i, j);
      }
    }
  }

  Vector &operator[](int idx)
  {
    return mat_[idx];
  }

  int size()
  {
    return N;
  }

private:
  Vector mat_[N];
};

using mat3 = Matrix<float, 3>;
using mat4 = Matrix<float, 4>;

} // namespace sculptcore::math
