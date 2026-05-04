#pragma once
#include "types.h"

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

  manager.add(Bind((GPUType *)nullptr));
  manager.add(Bind((GPUBufferType *)nullptr));
  manager.add(Bind((GPUFetchMode *)nullptr));
  manager.add(Bind((GPUCmdType *)nullptr));
  manager.add(Bind((Buffer *)nullptr));
  manager.add(Bind((DrawCommand *)nullptr));
  manager.add(Bind((DrawBatch *)nullptr));
  manager.add(Bind((GPUManager *)nullptr));
}
} // namespace sculptcore::gpu
