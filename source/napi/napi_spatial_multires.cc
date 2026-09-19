// Sculpt-layer settings mutators, the Multires engine seam, and the spatial
// tree's requested-attribute / draw-shader bridge (M5). Strings and JS
// arrays can't cross the generic method binding (marshalArg), so these route
// through dedicated extern "C" calls like the Mesh_* factories in
// napi_mesh_vdm.cc.

#include "napi_runtime.h"

#include <vector>

#include "napi_c_api.h"

#include "litestl/binding/binding_types.h"

namespace sculptcore::napi {

using binding::BindingType;

napi_value NapiRuntime::SpatialTreeFillDetailCarrier(napi_env env,
                                                     napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value undef;
  napi_get_undefined(env, &undef);
  Wrapped *tw = nullptr;
  if (argc >= 2 && napi_unwrap(env, argv[0], reinterpret_cast<void **>(&tw)) == napi_ok &&
      tw && tw->ptr)
  {
    int32_t carrier = 0;
    napi_get_value_int32(env, argv[1], &carrier);
    SpatialTree_fillDetailCarrier(tw->ptr, carrier);
  }
  return undef;
}

// meshUpdateFrames(mesh) -> void. Recompute vertex normals + the F3 frames —
// the splatter's frame prerequisite (call before meshVdmSplatDab).
napi_value NapiRuntime::MeshUpdateFrames(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value undef;
  napi_get_undefined(env, &undef);
  Wrapped *mw = nullptr;
  if (argc >= 1 && napi_unwrap(env, argv[0], reinterpret_cast<void **>(&mw)) == napi_ok &&
      mw && mw->ptr)
  {
    Mesh_updateFrames(mw->ptr);
  }
  return undef;
}

// Shared body of the sculpt-layer settings mutators: unwrap (mesh, li[, f]) and
// forward to the displace C-API, which keeps evaluated v.co current.
template <typename Fn>
static napi_value
meshLayerMutate(napi_env env, napi_callback_info info, bool hasValue, Fn fn)
{
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value undef;
  napi_get_undefined(env, &undef);
  Wrapped *mw = nullptr;
  size_t need = hasValue ? 3 : 2;
  if (argc >= need &&
      napi_unwrap(env, argv[0], reinterpret_cast<void **>(&mw)) == napi_ok && mw &&
      mw->ptr)
  {
    int32_t li = -1;
    napi_get_value_int32(env, argv[1], &li);
    double value = 0.0;
    if (hasValue) {
      napi_get_value_double(env, argv[2], &value);
    }
    fn(mw->ptr, li, value);
  }
  return undef;
}

// meshLayerSetWeight(mesh, li, weight) -> void.
napi_value NapiRuntime::MeshLayerSetWeight(napi_env env, napi_callback_info info)
{
  return meshLayerMutate(env, info, true, [](void *m, int li, double v) {
    Mesh_layerSetWeight(m, li, float(v));
  });
}

// meshLayerSetEnabled(mesh, li, enabled) -> void.
napi_value NapiRuntime::MeshLayerSetEnabled(napi_env env, napi_callback_info info)
{
  return meshLayerMutate(env, info, true, [](void *m, int li, double v) {
    Mesh_layerSetEnabled(m, li, v != 0.0 ? 1 : 0);
  });
}

// meshLayerSetFrozen(mesh, li, frozen) -> void.
napi_value NapiRuntime::MeshLayerSetFrozen(napi_env env, napi_callback_info info)
{
  return meshLayerMutate(env, info, true, [](void *m, int li, double v) {
    Mesh_layerSetFrozen(m, li, v != 0.0 ? 1 : 0);
  });
}

// meshLayerRemove(mesh, li) -> void. Subtracts the layer's contribution and
// drops its settings row + attribute column (destructive; caller snapshots).
napi_value NapiRuntime::MeshLayerRemove(napi_env env, napi_callback_info info)
{
  return meshLayerMutate(
      env, info, false, [](void *m, int li, double) { Mesh_layerRemove(m, li); });
}

// meshSetActiveEditLayer(mesh, li) -> int. Make layer li the V2 edit target
// (-1 clears); enables + pins weight 1. Returns the resulting target index.
napi_value NapiRuntime::MeshSetActiveEditLayer(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t result = -1;
  Wrapped *mw = nullptr;
  if (argc >= 2 && napi_unwrap(env, argv[0], reinterpret_cast<void **>(&mw)) == napi_ok &&
      mw && mw->ptr)
  {
    int32_t li = -1;
    napi_get_value_int32(env, argv[1], &li);
    result = Mesh_setActiveEditLayer(mw->ptr, li);
  }
  napi_value out;
  napi_create_int32(env, result, &out);
  return out;
}

// meshLayerFold(mesh) -> void. Fold the edit target's delta column from
// evaluated positions (idempotent; no-op without a target).
napi_value NapiRuntime::MeshLayerFold(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value undef;
  napi_get_undefined(env, &undef);
  Wrapped *mw = nullptr;
  if (argc >= 1 && napi_unwrap(env, argv[0], reinterpret_cast<void **>(&mw)) == napi_ok &&
      mw && mw->ptr)
  {
    Mesh_layerFold(mw->ptr);
  }
  return undef;
}

// multiresNew(cage, levels, leafLimit, depthLimit, gpuTriTarget) -> bound
// Multires wrapper (non-owning; free via multiresFree). The cage stays owned
// by the caller and must outlive the stack.
napi_value NapiRuntime::MultiresNew(napi_env env, napi_callback_info info)
{
  size_t argc = 5;
  napi_value argv[5];
  void *data;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);
  napi_value out;
  napi_get_undefined(env, &out);

