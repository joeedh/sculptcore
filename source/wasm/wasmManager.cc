#include "wasmManager.h"

using namespace litestl::binding;
namespace sculptcore::wasm {
BindingManager manager;

extern "C" BindingManager *getBindingManager()
{
  return &manager;
}
} // namespace sculptcore::wasm
