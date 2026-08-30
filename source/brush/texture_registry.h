#pragma once

#include "brush/texture_eval.h"
#include "litestl/math/vector.h"
#include "litestl/util/string.h"

namespace sculptcore::brush {

/** One precompiled standalone texture (a .stex unit member) — the registry
 * row generated into sculptcore_textures.gen.h by `sbrushc
 * --texture-registry`. Hosts look textures up by name to bind them as the
 * procedural brush texture (T3) and to splice their WGSL into brush shaders
 * (T5); documentation/plans/texture-scripts.md. */
struct TextureRegistryEntry {
  const char *name;
  /** The precompiled eval — the same array ABI brush call sites use. Pass
   * `defaults` (or a runtime slab of `slabSize` floats) and a TexEvalCtx
   * (null is valid; mapPoint becomes a pass-through). */
  float (*eval)(litestl::math::float3 p,
                litestl::math::float3 n,
                const float *params,
                const TexEvalCtx *ctx);
  const float *defaults; // authored param defaults; null when slabSize == 0
  int slabSize;
  const TexParamManifestEntry *params; // runtime-adjustable params, decl order
  int paramCount;
  const char *wgsl; // the owning unit's WGSL module text
  bool usesMap;     // eval calls mapPoint() — feed the ctx a real matrix
};

int textureRegistryCount();
const TextureRegistryEntry *textureRegistryEntry(int i);

/** Name lookup (case-sensitive, the .stex `texture <Name>` spelling); null
 * when absent. */
const TextureRegistryEntry *findTexture(litestl::util::stringref name);

} // namespace sculptcore::brush