  Wrapped *mw = nullptr;
  if (argc < 2 || napi_unwrap(env, argv[0], reinterpret_cast<void **>(&mw)) != napi_ok ||
      !mw || !mw->ptr)
  {
    return out;
  }
  int32_t iv[4] = {0, 0, 0, 0}; // levels, leafLimit, depthLimit, gpuTriTarget
  for (size_t i = 1; i < argc && i < 5; i++) {
    napi_get_value_int32(env, argv[i], &iv[i - 1]);
  }

  void *mr = Multires_new(mw->ptr, iv[0], iv[1], iv[2], iv[3]);
  const binding::BindingBase *st = rt->lookup("sculptcore::subdiv::Multires");
  if (!mr || !st || st->type != BindingType::Struct) {
    if (mr) {
      Multires_free(mr);
    }
    return out;
  }
  return rt->instantiate(
      static_cast<const types::_StructBase *>(st), mr, /*owning=*/false);
}

napi_value NapiRuntime::MultiresFree(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value undef;
  napi_get_undefined(env, &undef);
  Wrapped *w = nullptr;
  if (argc >= 1 && napi_unwrap(env, argv[0], reinterpret_cast<void **>(&w)) == napi_ok &&
      w && w->ptr)
  {
    Multires_free(w->ptr);
    // Null the wrapper so later access / the finalizer can't touch freed
    // storage (same contract as meshFree / vdmStoreFree).
    w->ptr = nullptr;
  }
  return undef;
}

// Shared body: unwrap (mr, level) and forward to an int-returning C-API call.
template <typename Fn>
static napi_value multiresLevelCall(napi_env env, napi_callback_info info, Fn fn)
{
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_create_int32(env, 0, &out);
  Wrapped *w = nullptr;
  if (argc >= 2 && napi_unwrap(env, argv[0], reinterpret_cast<void **>(&w)) == napi_ok &&
      w && w->ptr)
  {
    int32_t level = 0;
    napi_get_value_int32(env, argv[1], &level);
    napi_create_int32(env, fn(w->ptr, level), &out);
  }
  return out;
}

// multiresSetActiveLevel(mr, level) -> the active level after the switch.
// Slot mesh/tree pointers change — re-fetch via multiresActiveMesh/Tree.
napi_value NapiRuntime::MultiresSetActiveLevel(napi_env env, napi_callback_info info)
{
  return multiresLevelCall(
      env, info, [](void *mr, int level) { return Multires_setActiveLevel(mr, level); });
}

// multiresWriteback(mr, level) -> changed vert count (folds the level's
// resident edits into the grids store).
napi_value NapiRuntime::MultiresWriteback(napi_env env, napi_callback_info info)
{
  return multiresLevelCall(
      env, info, [](void *mr, int level) { return Multires_writeback(mr, level); });
}

// multiresDownRefit(mr, level) -> changed level-1 vert count.
napi_value NapiRuntime::MultiresDownRefit(napi_env env, napi_callback_info info)
{
  return multiresLevelCall(
      env, info, [](void *mr, int level) { return Multires_downRefit(mr, level); });
}

