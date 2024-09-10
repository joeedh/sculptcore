#include "shader.h"
#include "litestl/math/vector.h"
#include "litestl/util/map.h"

using litestl::util::Map;
using litestl::util::string;
using litestl::util::StringKey;
using litestl::util::stringref;

namespace sculptcore::gpu {
static Map<StringKey, Shader *> cached_shaders;

Shader *get_cached_shader(const Shader &shader)
{
  Shader **ptr = cached_shaders.lookup_ptr(shader.createShaderKey());
  return ptr ? *ptr : nullptr;
}

void set_cached_shader(Shader *shader)
{
  cached_shaders[shader->createShaderKey()] = shader;
}

ShaderDef shader = { //
    .vertexSource = R"(
precision highp float;

in vec3 alpha;
out vec3 a;
)",
    .fragmentSource = R"(
precision highp float;

in vec3 a;

)",
    .attrs = {"co", "no", "color"},
    .uniforms =
        {
            //
            Uniform("alpha", 1.0f),
            Uniform("matrix", math::mat4(0)),
            Uniform("normalMatrix", math::mat4(0)),
            Uniform("flatColor", math::float4(1.0f, 1.0f, 1.0f, 1.0f)),
        },
    .defines = {
        {"HAS_COLOR", "false"}, //
    }};
} // namespace sculptcore::gpu
