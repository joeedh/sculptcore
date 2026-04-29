#include "manager.h"
#include "batch.h"
#include "command.h"
#include "litestl/binding/binding.h"
#include "pipeline.h"
#include "shader.h"
#include "vbo.h"

namespace sculptcore::gpu {
GPUManager::~GPUManager()
{
  using namespace litestl;

  // copy pointers, since resources will remove themselves from
  // the manager as they are destroyed.
  auto shadersCpy = shaders;
  auto buffersCpy = buffers;
  auto batchesCpy = batches;
  auto commandsCpy = commands;

  for (auto &shader : shadersCpy) {
    alloc::Delete(shader);
  }
  for (auto &buffer : buffersCpy) {
    alloc::Delete(buffer);
  }
  for (auto &batch : batchesCpy) {
    alloc::Delete(batch);
  }
  for (auto &command : commandsCpy) {
    alloc::Delete(command);
  }
}

Buffer *GPUManager::createBuffer(litestl::util::string name,
                                 GPUType type,
                                 int elemsize,
                                 int elemCount)
{
  /* alloc::New deduces lvalue-ref args by value and forwards as rvalue, which
   * doesn't bind to GPUManager&; do placement new manually. Buffer constructor
   * adds itself to manager.buffers. */
  void *mem = litestl::alloc::alloc("Buffer", sizeof(Buffer));
  return new (mem) Buffer(*this, name, type, elemsize, GPUFetchMode::FETCH_FLOAT, elemCount);
}

DrawBatch *GPUManager::createBatch()
{
  DrawBatch *b = litestl::alloc::New<DrawBatch>("DrawBatch");
  batches.append(b);
  return b;
}

DrawCommand *GPUManager::createCommand(DrawBatch *batch,
                                       GPUCmdType type,
                                       ShaderDef *shader,
                                       int start,
                                       int end,
                                       int primCount)
{
  DrawCommand *c = litestl::alloc::New<DrawCommand>("DrawCommand");
  c->type = type;
  c->shader = shader;
  c->start = start;
  c->end = end;
  c->primCount = primCount;
  commands.append(c);
  if (batch) {
    batch->commands.append(c);
  }
  return c;
}

litestl::binding::types::Struct<GPUManager> *GPUManager::defineBindings()
{
  using namespace litestl::binding;
  types::Struct<GPUManager> *st =
      new types::Struct<GPUManager>("sculptcore::gpu::GPUManager", sizeof(GPUManager));

  BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);

  BIND_STRUCT_MEMBER(st, buffers);
  BIND_STRUCT_MEMBER(st, batches);
  BIND_STRUCT_MEMBER(st, commands);

  BIND_STRUCT_METHOD(st, createBuffer);
  BIND_STRUCT_METHOD(st, createBatch);
  BIND_STRUCT_METHOD(st, createCommand);

  return st;
}
} // namespace sculptcore::gpu
