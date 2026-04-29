#pragma once
#include "litestl/binding/binding.h"
#include "litestl/util/vector.h"
#include "shader.h"
#include "vbo.h"

namespace sculptcore::gpu {

enum class GPUCmdType {
  DRAW_TRIS = 0,      //
  DRAW_TRI_STRIP = 1, //
  DRAW_LINES = 2,
  DRAW_POINTS = 3
};

} // namespace sculptcore::gpu

namespace litestl::binding {
template <std::same_as<sculptcore::gpu::GPUCmdType> T> static const BindingBase *Bind()
{
  using namespace sculptcore::gpu;
  types::Enum *e = new types::Enum("sculptcore::gpu::GPUCmdType", sizeof(GPUCmdType));
  e->addItem("DRAW_TRIS", GPUCmdType::DRAW_TRIS);
  e->addItem("DRAW_TRI_STRIP", GPUCmdType::DRAW_TRI_STRIP);
  e->addItem("DRAW_LINES", GPUCmdType::DRAW_LINES);
  e->addItem("DRAW_POINTS", GPUCmdType::DRAW_POINTS);
  return e;
}
} // namespace litestl::binding

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

  static litestl::binding::types::Struct<DrawCommand> *defineBindings()
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
