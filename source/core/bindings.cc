#include "mesh/bindings.h"
#include "wasm/wasmManager.h"

extern "C" void initBindings()
{
  auto &manager = sculptcore::wasm::manager;
  sculptcore::mesh::registerBindings(manager);
}