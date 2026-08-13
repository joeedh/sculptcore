#pragma once

#include "litestl/math/vector.h"

#include <cmath>

namespace sculptcore::brush {

// Runtime support for DSL procedural textures — inline `texture` blocks in
// .sbrush kernels and standalone .stex units. Generated texture code
// (kernels/generated/*.tex.gen.h and the inline-texture lowering in emitted
// brush headers) compiles against this header. See
// documentation/plans/texture-scripts.md.

// Slots one `param ramp` occupies in a texture's parameter slab. The compiler
// IR (compiler/ir.h) carries its own copy of this constant for slab-offset
// assignment; generated headers static_assert the two agree.
inline constexpr int kTexRampSize = 256;

// Evaluation context threaded into a texture eval whose body calls
// mapPoint(). A null ctx makes mapPoint a pass-through; kept a plain float
// array (not mat4) so the T3 runtime-compile path needs no litestl types in
// its ABI.
struct TexEvalCtx {
  // Same flat layout as litestl::math::mat4 (memcpy-compatible).
  float map_matrix[16];
};

// mapPoint(p): q = M * (p, 1); q.xyz / (|q.w| > 1e-6 ? q.w : 1). The flat
// indexing reproduces litestl's `mat4 * float4` exactly, and the 1e-6 guard is
// part of the ABI — the WGSL emitter mirrors it so CPU and GPU evaluate in
// lockstep (same pairing as sampleViewUv / brush_view_uv).
inline litestl::math::float3 texMapPoint(const TexEvalCtx *ctx, litestl::math::float3 p)
{
  if (!ctx) {
    return p;
  }
  const float *m = ctx->map_matrix;
  float x = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
  float y = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
  float z = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
  float w = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];
  float d = std::abs(w) > 1e-6f ? w : 1.0f;
  return litestl::math::float3{x / d, y / d, z / d};
}

// Build a TexEvalCtx from a 16-float matrix (litestl mat4 converts via its
// `operator const float *`). Generated stage prologues use this to snapshot
// ctx.renderMatrix for textures whose eval calls mapPoint().
inline TexEvalCtx texEvalCtxFrom(const float *m16)
{
  TexEvalCtx c;
  for (int i = 0; i < 16; i++) {
    c.map_matrix[i] = m16[i];
  }
  return c;
}

// Sample a `param ramp` slab region at t, linearly interpolated with clamped
// ends. `ramp` points at the param's kTexRampSize slots.
inline float texRampSample(const float *ramp, float t)
{
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  float x = t * float(kTexRampSize - 1);
  int i = int(x);
  if (i >= kTexRampSize - 1) {
    return ramp[kTexRampSize - 1];
  }
  float f = x - float(i);
  return ramp[i] + (ramp[i + 1] - ramp[i]) * f;
}

// Codegen-emitted descriptor of one non-@const texture `param` — the texture
// analogue of BrushUniformManifestEntry. Static data in generated headers, so
// the name is a literal rather than a util::string.
struct TexParamManifestEntry {
  const char *name;
  bool isRamp;    // one float slot when false, kTexRampSize slots when true
  float def;      // authored default (`= <n>`); ramps default to identity
  bool hasRange;  // `@range(min, max)` present
  float rangeMin;
  float rangeMax;
  int offset;  // first slot in the texture's param slab
};

}  // namespace sculptcore::brush
