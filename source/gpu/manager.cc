#include "command.h"

#include "batch.h"
#include "litestl/binding/binding.h"
#include "litestl/util/set.h"
#include "manager.h"
#include "pipeline.h"
#include "shader.h"
#include "vbo.h"

namespace litestl::binding {
template <std::same_as<sculptcore::gpu::GPUCmdType> T> const BindingBase *Bind()
{
  using namespace sculptcore::gpu;
  using namespace litestl::binding;

  types::Enum *e = new types::Enum("sculptcore::gpu::GPUCmdType", sizeof(GPUCmdType));
  e->addItem("DRAW_TRIS", GPUCmdType::DRAW_TRIS);
  e->addItem("DRAW_TRI_STRIP", GPUCmdType::DRAW_TRI_STRIP);
  e->addItem("DRAW_LINES", GPUCmdType::DRAW_LINES);
  e->addItem("DRAW_POINTS", GPUCmdType::DRAW_POINTS);
  return e;
}
template const BindingBase *Bind<sculptcore::gpu::GPUCmdType>();

template <std::same_as<sculptcore::gpu::GPUType> T> const BindingBase *Bind()
{
  using namespace sculptcore::gpu;
  types::Enum *e = new types::Enum("sculptcore::gpu::GPUType", sizeof(GPUType));

  e->addItem("TYPE_INVALID", GPUType::TYPE_INVALID);
  e->addItem("FLOAT16", GPUType::FLOAT16);
  e->addItem("FLOAT32", GPUType::FLOAT32);
  e->addItem("FLOAT64", GPUType::FLOAT64);
  e->addItem("INT32", GPUType::INT32);
  e->addItem("INT16", GPUType::INT16);
  e->addItem("INT8", GPUType::INT8);
  e->addItem("UINT32", GPUType::UINT32);
  e->addItem("UINT16", GPUType::UINT16);
  e->addItem("UINT8", GPUType::UINT8);
  return e;
}
template const BindingBase *Bind<sculptcore::gpu::GPUType>();

template <std::same_as<sculptcore::gpu::GPUFetchMode> T> const BindingBase *Bind()
{
  using namespace sculptcore::gpu;
  types::Enum *e = new types::Enum("sculptcore::gpu::GPUFetchMode", sizeof(GPUFetchMode));

  e->addItem("FETCH_NONE", GPUFetchMode::FETCH_NONE);
  e->addItem("FETCH_FLOAT", GPUFetchMode::FETCH_FLOAT);
  return e;
}
template const BindingBase *Bind<sculptcore::gpu::GPUFetchMode>();

} // namespace litestl::binding

namespace sculptcore::gpu {

DrawCommand::~DrawCommand()
{
  for (auto *inst : blocks) {
    litestl::alloc::Delete(inst);
  }
  if (manager) {
    manager->commands.remove(this);
  }
  manager = nullptr;
}

DrawBatch::~DrawBatch()
{
  for (auto *inst : blocks) {
    litestl::alloc::Delete(inst);
  }
  if (manager) {
    manager->batches.remove(this);
  }
}

GPUManager::~GPUManager()
{
  using namespace litestl;

  /* Resources are owned by their producers (spatial tree, brush, etc.) which
   * free them via alloc::Delete directly. Buffer / DrawBatch / DrawCommand
   * self-remove from the lists below in their destructors, so by the time
   * we get here these vectors are usually empty. Anything still present is
   * a stragger we own — Delete it. Shader pointers stored in `shaders` are
   * owned externally (e.g. spatial::spatialShaders globals) and must NOT be
   * freed here. */
  auto buffersCpy = buffers;
  auto batchesCpy = batches;
  auto commandsCpy = commands;

  for (auto *buffer : buffersCpy) {
    alloc::Delete(buffer);
  }
  for (auto *batch : batchesCpy) {
    alloc::Delete(batch);
  }
  for (auto *command : commandsCpy) {
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
  return new (mem)
      Buffer(*this, name, type, elemsize, GPUFetchMode::FETCH_FLOAT, elemCount);
}

DrawBatch *GPUManager::createBatch()
{
  DrawBatch *b = litestl::alloc::New<DrawBatch>("DrawBatch");
  b->manager = this;
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
  c->manager = this;
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

void GPUManager::destroyBuffer(Buffer *buffer)
{
  /* Notify backends BEFORE freeing so they can drop cache entries keyed by
   * `buffer` and defer native-handle destruction to a safe point. */
  for (auto *obs : observers) {
    obs->onBufferDestroyed(buffer);
  }
  buffers.remove(buffer);
  alloc::Delete(buffer);
}

void GPUManager::destroyCommand(DrawCommand *cmd, bool destroy_buffers)
{
  if (destroy_buffers) {
    for (Buffer *buf : cmd->attrs) {
      destroyBuffer(buf);
    }
  }
  
  // note: cmd's destructor removes itself from commands
  alloc::Delete(cmd);
}

void GPUManager::destroyBatch(DrawBatch *batch,
                              bool destroy_commands,
                              bool destroy_buffers)
{
  if (destroy_buffers) {
    util::Set<Buffer *, 32> buffers;
    for (DrawCommand *cmd : batch->commands) {
      for (Buffer *buf : cmd->attrs) {
        buffers.add(buf);
      }
    }
    for (Buffer *buf : batch->buffers) {
      buffers.add(buf);
    }

    for (Buffer *buf : buffers) {
      destroyBuffer(buf);
    }
  }

  if (destroy_commands) {
    util::Set<DrawCommand *, 32> commands;
    // de-duplicate
    for (auto *cmd : batch->commands) {
      commands.add(cmd);
    }
    for (auto *cmd : commands) {
      destroyCommand(cmd, false);
    }
  }

  // note: batch's destructor removes itself from batches
  alloc::Delete(batch);
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

  BIND_STRUCT_METHOD(st, createBuffer, MARGS("name", "type", "elemsize", "elemCount"))
      .isNeverNull();

  BIND_STRUCT_METHOD(st, createBatch, MARGS()).isNeverNull();
  BIND_STRUCT_METHOD(
      st, createCommand, MARGS("batch", "type", "shader", "start", "end", "primCount"))
      .argIsNullable("shader")
      .isNeverNull();
  BIND_STRUCT_METHOD(
      st, destroyBatch, MARGS("batch", "destroy_commands", "destroy_buffers"));
  BIND_STRUCT_METHOD(st, destroyBuffer, MARGS("buffer"));
  BIND_STRUCT_METHOD(st, destroyCommand, MARGS("command", "destroy_buffers"));

  return st;
}
} // namespace sculptcore::gpu
