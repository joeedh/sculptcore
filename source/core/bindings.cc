#include "mesh/bindings.h"
#include "wasm/wasmManager.h"
#include "gpu/bindings.h"

extern "C" void initBindings()
{
  auto &manager = sculptcore::wasm::manager;
  sculptcore::mesh::registerBindings(manager);
  sculptcore::gpu::registerBindings(manager);
}