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

gpu::ShaderDef basicMeshShader = {
    "Basic Mesh Shader",
    R"glsl(
precision mediump float;
  
uniform mat4 drawMatrix;
uniform mat4 normalMatrix;

attribute vec3 position;
attribute vec3 normal;

varying vec3 vNormal;

void main() {
  vec4 p = drawMatrix * vec4(position, 1.0);
  gl_Position = p;
  vNormal = normalize( (normalMatrix * vec4(normal, 0.0)).xyz );
}
    )glsl",
    R"glsl(
precision mediump float;
varying vec3 vNormal;

void main() {
  vec3 no = vNormal; //normalize(vNormal);
  
  float f1 = dot(no, normalize(vec3(0.1, 0.2, -1.0)));
  float f = f1 < 0.0 ? -f1*0.2 : f1;
  
  f = f*0.8 + 0.2;

  vec4 vcolor = vec4(1.0, 1.0, 1.0, 1.0);
  if (f1 < 0.0) {
    vcolor[2] *= 0.8;
    vcolor[0] *= 0.5;
  }
  gl_FragColor = vec4(vcolor.rgb * f, vcolor.a);
}
    )glsl",
    {
        //
        {"position", GPUType::FLOAT32, 3},
        {"normal", GPUType::FLOAT32, 4} //
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
SpatialShaders spatialShaders = {basicMeshShader, basicLineShader};
} // namespace sculptcore::spatial
