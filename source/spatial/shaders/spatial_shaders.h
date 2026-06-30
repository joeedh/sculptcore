#include "gpu/shader.h"

namespace sculptcore::spatial {
using namespace gpu;
struct SpatialShaders {
  ShaderDef basicMeshShader;
  ShaderDef basicLineShader;
  ShaderDef basicPointShader;

  static litestl::binding::types::Struct<SpatialShaders> *defineBindings();
};

extern SpatialShaders spatialShaders;
} // namespace sculptcore::spatial
