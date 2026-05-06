#include "mesh/bindings.h"
#include "wasm/wasmManager.h"
#include "gpu/bindings.h"
#include "spatial/bindings.h"
#include "props/bindings.h"
#include "brush/bindings.h"

extern "C" void initBindings()
{
  auto &manager = sculptcore::wasm::manager;
  sculptcore::mesh::registerBindings(manager);
  sculptcore::gpu::registerBindings(manager);
  sculptcore::spatial::registerBindings(manager);
  sculptcore::props::registerBindings(manager);
  sculptcore::brush::bindings::registerBindings(manager);
}