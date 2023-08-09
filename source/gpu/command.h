#pragma once

namespace sculptcore::gpu {

enum GPUCmdType {
  DRAW_TRIS = 0;      //
  DRAW_TRI_STRIP = 1; //
  DRAW_LINES = 2;
  DRAW_POINTS = 3;
};

struct DrawCmd {
  GPUCmdType type;
};

}
