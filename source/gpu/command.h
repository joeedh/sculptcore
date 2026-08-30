#pragma once

#include "litestl/binding/binding_base.h"
#include "litestl/binding/binding_struct.h"
#include "litestl/util/vector.h"
#include "shader.h"
#include "types.h"
#include "vbo.h"
#include <concepts>

namespace sculptcore::gpu {
struct GPUManager;

struct DrawCommand {
  GPUCmdType type = GPUCmdType::DRAW_TRIS;
  ShaderDef *shader = nullptr;
  litestl::util::Vector<Buffer *> attrs;
  int start = 0, end = 0;
  int primCount = 0;

  /* Per-draw uniform blocks (set=2 by the link pass). Each instance owns its
   * UniformBlockDef and its data blob; the destructor deletes them. */
  litestl::util::Vector<UniformBlockInstance *> blocks;

  /* Back-pointer set by GPUManager::createCommand so the destructor can
   * self-remove from `manager->commands`. Mirrors Buffer's ownership
   * pattern — lets external producers `alloc::Delete` us without leaving
   * stale pointers in the manager. */
  GPUManager *manager = nullptr;

  DrawCommand()
  {
  }
  DrawCommand(DrawCommand &&) = delete;
  DrawCommand &operator=(DrawCommand &&) = delete;
  ~DrawCommand();

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
    BIND_STRUCT_MEMBER(st, blocks);

    return st;
  }
};

} // namespace sculptcore::gpu
