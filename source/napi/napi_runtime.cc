#include "napi_runtime.h"

#include <cstdio>

#include "napi_log.h"

// --- console.log sink for sc_napi_log (napi_log.h) --------------------------
// Installed at module init (NapiRuntime::installExports) via sc_napi_set_sink.
// N-API calls must happen on the JS thread; the brush/remesh/host paths run
// synchronously there. We fall back to stderr if env/console are unavailable or
// an exception is already in flight (which would swallow the console.log call).
namespace {
napi_env g_logEnv = nullptr;
napi_ref g_consoleRef = nullptr; // strong ref to the global `console`

void logToStderr(const char *msg)
{
  std::fputs(msg, stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

void consoleSink(const char *msg)
{
  napi_env env = g_logEnv;
  if (!env || !g_consoleRef) {
    logToStderr(msg);
    return;
  }
  napi_handle_scope scope;
  if (napi_open_handle_scope(env, &scope) != napi_ok) {
    logToStderr(msg);
    return;
  }
  bool pending = false;
  napi_is_exception_pending(env, &pending);
  if (pending) {
    logToStderr(msg);
    napi_close_handle_scope(env, scope);
    return;
  }
  napi_value console = nullptr, logFn = nullptr;
  napi_get_reference_value(env, g_consoleRef, &console);
  if (console && napi_get_named_property(env, console, "log", &logFn) == napi_ok) {
    napi_value arg = nullptr, ret = nullptr;
    napi_create_string_utf8(env, msg, NAPI_AUTO_LENGTH, &arg);
    napi_call_function(env, console, logFn, 1, &arg, &ret);
    // Don't let a logging failure poison the caller's env.
    bool threw = false;
    napi_is_exception_pending(env, &threw);
    if (threw) {
      napi_value err = nullptr;
      napi_get_and_clear_last_exception(env, &err);
    }
  } else {
    logToStderr(msg);
  }
  napi_close_handle_scope(env, scope);
}
} // namespace

namespace sculptcore::napi {

void NapiRuntime::define(napi_value exports, const char *name, napi_callback cb)
{
  napi_value fn;
  napi_create_function(env_, name, NAPI_AUTO_LENGTH, cb, this, &fn);
  napi_set_named_property(env_, exports, name, fn);
}

void NapiRuntime::installExports(napi_value exports)
{
  // Route sc_napi_log / sc_napi_logf (napi_log.h) to the renderer's DevTools
  // console — visible regardless of how Electron plumbs child-process stdout.
  g_logEnv = env_;
  {
    napi_value global = nullptr, console = nullptr;
    napi_get_global(env_, &global);
    if (napi_get_named_property(env_, global, "console", &console) == napi_ok) {
      napi_create_reference(env_, console, 1, &g_consoleRef);
    }
  }
  sc_napi_set_sink(&consoleSink);

  // IMPORTANT: expose these in makeNativeInterface in typescript/api/nativeManager.ts,
  // see that function's doc comment

  define(exports, "version", &NapiRuntime::Version);
  define(exports, "bindingCount", &NapiRuntime::BindingCount);
  define(exports, "structNames", &NapiRuntime::StructNames);
  define(exports, "structInfo", &NapiRuntime::StructInfo);
  define(exports, "construct", &NapiRuntime::Construct);
  define(exports, "constructWith", &NapiRuntime::ConstructWith);
  define(exports, "makeNodeVector", &NapiRuntime::MakeNodeVector);
  define(exports, "makeIntVector", &NapiRuntime::MakeIntVector);
  define(exports, "makeFloatVector", &NapiRuntime::MakeFloatVector);
  define(exports, "vectorLength", &NapiRuntime::VectorLength);
  define(exports, "vectorView", &NapiRuntime::VectorView);
  define(exports, "vectorGet", &NapiRuntime::VectorGet);
  define(exports, "intVectorAssign", &NapiRuntime::IntVectorAssign);
  define(exports, "floatVectorAssign", &NapiRuntime::FloatVectorAssign);
  define(exports, "pointerBytes", &NapiRuntime::PointerBytes);
  define(exports, "objectAddress", &NapiRuntime::ObjectAddress);
  define(exports, "meshCreateCube", &NapiRuntime::MeshCreateCube);
  define(exports, "meshMakeUVSphere", &NapiRuntime::MeshMakeUVSphere);
  define(exports, "meshMakeGrid", &NapiRuntime::MeshMakeGrid);
  define(exports, "meshBuildSpatialTree", &NapiRuntime::MeshBuildSpatialTree);
  define(exports, "spatialTreeFree", &NapiRuntime::SpatialTreeFree);
  define(exports, "meshFree", &NapiRuntime::MeshFree);
  define(exports, "meshTriangulate", &NapiRuntime::MeshTriangulate);
  define(exports, "meshQuadRemesh", &NapiRuntime::MeshQuadRemesh);
  define(exports, "meshSerialize", &NapiRuntime::MeshSerialize);
  define(exports, "meshSerializeRaw", &NapiRuntime::MeshSerializeRaw);
  define(exports, "meshDeserialize", &NapiRuntime::MeshDeserialize);
  define(exports, "vdmStoreNew", &NapiRuntime::VdmStoreNew);
  define(exports, "vdmStoreFree", &NapiRuntime::VdmStoreFree);
  define(exports, "meshVdmSplatDab", &NapiRuntime::MeshVdmSplatDab);
  define(exports, "meshVdmSplatDabLogged", &NapiRuntime::MeshVdmSplatDabLogged);
  define(exports, "meshVdmApplyToVerts", &NapiRuntime::MeshVdmApplyToVerts);
  define(exports, "vdmStoreSerialize", &NapiRuntime::VdmStoreSerialize);
  define(exports, "vdmStoreDeserialize", &NapiRuntime::VdmStoreDeserialize);
  define(exports, "vdmStoreRestoreBlob", &NapiRuntime::VdmStoreRestoreBlob);
  define(exports,
         "spatialTreeFillDetailCarrier",
         &NapiRuntime::SpatialTreeFillDetailCarrier);
  define(exports, "meshUpdateFrames", &NapiRuntime::MeshUpdateFrames);
  define(exports, "meshLayerSetWeight", &NapiRuntime::MeshLayerSetWeight);
  define(exports, "meshLayerSetEnabled", &NapiRuntime::MeshLayerSetEnabled);
  define(exports, "meshLayerSetFrozen", &NapiRuntime::MeshLayerSetFrozen);
  define(exports, "meshLayerRemove", &NapiRuntime::MeshLayerRemove);
  define(exports, "meshSetActiveEditLayer", &NapiRuntime::MeshSetActiveEditLayer);
  define(exports, "meshLayerFold", &NapiRuntime::MeshLayerFold);
  define(exports, "vdmLastSplatClamped", &NapiRuntime::VdmLastSplatClamped);
  define(exports, "multiresNew", &NapiRuntime::MultiresNew);
  define(exports, "multiresFree", &NapiRuntime::MultiresFree);
  define(exports, "multiresSetActiveLevel", &NapiRuntime::MultiresSetActiveLevel);
  define(exports, "multiresActiveMesh", &NapiRuntime::MultiresActiveMesh);
  define(exports, "multiresActiveTree", &NapiRuntime::MultiresActiveTree);
  define(exports, "multiresWriteback", &NapiRuntime::MultiresWriteback);
  define(exports, "multiresDownRefit", &NapiRuntime::MultiresDownRefit);
  define(exports, "multiresSerializeStore", &NapiRuntime::MultiresSerializeStore);
  define(exports, "multiresCaptureToVdm", &NapiRuntime::MultiresCaptureToVdm);
  define(exports, "multiresRestoreStore", &NapiRuntime::MultiresRestoreStore);
  define(exports,
         "spatialTreeSetRequestedAttrs",
         &NapiRuntime::SpatialTreeSetRequestedAttrs);
  define(exports, "spatialTreeSetDrawShader", &NapiRuntime::SpatialTreeSetDrawShader);
  define(exports,
         "spatialTreeGetMissingAttrSlots",
         &NapiRuntime::SpatialTreeGetMissingAttrSlots);
  define(exports,
         "spatialTreeRefreshRequestedAttrs",
         &NapiRuntime::SpatialTreeRefreshRequestedAttrs);
  define(exports, "gpuBrushBeginStroke", &NapiRuntime::GpuBrushBeginStroke);
  define(exports, "gpuBrushFree", &NapiRuntime::GpuBrushFree);
  define(exports, "gpuBrushKernelName", &NapiRuntime::GpuBrushKernelName);
  define(exports, "gpuBrushInfo", &NapiRuntime::GpuBrushInfo);
  define(exports, "gpuBrushMarshalDab", &NapiRuntime::GpuBrushMarshalDab);
  define(exports, "gpuBrushData", &NapiRuntime::GpuBrushData);
  define(exports, "gpuBrushApplyCo", &NapiRuntime::GpuBrushApplyCo);
  define(exports, "gpuBrushEndStroke", &NapiRuntime::GpuBrushEndStroke);
  define(exports, "getMemSize", &NapiRuntime::GetMemSize);
  define(exports, "printAllocBlocks", &NapiRuntime::PrintAllocBlocks);
  define(exports, "formatBlock", &NapiRuntime::FormatBlock);
  define(exports, "formatBlocks", &NapiRuntime::FormatBlocks);
  define(exports, "testPrint", &NapiRuntime::TestPrint);
  define(exports, "redirectStdout", &NapiRuntime::RedirectStdout);
  define(exports, "crashTest", &NapiRuntime::CrashTest);
}

} // namespace sculptcore::napi
