#pragma once

#include <concepts>
#include "types.h"
#include "litestl/binding/binding_base.h"
#include "litestl/binding/binding_struct.h"
#include "litestl/util/vector.h"
#include "shader.h"
#include "vbo.h"

namespace sculptcore::gpu {
struct DrawCommand {
  GPUCmdType type = GPUCmdType::DRAW_TRIS;
  ShaderDef *shader = nullptr;
  litestl::util::Vector<Buffer *> attrs;
  int start = 0, end = 0;
  int primCount = 0;

  DrawCommand()
  {
  }
  DrawCommand(DrawCommand &&) = default;
  DrawCommand &operator=(DrawCommand &&) = default;
  
  static const litestl::binding::types::Struct<DrawCommand> *defineBindings()
  {
    using namespace litestl::binding;

    types::Struct<DrawCommand> *st = new types::Struct<DrawCommand>(
        "sculptcore::gpu::DrawCommand", sizeof(DrawCommand));

    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, type);
    BIND_STRUCT_MEMBER(st, shader);
    BIND_STRUCT_MEMBER(st, attrs);
    BIND_STRUCT_MEMBER(st, start);
    BIND_STRUCT_MEMBER(st, end);
    BIND_STRUCT_MEMBER(st, primCount);

    return st;
  }
};

} // namespace sculptcore::gpu
