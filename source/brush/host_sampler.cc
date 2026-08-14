#include "host_sampler.h"

#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

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

  HostSampler *hs = findMutable(s.name.c_str());
  if (!hs) {
    litestl::alloc::PermanentGuard guard;
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

}  // namespace sculptcore::brush

using sculptcore::brush::HostSampler;

extern "C" float sb_hs_value(
    const void *s, float px, float py, float pz, float nx, float ny, float nz)
{
  const HostSampler *hs = (const HostSampler *)s;
  if (!hs || !hs->fn) {
    return 0.0f;
  }

  const float p[3] = {px, py, pz};
  const float n[3] = {nx, ny, nz};
  return hs->fn(hs->user, p, n);
}

extern "C" int sc_host_sampler_register(const char *name,
                                        float (*fn)(void *user,
                                                    const float p[3],
                                                    const float n[3]),
                                        void (*fn_grad)(void *user,
                                                        const float p[3],
                                                        const float n[3],
                                                        float out[4]),
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

extern "C" void sb_hs_grad(
    const void *s, float px, float py, float pz, float nx, float ny, float nz, float *out4)
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
