#pragma once
#include "command.h"
#include "litestl/binding/binding.h"
#include "shader.h"

namespace sculptcore::gpu {
struct DrawBatch {
  litestl::util::Vector<DrawCommand *> commands;
  litestl::util::Vector<Buffer *> buffers;

  static litestl::binding::types::Struct<DrawBatch> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<DrawBatch> *st =
        new types::Struct<DrawBatch>("sculptcore::gpu::DrawBatch", sizeof(DrawBatch));

    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, commands);
    BIND_STRUCT_MEMBER(st, buffers);

    return st;
  }
};

FORWARD_CLS_BINDING(DrawBatch)

} // namespace sculptcore::gpu
