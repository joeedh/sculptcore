#pragma once
#include "vbo.h"
namespace sculptcore::gpu {

enum class GPUCmdType {
  DRAW_TRIS = 0,      //
  DRAW_TRI_STRIP = 1, //
  DRAW_LINES = 2,
  DRAW_POINTS = 3
};

struct DrawCmd {
  GPUCmdType type;
  

};

} // namespace sculptcore::gpu
