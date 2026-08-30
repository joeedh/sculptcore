#include "host_sampler.h"

#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace sculptcore::brush {

using litestl::util::string;
using litestl::util::stringref;
using litestl::util::Vector;

/** Heap entries with stable addresses, never freed: JIT'd TUs bind
 * sb_hs_<name> handles straight to these pointers, so an entry must outlive
 * every program compiled against it. Unregister clears callbacks in place
 * rather than removing the entry. */
static Vector<HostSampler *> &registry()
{
  static Vector<HostSampler *> table;
  return table;
}

static HostSampler *findMutable(const char *name)
{
  for (HostSampler *hs : registry()) {
    if (std::strcmp(hs->name.c_str(), name) == 0) {
      return hs;
    }
  }
  return nullptr;
}

bool registerHostSampler(const HostSampler &s)
{
  if (s.name.size() == 0 || !s.fn) {
    return false;
  }

  // The guard spans the string copies below too: an entry (and its wgsl
  // buffer) lives for the process, so the leak tracker must not report it.
  litestl::alloc::PermanentGuard guard;
  HostSampler *hs = findMutable(s.name.c_str());
  if (!hs) {
    hs = litestl::alloc::New<HostSampler>("HostSampler");
    hs->name = s.name;
    registry().append(hs);
  }

  hs->fn = s.fn;
  hs->fn_grad = s.fn_grad;
  hs->user = s.user;
  hs->wgsl = s.wgsl;
  hs->fd_step = s.fd_step > 0.0f ? s.fd_step : 1e-3f;
  return true;
}

bool unregisterHostSampler(stringref name)
{
  HostSampler *hs = findMutable(name.c_str());
  if (!hs) {
    return false;
  }

  hs->fn = nullptr;
  hs->fn_grad = nullptr;
  hs->user = nullptr;
  hs->wgsl = string("");
  return true;
}

const HostSampler *findHostSampler(stringref name)
{
  return findMutable(name.c_str());
}

/** Builtin `vnoise`: one octave of 3D lattice value noise in [0, 1).
 * Corner values come from an integer hash of the cell coords (multiply-xor
 * mix, lowbias32-style avalanche), blended trilinearly with smoothstep
 * fades — the standard value-noise construction. Sharing a corner's lattice
 * coords across the 8 cells that touch it is what makes the field C0 across
 * cell borders. */
static uint32_t vnoiseHash(int32_t x, int32_t y, int32_t z)
{
  uint32_t h = (uint32_t)x * 0x8da6b343u;
  h ^= (uint32_t)y * 0xd8163841u;
  h ^= (uint32_t)z * 0xcb1ab31fu;
  h ^= h >> 13;
  h *= 0x7feb352du;
  h ^= h >> 15;
  return h;
}

static float vnoiseCorner(int32_t x, int32_t y, int32_t z)
{
  return (float)vnoiseHash(x, y, z) * (1.0f / 4294967296.0f);
}

static float vnoiseEval(void * /*user*/, const float p[3], const float /*n*/[3])
{
  const float bx = std::floor(p[0]);
  const float by = std::floor(p[1]);
  const float bz = std::floor(p[2]);
  const int32_t ix = (int32_t)bx, iy = (int32_t)by, iz = (int32_t)bz;

  float fx = p[0] - bx, fy = p[1] - by, fz = p[2] - bz;
  fx = fx * fx * (3.0f - 2.0f * fx);
  fy = fy * fy * (3.0f - 2.0f * fy);
  fz = fz * fz * (3.0f - 2.0f * fz);

  const float h000 = vnoiseCorner(ix, iy, iz);
  const float h100 = vnoiseCorner(ix + 1, iy, iz);
  const float h010 = vnoiseCorner(ix, iy + 1, iz);
  const float h110 = vnoiseCorner(ix + 1, iy + 1, iz);
  const float h001 = vnoiseCorner(ix, iy, iz + 1);
  const float h101 = vnoiseCorner(ix + 1, iy, iz + 1);
  const float h011 = vnoiseCorner(ix, iy + 1, iz + 1);
  const float h111 = vnoiseCorner(ix + 1, iy + 1, iz + 1);

  const float m00 = h000 + (h100 - h000) * fx;
  const float m10 = h010 + (h110 - h010) * fx;
  const float m01 = h001 + (h101 - h001) * fx;
  const float m11 = h011 + (h111 - h011) * fx;
  const float m0 = m00 + (m10 - m00) * fy;
  const float m1 = m01 + (m11 - m01) * fy;
  return m0 + (m1 - m0) * fz;
}

