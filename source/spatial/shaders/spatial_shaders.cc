#include "spatial_shaders.h"
#include "gpu/shader.h"
#include "litestl/binding/binding.h"

namespace sculptcore::spatial {

litestl::binding::types::Struct<SpatialShaders> *SpatialShaders::defineBindings()
{
  using namespace litestl::binding;

  types::Struct<SpatialShaders> *st = new types::Struct<SpatialShaders>(
      "sculptcore::spatial::SpatialShaders", sizeof(SpatialShaders));

  BIND_STRUCT_MEMBER(st, basicMeshShader);
  BIND_STRUCT_MEMBER(st, basicLineShader);

  return st;
}

gpu::ShaderDef basicLineShader = {
    "Basic Line Shader",
    R"glsl(
precision mediump float;
uniform mat4 drawMatrix;

attribute vec3 position;
attribute vec4 color;

varying vec4 vColor;

void main() {
  vec4 p = drawMatrix * vec4(position, 1.0);
  gl_Position = p;
  vColor = color;
}
    )glsl",
    R"glsl(
precision mediump float;
uniform vec4 uColor;
varying vec4 vColor;

void main() {
  gl_FragColor = uColor * vColor;
}
    )glsl",
    {
        //
        {"position", GPUType::FLOAT32, 3},
        {"color", GPUType::FLOAT32, 4} //
    },
    {
        new UniformDef<litestl::math::float4>(
            "uColor", //
            GPUType::FLOAT32,
            3,
            litestl::math::float4(1.0f, 1.0f, 1.0f, 1.0f)),
        new UniformDef<litestl::math::mat4>( //
            "drawMatrix",                    //
            GPUType::FLOAT32,
            16,
            litestl::math::mat4().identity()),
    },
    {}
    //
};

SpatialShaders spatialShaders = {basicLineShader, basicLineShader};
} // namespace sculptcore::spatial
