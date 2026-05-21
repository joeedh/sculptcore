#pragma once

#include "litestl/util/string.h"

namespace sculptcore::gpu {
struct DrawPipeline;
struct DrawBatch;
struct DrawCommand;
struct ShaderDef;
struct UniformBlockDef;
struct UniformBlockInstance;

/* Outcome of linkPipeline / linkCommand. `ok == false` means the pipeline
 * cannot be rendered safely; the human-readable `error` describes what went
 * wrong (missing block, duplicate, type mismatch). */
struct LinkResult {
  bool ok = true;
  litestl::util::string error;
};

/* Resolve every UniformBlockInstance on `pipe` (across pipe.blocks,
 * batch.blocks, cmd.blocks) against its shader's `uniforms` array. After a
 * successful link, each instance has:
 *   - `def->set / def->binding` stamped (set = 0 for pipe, 1 for batch, 2 for
 *     command — overrides any shader hint),
 *   - `def->packedBytes` and `def->fieldOffsets` filled in via std140 layout,
 *   - `def->fields` populated from the shader's schema if the layer didn't
 *     provide its own (the spatial migration provides its own; future
 *     producers can declare just the name),
 *   - `data` resized to packedBytes and pre-filled with each field's
 *     defaultValue.
 *
 * Validation: every shader block must be covered by exactly one instance
 * across all three layers. A missing or duplicated block is a link error. */
LinkResult linkPipeline(DrawPipeline &pipe);

/* Same, but link a single command in isolation (no pipeline / batch
 * context). Used by tests and by callers that don't have a full pipeline. */
LinkResult linkCommand(DrawCommand &cmd);

/* Compute std140 layout (`packedBytes`, `fieldOffsets`) for every block in
 * `shader->uniforms`. Idempotent. Call once after constructing a ShaderDef
 * so backends can size UBOs without re-linking. */
void linkShaderDef(ShaderDef *shader);

/* Resolve a single instance against a shader, using `setIndex` as the
 * fallback set assignment if the shader's block leaves it unset. Exposed for
 * backends that need to synthesize/populate instances on the fly (e.g. the
 * Vulkan DrawUniforms backward-compat shim). */
LinkResult linkInstanceAgainstShader(UniformBlockInstance *inst,
                                     ShaderDef *shader,
                                     uint32_t setIndex);

namespace uniform_link_detail {

/* Compute std140 (alignment, size) for a field described by `(type, elemSize)`.
 * Exposed for tests; treats elemSize==16 as mat4-as-4-vec4. */
struct FieldLayout {
  uint32_t align;
  uint32_t size;
};
FieldLayout std140For(int gpuType, int elemSize);

/* Compute layout for an entire UniformBlockDef in place: populates
 * `block->fieldOffsets` (parallel to `block->fields`) and `block->packedBytes`
 * (rounded up to 16). */
void computeStd140Layout(UniformBlockDef *block);

/* Pre-fill `data` (sized to block->packedBytes) with each field's default
 * value. Fields with no known type-dispatch (rare, no current shaders use
 * such types) are left zero. */
void fillDefaults(const UniformBlockDef *block, void *data, size_t bytes);

} // namespace uniform_link_detail

} // namespace sculptcore::gpu