// The WGSL twin mirrors vnoiseEval term for term (bitcast<u32> matches the
// C++ modular int-to-uint casts; the a + (b - a) * t lerp form matches the
// CPU's rounding more closely than WGSL's mix builtin).
static const char *kVnoiseWgsl =
    "fn hs_vnoise_hash(x: i32, y: i32, z: i32) -> f32 {\n"
    "  var h: u32 = bitcast<u32>(x) * 0x8da6b343u;\n"
    "  h = h ^ (bitcast<u32>(y) * 0xd8163841u);\n"
    "  h = h ^ (bitcast<u32>(z) * 0xcb1ab31fu);\n"
    "  h = h ^ (h >> 13u);\n"
    "  h = h * 0x7feb352du;\n"
    "  h = h ^ (h >> 15u);\n"
    "  return f32(h) * 2.3283064365386963e-10;\n"
    "}\n"
    "fn hs_vnoise(p: vec3f, n: vec3f) -> f32 {\n"
    "  let b = floor(p);\n"
    "  let i = vec3i(b);\n"
    "  let f = p - b;\n"
    "  let u = f * f * (3.0 - 2.0 * f);\n"
    "  let h000 = hs_vnoise_hash(i.x, i.y, i.z);\n"
    "  let h100 = hs_vnoise_hash(i.x + 1, i.y, i.z);\n"
    "  let h010 = hs_vnoise_hash(i.x, i.y + 1, i.z);\n"
    "  let h110 = hs_vnoise_hash(i.x + 1, i.y + 1, i.z);\n"
    "  let h001 = hs_vnoise_hash(i.x, i.y, i.z + 1);\n"
    "  let h101 = hs_vnoise_hash(i.x + 1, i.y, i.z + 1);\n"
    "  let h011 = hs_vnoise_hash(i.x, i.y + 1, i.z + 1);\n"
    "  let h111 = hs_vnoise_hash(i.x + 1, i.y + 1, i.z + 1);\n"
    "  let m00 = h000 + (h100 - h000) * u.x;\n"
    "  let m10 = h010 + (h110 - h010) * u.x;\n"
    "  let m01 = h001 + (h101 - h001) * u.x;\n"
    "  let m11 = h011 + (h111 - h011) * u.x;\n"
    "  let m0 = m00 + (m10 - m00) * u.y;\n"
    "  let m1 = m01 + (m11 - m01) * u.y;\n"
    "  return m0 + (m1 - m0) * u.z;\n"
    "}\n";

void registerBuiltinHostSamplers()
{
  static bool done = false;
  if (done) {
    return;
  }
  done = true;

  HostSampler s;
  s.name = string("vnoise");
  s.fn = vnoiseEval;
  s.fn_grad = nullptr;
  s.user = nullptr;
  s.wgsl = string(kVnoiseWgsl);
  s.fd_step = 1e-3f;
  registerHostSampler(s);
}

int hostSamplerCount()
{
  return (int)registry().size();
}

const HostSampler *hostSamplerEntry(int i)
{
  if (i < 0 || i >= (int)registry().size()) {
    return nullptr;
  }
  return registry()[i];
}

} // namespace sculptcore::brush

using sculptcore::brush::HostSampler;

extern "C" float
sb_hs_value(const void *s, float px, float py, float pz, float nx, float ny, float nz)
{
  const HostSampler *hs = (const HostSampler *)s;
  if (!hs || !hs->fn) {
    return 0.0f;
  }

  const float p[3] = {px, py, pz};
  const float n[3] = {nx, ny, nz};
  return hs->fn(hs->user, p, n);
}

extern "C" int sc_host_sampler_register(
    const char *name,
    float (*fn)(void *user, const float p[3], const float n[3]),
    void (*fn_grad)(void *user, const float p[3], const float n[3], float out[4]),
    void *user,
    const char *wgsl,
    float fd_step)
{
  HostSampler s;
  s.name = litestl::util::string(name ? name : "");
  s.fn = fn;
  s.fn_grad = fn_grad;
  s.user = user;
  s.wgsl = litestl::util::string(wgsl ? wgsl : "");
  s.fd_step = fd_step;
  return sculptcore::brush::registerHostSampler(s) ? 1 : 0;
}

extern "C" int sc_host_sampler_unregister(const char *name)
{
  if (!name) {
    return 0;
  }
  return sculptcore::brush::unregisterHostSampler(litestl::util::stringref(name)) ? 1 : 0;
}

extern "C" void sb_hs_grad(const void *s,
                           float px,
                           float py,
                           float pz,
                           float nx,
                           float ny,
                           float nz,
                           float *out4)
{
  const HostSampler *hs = (const HostSampler *)s;
  if (!hs || !hs->fn) {
    out4[0] = out4[1] = out4[2] = out4[3] = 0.0f;
    return;
  }

  const float p[3] = {px, py, pz};
  const float n[3] = {nx, ny, nz};

  if (hs->fn_grad) {
    hs->fn_grad(hs->user, p, n, out4);
    return;
  }

  // Central differences: value at p, then ±fd_step along each axis (6 taps).
  out4[0] = hs->fn(hs->user, p, n);
  const float h = hs->fd_step > 0.0f ? hs->fd_step : 1e-3f;
  for (int i = 0; i < 3; i++) {
    float q[3] = {px, py, pz};
    q[i] += h;
    const float hi = hs->fn(hs->user, q, n);
    q[i] = p[i] - h;
    const float lo = hs->fn(hs->user, q, n);
    out4[1 + i] = (hi - lo) / (2.0f * h);
  }
}
