#pragma once

#include "litestl/math/matrix.h"
#include "litestl/math/vector.h"

#include <cmath>

namespace sculptcore::debug_app {

using litestl::math::float3;
using litestl::math::mat4;

/** Right-handed perspective + lookAt matching the GL clip-space convention.
 *  Column-major to match `litestl::math::mat4` (Eigen ColMajor default). */
inline mat4 perspective(float fovyRad, float aspect, float zn, float zf)
{
  mat4 m;
  m.zero();
  float f = 1.0f / std::tan(fovyRad * 0.5f);
  float *d = static_cast<float *>(m);
  d[0] = f / aspect;
  d[5] = f;
  d[10] = (zf + zn) / (zn - zf);
  d[11] = -1.0f;
  d[14] = (2.0f * zf * zn) / (zn - zf);
  return m;
}

inline mat4 lookAt(float3 eye, float3 target, float3 up)
{
  float3 f = target - eye;
  f.normalize();
  float3 s = f.cross(up);
  s.normalize();
  float3 u = s.cross(f);

  mat4 m;
  m.identity();
  float *d = static_cast<float *>(m);
  d[0] = s[0]; d[4] = s[1]; d[8]  = s[2];
  d[1] = u[0]; d[5] = u[1]; d[9]  = u[2];
  d[2] = -f[0]; d[6] = -f[1]; d[10] = -f[2];
  d[12] = -s.dot(eye);
  d[13] = -u.dot(eye);
  d[14] = f.dot(eye);
  return m;
}

inline mat4 mul(const mat4 &a, const mat4 &b)
{
  mat4 r;
  const float *ad = static_cast<const float *>(a);
  const float *bd = static_cast<const float *>(b);
  float *rd = static_cast<float *>(r);
  for (int c = 0; c < 4; c++) {
    for (int row = 0; row < 4; row++) {
      float s = 0.0f;
      for (int k = 0; k < 4; k++) {
        s += ad[k * 4 + row] * bd[c * 4 + k];
      }
      rd[c * 4 + row] = s;
    }
  }
  return r;
}

struct Camera {
  float3 eye{2.5f, 2.5f, 2.5f};
  float3 target{0, 0, 0};
  float3 up{0, 0, 1};
  float fovy = 0.9f;
  float zn = 0.05f;
  float zf = 100.0f;

  mat4 viewProj(float aspect) const
  {
    return mul(perspective(fovy, aspect, zn, zf), lookAt(eye, target, up));
  }

  /** Frame an AABB so it fills the view from `direction`. */
  void frame(float3 aabbMin, float3 aabbMax, float3 direction, float pad = 1.5f)
  {
    float3 c = (aabbMin + aabbMax) * 0.5f;
    float3 ext = aabbMax - aabbMin;
    float r = std::sqrt(ext[0] * ext[0] + ext[1] * ext[1] + ext[2] * ext[2]) * 0.5f;
    if (r < 1e-4f) {
      r = 1.0f;
    }
    float3 d = direction;
    d.normalize();
    float dist = r / std::tan(fovy * 0.5f) * pad;
    target = c;
    eye = c + d * dist;
    if (std::fabs(d[2]) > 0.95f) {
      up = float3(0, 1, 0);
    } else {
      up = float3(0, 0, 1);
    }
  }
};

} // namespace sculptcore::debug_app
