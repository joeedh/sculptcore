#pragma once
#include "vbo.h"
#include "shader.h"
#include "litestl/util/vector.h"
namespace sculptcore::gpu {

enum class GPUCmdType {
  DRAW_TRIS = 0,      //
  DRAW_TRI_STRIP = 1, //
  DRAW_LINES = 2,
  DRAW_POINTS = 3
};

struct DrawCommand {
  GPUCmdType type;
  ShaderDef *shader;
  litestl::util::Vector<Buffer*> attrs;
  int start, end;
  int primCount;
};

} // namespace sculptcore::gpu