// Shared body of multiresActiveMesh/Tree: unwrap (mr), wrap the returned
// engine pointer as a NON-owning bound object of `structName`.
napi_value NapiRuntime::multiresActiveView(napi_env env,
                                           napi_callback_info info,
                                           const char *structName,
                                           void *(*fn)(void *))
{
  size_t argc = 1;
  napi_value argv[1];
  void *data;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);
  napi_value out;
  napi_get_undefined(env, &out);
  Wrapped *w = nullptr;
  if (argc < 1 || napi_unwrap(env, argv[0], reinterpret_cast<void **>(&w)) != napi_ok ||
      !w || !w->ptr)
  {
    return out;
  }
  void *p = fn(w->ptr);
  const binding::BindingBase *st = rt->lookup(structName);
  if (!p || !st || st->type != BindingType::Struct) {
    return out;
  }
  return rt->instantiate(
      static_cast<const types::_StructBase *>(st), p, /*owning=*/false);
}

napi_value NapiRuntime::MultiresActiveMesh(napi_env env, napi_callback_info info)
{
  return multiresActiveView(env, info, "sculptcore::mesh::Mesh", &Multires_activeMesh);
}

napi_value NapiRuntime::MultiresActiveTree(napi_env env, napi_callback_info info)
{
  return multiresActiveView(
      env, info, "sculptcore::spatial::SpatialTree", &Multires_activeTree);
}

// multiresSerializeStore(mr) -> Uint8Array (grids-store blob; copy semantics
// as MeshSerialize — the V8 sandbox forbids external buffers).
napi_value NapiRuntime::MultiresSerializeStore(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);
  Wrapped *w = nullptr;
  if (argc < 1 || napi_unwrap(env, argv[0], reinterpret_cast<void **>(&w)) != napi_ok ||
      !w || !w->ptr)
  {
    return out;
  }
  int size = 0;
  uint8_t *buf = Multires_serializeStore(w->ptr, &size);
  if (!buf) {
    return out;
  }
  if (size <= 0) {
    freeMeshBuffer(buf);
    return out;
  }
  napi_value ab;
  void *abData = nullptr;
  napi_create_arraybuffer(env, static_cast<size_t>(size), &abData, &ab);
  if (abData)
    std::memcpy(abData, buf, static_cast<size_t>(size));
  freeMeshBuffer(buf);
  napi_create_typedarray(env, napi_uint8_array, static_cast<size_t>(size), ab, 0, &out);
  return out;
}

// multiresCaptureToVdm(mr, store, level) -> texels written. X4 geometry ->
// VDM capture; caller owns undo snapshots + spatial refresh.
napi_value NapiRuntime::MultiresCaptureToVdm(napi_env env, napi_callback_info info)
{
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_create_int32(env, 0, &out);
  Wrapped *mw = nullptr, *sw = nullptr;
  if (argc < 3 || napi_unwrap(env, argv[0], reinterpret_cast<void **>(&mw)) != napi_ok ||
      napi_unwrap(env, argv[1], reinterpret_cast<void **>(&sw)) != napi_ok || !mw ||
      !sw || !mw->ptr || !sw->ptr)
  {
    return out;
  }
  int32_t level = 0;
  napi_get_value_int32(env, argv[2], &level);
  int n = Multires_captureToVdm(mw->ptr, sw->ptr, level);
  napi_create_int32(env, n, &out);
  return out;
}

// multiresRestoreStore(mr, bytes) -> boolean. Replaces the grids store and
// invalidates all levels; the caller re-sets the active level afterwards.
napi_value NapiRuntime::MultiresRestoreStore(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_boolean(env, false, &out);
  Wrapped *w = nullptr;
  if (argc < 2 || napi_unwrap(env, argv[0], reinterpret_cast<void **>(&w)) != napi_ok ||
      !w || !w->ptr)
  {
    return out;
  }
  void *bytes = nullptr;
  size_t byteLen = 0;
  bool isTa = false;
  napi_is_typedarray(env, argv[1], &isTa);
  if (isTa) {
    napi_typedarray_type t;
    napi_value ab;
    size_t off = 0;
    napi_get_typedarray_info(env, argv[1], &t, &byteLen, &bytes, &ab, &off);
  } else {
    bool isAb = false;
    napi_is_arraybuffer(env, argv[1], &isAb);
    if (isAb) {
      napi_get_arraybuffer_info(env, argv[1], &bytes, &byteLen);
    }
  }
  if (!bytes || byteLen == 0) {
    return out;
  }
  int ok = Multires_restoreStore(
      w->ptr, static_cast<const uint8_t *>(bytes), static_cast<int>(byteLen));
  napi_get_boolean(env, ok != 0, &out);
  return out;
}

