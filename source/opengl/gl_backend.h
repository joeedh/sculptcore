#pragma once

#include "glew/GL/glew.h"

#include "gpu/manager.h"
#include "gpu/types.h"

#include "litestl/math/matrix.h"
#include "litestl/math/vector.h"
#include "litestl/util/map.h"

namespace sculptcore::gpu {
struct Buffer;
struct DrawBatch;
struct DrawCommand;
struct ShaderDef;
} // namespace sculptcore::gpu

namespace sculptcore::opengl {

struct DrawUniforms {
  litestl::math::mat4 drawMatrix;
  litestl::math::mat4 normalMatrix;
  litestl::math::float4 uColor{1.0f, 1.0f, 1.0f, 1.0f};
};

/** Walks `gpu::GPUManager` resources and issues GL calls.
 *  Caches per-Buffer VBOs and per-ShaderDef programs; uploads lazily on
 *  first use and re-uploads whenever a Buffer's `update_buffer` flag is set. */
struct GLBackend {
  GLBackend(sculptcore::gpu::GPUManager *mgr);
  GLBackend(const GLBackend &) = delete;
  ~GLBackend();

  /** Issue every command in `batch`, applying `u` as uniforms. */
  void draw(sculptcore::gpu::DrawBatch *batch, const DrawUniforms &u);

  /** Drop all cached GL objects (call before destroying the GL context). */
  void invalidate();

private:
  sculptcore::gpu::GPUManager *mgr_;
  litestl::util::Map<sculptcore::gpu::Buffer *, GLuint> vbo_cache_;
  litestl::util::Map<sculptcore::gpu::ShaderDef *, GLuint> prog_cache_;

  GLuint ensureBuffer(sculptcore::gpu::Buffer *buf);
  GLuint ensureProgram(sculptcore::gpu::ShaderDef *def);
  void issue(sculptcore::gpu::DrawCommand *cmd, const DrawUniforms &u);
};

} // namespace sculptcore::opengl
