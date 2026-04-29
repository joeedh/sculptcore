#include "manager.h"
#include "batch.h"
#include "command.h"
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
} // namespace sculptcore::gpu