// spatialTreeSetRequestedAttrs(tree, count, namesJoined, srcTypes, elemSizes,
// slots, domains, defaultKinds) -> void. Routes the requested-attr set to the
// extern "C" bridge (setTreeRequestedAttrs). Strings/JS arrays can't cross the
// generic method binding (marshalArg has no string/typed-array case), so this
// reads them directly: the names are one '\n'-joined string, each int field an
// Int32Array whose backing pointer we hand straight to C (read-only there).
napi_value NapiRuntime::SpatialTreeSetRequestedAttrs(napi_env env,
                                                     napi_callback_info info)
{
  size_t argc = 8;
  napi_value argv[8];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value undef;
  napi_get_undefined(env, &undef);
  if (argc < 8)
    return undef;

  Wrapped *tw = nullptr;
  if (napi_unwrap(env, argv[0], reinterpret_cast<void **>(&tw)) != napi_ok || !tw ||
      !tw->ptr)
  {
    return undef;
  }

  int32_t count = 0;
  napi_get_value_int32(env, argv[1], &count);
  if (count < 0)
    count = 0;

  size_t nameLen = 0;
  napi_get_value_string_utf8(env, argv[2], nullptr, 0, &nameLen);
  std::vector<char> nameBuf(nameLen + 1, 0);
  napi_get_value_string_utf8(env, argv[2], nameBuf.data(), nameLen + 1, &nameLen);

  // Int32Array backing pointer (already byte-offset applied), or null if the arg
  // isn't a typed array — the C side then treats that field as defaulted.
  auto getInts = [&](napi_value v) -> const int * {
    bool isTa = false;
    napi_is_typedarray(env, v, &isTa);
    if (!isTa)
      return nullptr;
    napi_typedarray_type t;
    size_t len = 0;
    void *data = nullptr;
    napi_value ab;
    size_t off = 0;
    napi_get_typedarray_info(env, v, &t, &len, &data, &ab, &off);
    return static_cast<const int *>(data);
  };

  setTreeRequestedAttrs(tw->ptr,
                        count,
                        nameBuf.data(),
                        getInts(argv[3]),
                        getInts(argv[4]),
                        getInts(argv[5]),
                        getInts(argv[6]),
                        getInts(argv[7]));
  return undef;
}

// spatialTreeSetDrawShader(tree, wgsl) -> void. Copies the (possibly large) WGSL
// string inbound and hands it to the extern "C" bridge.
napi_value NapiRuntime::SpatialTreeSetDrawShader(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value undef;
  napi_get_undefined(env, &undef);
  if (argc < 2)
    return undef;

  Wrapped *tw = nullptr;
  if (napi_unwrap(env, argv[0], reinterpret_cast<void **>(&tw)) != napi_ok || !tw ||
      !tw->ptr)
  {
    return undef;
  }
  size_t len = 0;
  napi_get_value_string_utf8(env, argv[1], nullptr, 0, &len);
  std::vector<char> buf(len + 1, 0);
  napi_get_value_string_utf8(env, argv[1], buf.data(), len + 1, &len);
  setTreeDrawShader(tw->ptr, buf.data());
  return undef;
}

// spatialTreeGetMissingAttrSlots(tree) -> number[]. Two-call pattern: query the
// count, then copy into a temp and build a JS array (advisory; small).
napi_value NapiRuntime::SpatialTreeGetMissingAttrSlots(napi_env env,
                                                       napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_create_array(env, &out);
  if (argc < 1)
    return out;

  Wrapped *tw = nullptr;
  if (napi_unwrap(env, argv[0], reinterpret_cast<void **>(&tw)) != napi_ok || !tw ||
      !tw->ptr)
  {
    return out;
  }
  int n = getTreeMissingAttrSlots(tw->ptr, nullptr, 0);
  if (n <= 0)
    return out;
  std::vector<int> tmp(static_cast<size_t>(n), 0);
  getTreeMissingAttrSlots(tw->ptr, tmp.data(), n);
  for (int i = 0; i < n; i++) {
    napi_value e;
    napi_create_int32(env, tmp[i], &e);
    napi_set_element(env, out, static_cast<uint32_t>(i), e);
  }
  return out;
}

// spatialTreeRefreshRequestedAttrs(tree) -> void. Forces a per-attribute buffer
// rebuild against the current mesh layers when only the layer set changed (the
// requested descriptors are byte-identical, so setRequestedAttrs would no-op).
napi_value NapiRuntime::SpatialTreeRefreshRequestedAttrs(napi_env env,
                                                         napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value undef;
  napi_get_undefined(env, &undef);
  if (argc < 1)
    return undef;

  Wrapped *tw = nullptr;
  if (napi_unwrap(env, argv[0], reinterpret_cast<void **>(&tw)) != napi_ok || !tw ||
      !tw->ptr)
  {
    return undef;
  }
  refreshTreeRequestedAttrs(tw->ptr);
  return undef;
}

} // namespace sculptcore::napi
