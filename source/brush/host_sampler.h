#pragma once

#include "litestl/util/string.h"

namespace sculptcore::brush {

/** A host-provided procedural field callable from texture scripts through
 * `sampler float name(float3 p[, float3 n]);` declarations (texture-scripts
 * T4). `fn` is required. `fn_grad` writes {value, gx, gy, gz} and is
 * optional — without it the engine synthesizes central differences with step
 * `fd_step` (6 extra `fn` taps per grad() evaluation), CPU and GPU alike.
 * `wgsl` optionally supplies the GPU implementation: it must define
 * `fn hs_<name>(p: vec3f, n: vec3f) -> f32` and may define
 * `fn hs_<name>_grad(p: vec3f, n: vec3f) -> vec4f`. Left empty, the sampler
 * is CPU-only and every texture calling it compiles with
 * gpuAvailable = false. */
struct HostSampler {
  litestl::util::string name;
  float (*fn)(void *user, const float p[3], const float n[3]) = nullptr;
  void (*fn_grad)(void *user, const float p[3], const float n[3], float out[4]) = nullptr;
  void *user = nullptr;
  litestl::util::string wgsl;
  float fd_step = 1e-3f;
};

/** Register a host sampler, or update the existing entry of the same name in
 * place. Entries have stable addresses for the life of the process — a bound
 * TextureProgram holds the pointer, so re-registering changes what it calls
 * without a recompile. Registration is not synchronized against concurrent
 * sampler evaluation; register between strokes. Returns false (registering
 * nothing) when `name` is empty or `fn` is null. */
bool registerHostSampler(const HostSampler &s);

/** Clears the named entry's callbacks and WGSL; the entry itself stays (a
 * bound program calling it reads 0.0 from then on, and programs compiled
 * later fail their dep resolution). Returns false if no such sampler. */
bool unregisterHostSampler(litestl::util::stringref name);

const HostSampler *findHostSampler(litestl::util::stringref name);

int hostSamplerCount();
const HostSampler *hostSamplerEntry(int i);

}  // namespace sculptcore::brush

/** JIT bridges: the emitted C TU reaches host samplers only through these two
 * (bound via tcc_add_symbol), with `s` = the registry entry. Scalar float
 * args sidestep any small-struct ABI mismatch between tcc and the engine
 * compiler. sb_hs_grad writes {value, gx, gy, gz}, synthesizing central
 * differences when the sampler registered no analytic gradient. */
extern "C" float sb_hs_value(
    const void *s, float px, float py, float pz, float nx, float ny, float nz);
extern "C" void sb_hs_grad(
    const void *s, float px, float py, float pz, float nx, float ny, float nz, float *out4);

/** C-api mirror of registerHostSampler for external hosts (the Blender
 * addon's ctypes bridge): the callbacks use the HostSampler signatures, and
 * `wgsl` may be null or empty for a CPU-only sampler. The caller owns the
 * lifetime of `fn`/`fn_grad`/`user` until the sampler is unregistered or
 * re-registered — for ctypes that means keeping the CFUNCTYPE objects alive.
 * Both return 1 on success, 0 on the corresponding false case above. */
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
                                        float fd_step);
extern "C" int sc_host_sampler_unregister(const char *name);
