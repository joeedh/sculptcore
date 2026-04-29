#pragma once
#include "command.h"
#include "shader.h"

namespace sculptcore::gpu {
struct DrawBatch {
  litestl::util::Vector<DrawCommand *> commands;
  litestl::util::Vector<Buffer *> buffers;
};

} // namespace sculptcore::gpu