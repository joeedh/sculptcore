#pragma once

#include "brush/texture_eval.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

namespace sculptcore::brush {

/** Mirrors of the JIT'd TU's sbdual / sbdual3 structs (plain float layout —
 * value plus per-seed-component partials; an identity seed makes the scalar
 * result's d the spatial gradient). */
struct TexDual {
  float v;
  float d[3];
};
struct TexDual3 {
  float v[3];
  float dx[3];
  float dy[3];
  float dz[3];
};

/** One texture `param` of a runtime-compiled program — the owned-string
 * analogue of TexParamManifestEntry (whose names are static literals in
 * generated headers). `@const` params are frozen into the compiled code:
 * offset is -1 and changing one means recompiling the program. */
struct TextureProgramParam {
  litestl::util::string name;
  bool isRamp = false;
  bool isConst = false;
  float def = 0.0f;  // Float/Int default; ramps default to identity
  bool hasRange = false;
  float rangeMin = 0.0f;
  float rangeMax = 0.0f;
  int offset = -1;  // first slot in the param slab; -1 for @const
};

/** A texture script compiled at runtime (texture-scripts T3): the tinycc-JIT'd
 * CPU entries plus the unit's WGSL module for the T5 stroke-shader splice.
 * Owns the JIT state — the function pointers die with the program. */
struct TextureProgram {
  litestl::util::string name;

  /** The JIT'd value entry — same semantics as TextureRegistryEntry::eval,
   * pointer ABI. Pass `defaults.data()` or a runtime slab of `paramSlabSize`
   * floats; ctx may be null (mapPoint becomes a pass-through). */
  float (*eval)(const float *p, const float *n, const float *params, const TexEvalCtx *ctx) =
      nullptr;
  /** The dual twin, or null when the eval is not differentiable
   * (ramp.sample / sampler calls have no derivative rule). */
  void (*evalDual)(const TexDual3 *p,
                   const TexDual3 *n,
                   const float *params,
                   const TexEvalCtx *ctx,
                   TexDual *out) = nullptr;

  litestl::util::string wgsl;  // unit WGSL module text; empty if emission failed
  litestl::util::Vector<litestl::util::string> samplerDeps;
  bool gpuAvailable = false;  // wgsl present and every samplerDep has a GPU impl
  bool usesMap = false;       // eval calls mapPoint() — feed the ctx a real matrix

  int paramSlabSize = 0;
  litestl::util::Vector<float> defaults;  // paramSlabSize floats
  litestl::util::Vector<TextureProgramParam> params;

  /** The script source, kept for `@const` re-specialization (edit the const,
   * recompile — milliseconds under tcc). */
  litestl::util::string source;

  TextureProgram() = default;
  TextureProgram(const TextureProgram &) = delete;
  TextureProgram &operator=(const TextureProgram &) = delete;
  ~TextureProgram();

  void *jit_state = nullptr;  // owned TCCState
};

/** Compile one .stex source (exactly one texture per script) to a
 * TextureProgram: parse -> emit C99 -> tinycc JIT -> resolve entries. Returns
 * null with `error` filled on any failure — including when
 * textureScriptCpuAvailable() is false (WASM, or a hardened-runtime host
 * without JIT pages); hosts degrade to the precompiled registry then.
 * Free the result with freeTextureProgram. */
TextureProgram *compileTextureScript(litestl::util::stringref source,
                                     litestl::util::stringref filename,
                                     litestl::util::string &error);

void freeTextureProgram(TextureProgram *program);

}  // namespace sculptcore::brush
