#pragma once
#include "command.h"
#include "litestl/binding/binding.h"
#include "shader.h"

namespace sculptcore::gpu {
struct GPUManager;

struct DrawBatch {
  litestl::util::Vector<DrawCommand *> commands;
  litestl::util::Vector<Buffer *> buffers;

  /* Per-batch uniform blocks (set=1 by the link pass). Owns the instances. */
  litestl::util::Vector<UniformBlockInstance *> blocks;

  /* Back-pointer set by GPUManager::createBatch — see DrawCommand. */
  GPUManager *manager = nullptr;

  DrawBatch() = default;
  DrawBatch(const DrawBatch &) = delete;
  DrawBatch(DrawBatch &&) = delete;
  DrawBatch &operator=(const DrawBatch &) = delete;
  DrawBatch &operator=(DrawBatch &&) = delete;
  ~DrawBatch();

  DrawBatch &clear()
  {
    commands.clear();
    buffers.clear();
    /* `blocks` is owned — don't clear without deleting; leave to destructor or
     * an explicit reset path. */
    return *this;
  }

  static litestl::binding::types::Struct<DrawBatch> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<DrawBatch> *st =
        new types::Struct<DrawBatch>("sculptcore::gpu::DrawBatch", sizeof(DrawBatch));

    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, commands);
    BIND_STRUCT_MEMBER(st, buffers);
    BIND_STRUCT_MEMBER(st, blocks);

    return st;
  }
};

} // namespace sculptcore::gpu
