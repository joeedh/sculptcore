#include "shader.h"
#include "math/vector.h"

namespace sculptcore::gpu {
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
}
