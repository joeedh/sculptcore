#pragma once
#include "command.h"
#include "batch.h"

namespace sculptcore::gpu {
struct DrawPipeline {
  litestl::util::Vector<DrawBatch *> batches;
};

} // namespace sculptcore::gpu