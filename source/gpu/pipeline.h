#pragma once
#include "batch.h"
#include "command.h"
#include "shader.h"

namespace sculptcore::gpu {
struct DrawPipeline {
  litestl::util::Vector<DrawBatch *> batches;

  /* Per-pipeline / per-pass uniform blocks (set=0 by the link pass). Owns the
   * instances; destructor deletes them. */
  litestl::util::Vector<UniformBlockInstance *> blocks;

  DrawPipeline() = default;
  DrawPipeline(const DrawPipeline &) = delete;
  DrawPipeline &operator=(const DrawPipeline &) = delete;
  ~DrawPipeline()
  {
    for (auto *inst : blocks) {
      litestl::alloc::Delete(inst);
    }
  }

  static litestl::binding::types::Struct<DrawPipeline> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<DrawPipeline> *st = new types::Struct<DrawPipeline>(
        "sculptcore::gpu::DrawPipeline", sizeof(DrawPipeline));

    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, batches);
    BIND_STRUCT_MEMBER(st, blocks);
    return st;
  }
};

} // namespace sculptcore::gpu