#pragma once
#include "batch.h"
#include "command.h"
#include "litestl/binding/binding.h"
#include "litestl/binding/manager.h"
#include "manager.h"
#include "vbo.h"

namespace sculptcore::gpu {
static void registerBindings(litestl::binding::BindingManager &manager)
{
  using namespace litestl::binding;
  manager.add(Bind<sculptcore::gpu::GPUType>());
  manager.add(Bind<sculptcore::gpu::GPUBufferType>());
  manager.add(Bind<sculptcore::gpu::GPUFetchMode>());
  manager.add(Bind<sculptcore::gpu::GPUCmdType>());
  manager.add(Bind<sculptcore::gpu::Buffer>());
  manager.add(Bind<sculptcore::gpu::DrawCommand>());
  manager.add(Bind<sculptcore::gpu::DrawBatch>());
  manager.add(Bind<sculptcore::gpu::GPUManager>());
}
} // namespace sculptcore::gpu
