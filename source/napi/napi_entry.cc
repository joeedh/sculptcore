// Native Node/Electron N-API entry for sculptcore.
//
// documentation/plans/native-electron.md, Workstream A (build) + B (runtime).
// Built only when CMake is configured by cmake-js; see the root CMakeLists.txt
// addon block and `make.mjs build node`. Raw C N-API (node_api.h), not node-addon-api
// (spike A.5 / sculptcore/spike/napi/RESULTS.md).

#include <node_api.h>

#include "napi_runtime.h"

// Reflection entry points (extern "C"): initBindings() in source/core/
// bindings.cc registers every module's descriptors into the global manager;
// getBindingManager() in source/wasm/wasmManager.cc returns it.
extern "C" void initBindings();
extern "C" litestl::binding::BindingManager *getBindingManager();

// Defined in napi_runtime.cc (Workstream B). Installs listStructs/construct/
// destroy/getMember/setMember/liveCount onto exports.
void RegisterRuntime(napi_env env, napi_value exports);

namespace {

napi_value Init(napi_env env, napi_value exports)
{
  initBindings(); // populate the reflection registry (same call the WASM loader makes)

  // One runtime per module init; lives for the process. Wires version /
  // bindingCount / structNames / structInfo / construct onto exports.
  auto *rt = new sculptcore::napi::NapiRuntime(env, getBindingManager());
  rt->installExports(exports);
  return exports;
}

} // namespace

NAPI_MODULE(sculptcore_node, Init)
