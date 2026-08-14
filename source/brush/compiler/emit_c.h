#pragma once
#include "emit_cpp.h"

namespace sculptcore::brush::sbrush {

/** C99 texture backend (texture-scripts T3.2): lowers a scratch brush wrapping
 * a .stex unit's textures to one freestanding C translation unit — the source
 * the runtime tinycc JIT compiles (compileTextureScript, T3.3). Semantics
 * mirror the cpp texture path bit-for-bit (params slab, TexEvalCtx/mapPoint,
 * ramp sampling, the sbd_* dual prelude), and every differentiable texture
 * gets its dual twin up front so grad availability never needs a re-JIT; a
 * non-differentiable eval (ramp.sample) keeps its value entry and simply
 * omits the twin.
 *
 * The TU has no #includes; its only external symbols are libm's
 * sinf/cosf/sqrtf/floorf/fabsf plus, for units calling host samplers (T4),
 * the sb_hs_value/sb_hs_grad bridges (tcc_add_symbol, or -lm standalone;
 * sampler units cannot run standalone). Each sampler dep also gets a
 * TU-defined `void *sb_hs_<name>` slot the host points at the registry entry
 * after relocation. Exported entry points use a pointer ABI:
 *   float tex_<name>_eval(const float *p, const float *n,
 *                         const float *sb_tex_params, const TexEvalCtx *ctx);
 *   void tex_<name>_eval_d(const sbdual3 *p, const sbdual3 *n,
 *                          const float *sb_tex_params, const TexEvalCtx *ctx,
 *                          sbdual *out);
 * plus a `tex_<name>_param_defaults` slab when the texture declares runtime
 * params. No manifest is emitted — T3.3 reads the parsed AST in-process. */
EmitResult emitCTextureDefs(const Brush &brush);

} // namespace sculptcore::brush::sbrush
