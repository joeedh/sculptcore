// Native engine factory free-functions: create/operate on real engine
// objects and hand JS bound wrappers. Mesh primitives, spatial-tree
// construction/serialization, and the VDM store seam.

#include "napi_runtime.h"

#include <cstdint>
#include <cstring>

#include "napi_c_api.h"

#include "litestl/binding/binding_types.h"

namespace sculptcore::napi {

using binding::BindingType;

napi_value NapiRuntime::MeshCreateCube(napi_env env, napi_callback_info info)
{
  size_t argc = 3;
  napi_value argv[3];
  void *data;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);

  int32_t dimen = 0;
  double size = 0.5, sphere = 0.0;
  if (argc >= 1)
    napi_get_value_int32(env, argv[0], &dimen);
  if (argc >= 2)
    napi_get_value_double(env, argv[1], &size);
  if (argc >= 3)
    napi_get_value_double(env, argv[2], &sphere);

  void *m = Mesh_createCube(dimen, static_cast<float>(size), static_cast<float>(sphere));
  const binding::BindingBase *st = rt->lookup("sculptcore::mesh::Mesh");
  napi_value out;
  if (!m || !st || st->type != BindingType::Struct) {
    napi_get_undefined(env, &out);
    return out;
  }
  return rt->instantiate(
      static_cast<const types::_StructBase *>(st), m, /*owning=*/false);
}

// meshMakeUVSphere(rings, segs, radius) -> Mesh. A clean all-quad UV sphere
// (poles are the only singularities) — the remesh-friendly primitive the host's
// quad-remesh parity test drives, paralleling MeshCreateCube's wrapping.
napi_value NapiRuntime::MeshMakeUVSphere(napi_env env, napi_callback_info info)
{
  size_t argc = 3;
  napi_value argv[3];
  void *data;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);

  int32_t rings = 0, segs = 0;
  double radius = 1.0;
  if (argc >= 1)
    napi_get_value_int32(env, argv[0], &rings);
  if (argc >= 2)
    napi_get_value_int32(env, argv[1], &segs);
  if (argc >= 3)
    napi_get_value_double(env, argv[2], &radius);

  void *m = Mesh_makeUVSphere(rings, segs, static_cast<float>(radius));
  const binding::BindingBase *st = rt->lookup("sculptcore::mesh::Mesh");
  napi_value out;
  if (!m || !st || st->type != BindingType::Struct) {
    napi_get_undefined(env, &out);
    return out;
  }
  return rt->instantiate(
      static_cast<const types::_StructBase *>(st), m, /*owning=*/false);
}

// meshMakeGrid(nx, ny, size) -> Mesh. Flat XY quad grid facing +Z (the
// add-plane primitive), paralleling MeshCreateCube's wrapping.
napi_value NapiRuntime::MeshMakeGrid(napi_env env, napi_callback_info info)
{
  size_t argc = 3;
  napi_value argv[3];
  void *data;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);

  int32_t nx = 0, ny = 0;
  double size = 1.0;
  if (argc >= 1)
    napi_get_value_int32(env, argv[0], &nx);
  if (argc >= 2)
    napi_get_value_int32(env, argv[1], &ny);
  if (argc >= 3)
    napi_get_value_double(env, argv[2], &size);

  void *m = Mesh_makeGrid(nx, ny, static_cast<float>(size));
  const binding::BindingBase *st = rt->lookup("sculptcore::mesh::Mesh");
  napi_value out;
  if (!m || !st || st->type != BindingType::Struct) {
    napi_get_undefined(env, &out);
    return out;
  }
  return rt->instantiate(
      static_cast<const types::_StructBase *>(st), m, /*owning=*/false);
}

napi_value NapiRuntime::MeshBuildSpatialTree(napi_env env, napi_callback_info info)
{
  size_t argc = 4;
  napi_value argv[4];
  void *data;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);

  napi_value out;
  Wrapped *mw = nullptr;
  if (argc < 1 || napi_unwrap(env, argv[0], reinterpret_cast<void **>(&mw)) != napi_ok ||
      !mw)
  {
    napi_get_undefined(env, &out);
    return out;
  }
  int32_t leaf = 0, depth = 0, tri_target = 0;
  if (argc >= 2)
    napi_get_value_int32(env, argv[1], &leaf);
  if (argc >= 3)
    napi_get_value_int32(env, argv[2], &depth);
  if (argc >= 4)
    napi_get_value_int32(env, argv[3], &tri_target);

  void *t = Mesh_buildSpatialTree(mw->ptr, leaf, depth, tri_target);
  const binding::BindingBase *st = rt->lookup("sculptcore::spatial::SpatialTree");
  if (!t || !st || st->type != BindingType::Struct) {
    napi_get_undefined(env, &out);
    return out;
  }
  return rt->instantiate(
      static_cast<const types::_StructBase *>(st), t, /*owning=*/false);
}

napi_value NapiRuntime::SpatialTreeFree(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value undef;
  napi_get_undefined(env, &undef);
  Wrapped *tw = nullptr;
  if (argc >= 1 && napi_unwrap(env, argv[0], reinterpret_cast<void **>(&tw)) == napi_ok &&
      tw && tw->ptr)
  {
    SpatialTree_free(tw->ptr);
  }
  return undef;
}

napi_value NapiRuntime::MeshFree(napi_env env, napi_callback_info info)
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
    Mesh_free(mw->ptr);
    // The wrapper no longer owns valid memory; null it so a later member access
    // or finalizer can't dereference freed storage.
    mw->ptr = nullptr;
  }
  return undef;
}

// meshTriangulate(mesh) -> void. Fan-triangulates every n-gon in place (the
// triangulate button / dyntopo-on-quads prep). The caller rebuilds the spatial
// tree afterwards.
napi_value NapiRuntime::MeshTriangulate(napi_env env, napi_callback_info info)
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
    Mesh_triangulate(mw->ptr);
  }
  return undef;
}

// meshQuadRemesh(mesh, params) -> Mesh. params is a bound RemeshParams struct
// wrapper. Returns a new non-owning Mesh wrapper (freed via meshFree, like
// meshCreateCube); the input mesh is left untouched (host snapshots it for undo).
napi_value NapiRuntime::MeshQuadRemesh(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  void *data;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);

  napi_value out;
  Wrapped *mw = nullptr, *pw = nullptr;
  if (argc < 2 || napi_unwrap(env, argv[0], reinterpret_cast<void **>(&mw)) != napi_ok ||
      napi_unwrap(env, argv[1], reinterpret_cast<void **>(&pw)) != napi_ok || !mw ||
      !pw || !mw->ptr || !pw->ptr)
  {
    napi_get_undefined(env, &out);
    return out;
  }

  void *m = Mesh_quadRemesh(mw->ptr, pw->ptr);
  const binding::BindingBase *st = rt->lookup("sculptcore::mesh::Mesh");
  if (!m || !st || st->type != BindingType::Struct) {
    napi_get_undefined(env, &out);
    return out;
  }
  return rt->instantiate(
      static_cast<const types::_StructBase *>(st), m, /*owning=*/false);
}

// meshSerialize(mesh) -> Uint8Array of the versioned, lz4hc-compressed blob.
// Always copies into a sandbox-internal ArrayBuffer (no zero-copy external view
// like PointerBytes/VectorView attempt): V8 forbids external buffers in Electron,
// and the freshly-malloc'd C++ buffer is transient — freed here on the next line.
napi_value NapiRuntime::MeshSerialize(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);

  Wrapped *mw = nullptr;
  if (argc < 1 || napi_unwrap(env, argv[0], reinterpret_cast<void **>(&mw)) != napi_ok ||
      !mw || !mw->ptr)
  {
    return out;
  }

  int size = 0;
  uint8_t *buf = serializeMesh(mw->ptr, &size);
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

// meshSerializeRaw(mesh) -> Uint8Array of the uncompressed column payload only
// (autosave worker frames + lz4-compresses it off-thread). Same copy semantics
// as MeshSerialize above.
napi_value NapiRuntime::MeshSerializeRaw(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);

  Wrapped *mw = nullptr;
  if (argc < 1 || napi_unwrap(env, argv[0], reinterpret_cast<void **>(&mw)) != napi_ok ||
      !mw || !mw->ptr)
  {
    return out;
  }

  int size = 0;
  uint8_t *buf = serializeMeshRaw(mw->ptr, &size);
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

// meshDeserialize(bytes) -> a fresh, non-owning Mesh wrapper. Accepts a
// Uint8Array (what the TS Mesh_deserialize helper passes) or an ArrayBuffer.
napi_value NapiRuntime::MeshDeserialize(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  void *data;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);
  napi_value out;
  napi_get_undefined(env, &out);
  if (argc < 1)
    return out;

  void *bytes = nullptr;
  size_t byteLen = 0;
  bool isTa = false;
  napi_is_typedarray(env, argv[0], &isTa);
  if (isTa) {
    // get_typedarray_info returns `length` in elements and a `data` pointer that
    // already has byte_offset applied; for a Uint8Array element size is 1.
    napi_typedarray_type t;
    napi_value ab;
    size_t off = 0;
    napi_get_typedarray_info(env, argv[0], &t, &byteLen, &bytes, &ab, &off);
  } else {
    bool isAb = false;
    napi_is_arraybuffer(env, argv[0], &isAb);
    if (isAb) {
      napi_get_arraybuffer_info(env, argv[0], &bytes, &byteLen);
    }
  }
  if (!bytes || byteLen == 0)
    return out;

  void *m =
      deserializeMesh(static_cast<const uint8_t *>(bytes), static_cast<int>(byteLen));
  const binding::BindingBase *st = rt->lookup("sculptcore::mesh::Mesh");
  if (!m || !st || st->type != BindingType::Struct) {
    return out;
  }
  return rt->instantiate(
      static_cast<const types::_StructBase *>(st), m, /*owning=*/false);
}

// vdmStoreNew(resolution, tileSize) -> bound VdmStore wrapper (non-owning; free
// via vdmStoreFree). Pass <= 0 to keep a VdmStoreParams default.
napi_value NapiRuntime::VdmStoreNew(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  void *data;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);

  int32_t resolution = 0, tileSize = 0;
  if (argc >= 1)
    napi_get_value_int32(env, argv[0], &resolution);
  if (argc >= 2)
    napi_get_value_int32(env, argv[1], &tileSize);

  napi_value out;
  void *store = VdmStore_new(resolution, tileSize);
  const binding::BindingBase *st = rt->lookup("sculptcore::vdm::VdmStore");
  if (!store || !st || st->type != BindingType::Struct) {
    if (store) {
      VdmStore_free(store);
    }
    napi_get_undefined(env, &out);
    return out;
  }
  return rt->instantiate(
      static_cast<const types::_StructBase *>(st), store, /*owning=*/false);
}

napi_value NapiRuntime::VdmStoreFree(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value undef;
  napi_get_undefined(env, &undef);
  Wrapped *sw = nullptr;
  if (argc >= 1 && napi_unwrap(env, argv[0], reinterpret_cast<void **>(&sw)) == napi_ok &&
      sw && sw->ptr)
  {
    VdmStore_free(sw->ptr);
    // Null the wrapper so a later member access or finalizer can't dereference
    // freed storage (same contract as meshFree).
    sw->ptr = nullptr;
  }
  return undef;
}

// meshVdmSplatDab(mesh, tree, store, cx,cy,cz, nx,ny,nz, radius, strength,
// alpha, invert) -> texels touched. Writes tangent-space texels into UV-keyed
// tiles only — no vertex moves; the caller owns any undo bracket.
napi_value NapiRuntime::MeshVdmSplatDab(napi_env env, napi_callback_info info)
{
  size_t argc = 13;
  napi_value argv[13];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_create_int32(env, 0, &out);
  Wrapped *mw = nullptr, *tw = nullptr, *sw = nullptr;
  if (argc < 13 || napi_unwrap(env, argv[0], reinterpret_cast<void **>(&mw)) != napi_ok ||
      napi_unwrap(env, argv[1], reinterpret_cast<void **>(&tw)) != napi_ok ||
      napi_unwrap(env, argv[2], reinterpret_cast<void **>(&sw)) != napi_ok || !mw ||
      !tw || !sw || !mw->ptr || !tw->ptr || !sw->ptr)
  {
    return out;
  }
  double f[9] = {};
  for (int i = 0; i < 9; i++) {
    napi_get_value_double(env, argv[3 + i], &f[i]);
  }
  int32_t invert = 0;
  napi_get_value_int32(env, argv[12], &invert);
  int n = Mesh_vdmSplatDab(mw->ptr,
                           tw->ptr,
                           sw->ptr,
                           float(f[0]),
                           float(f[1]),
                           float(f[2]),
                           float(f[3]),
                           float(f[4]),
                           float(f[5]),
                           float(f[6]),
                           float(f[7]),
                           float(f[8]),
                           invert);
  napi_create_int32(env, n, &out);
  return out;
}

// meshVdmSplatDabLogged(mesh, tree, store, meshLog, cx,cy,cz, nx,ny,nz,
// radius, strength, alpha, invert) -> texels touched. The interactive splat:
// the store delta rides `meshLog`'s open step as a VdmLogChunk.
napi_value NapiRuntime::MeshVdmSplatDabLogged(napi_env env, napi_callback_info info)
{
  size_t argc = 14;
  napi_value argv[14];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_create_int32(env, 0, &out);
  Wrapped *mw = nullptr, *tw = nullptr, *sw = nullptr, *lw = nullptr;
  if (argc < 14 || napi_unwrap(env, argv[0], reinterpret_cast<void **>(&mw)) != napi_ok ||
      napi_unwrap(env, argv[1], reinterpret_cast<void **>(&tw)) != napi_ok ||
      napi_unwrap(env, argv[2], reinterpret_cast<void **>(&sw)) != napi_ok ||
      napi_unwrap(env, argv[3], reinterpret_cast<void **>(&lw)) != napi_ok || !mw ||
      !tw || !sw || !lw || !mw->ptr || !tw->ptr || !sw->ptr || !lw->ptr)
  {
    return out;
  }
  double f[9] = {};
  for (int i = 0; i < 9; i++) {
    napi_get_value_double(env, argv[4 + i], &f[i]);
  }
  int32_t invert = 0;
  napi_get_value_int32(env, argv[13], &invert);
  int n = Mesh_vdmSplatDabLogged(mw->ptr,
                                 tw->ptr,
                                 sw->ptr,
                                 lw->ptr,
                                 float(f[0]),
                                 float(f[1]),
                                 float(f[2]),
                                 float(f[3]),
                                 float(f[4]),
                                 float(f[5]),
                                 float(f[6]),
                                 float(f[7]),
                                 float(f[8]),
                                 invert);
  napi_create_int32(env, n, &out);
  return out;
}

// meshVdmApplyToVerts(mesh, store, clearStore) -> verts moved. X4 VDM ->
// geometry extraction; caller owns undo snapshots + spatial refresh.
napi_value NapiRuntime::MeshVdmApplyToVerts(napi_env env, napi_callback_info info)
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
  int32_t clearStore = 0;
  napi_get_value_int32(env, argv[2], &clearStore);
  int n = Mesh_vdmApplyToVerts(mw->ptr, sw->ptr, clearStore);
  napi_create_int32(env, n, &out);
  return out;
}

// vdmStoreSerialize(store) -> Uint8Array | undefined (v2 store container).
napi_value NapiRuntime::VdmStoreSerialize(napi_env env, napi_callback_info info)
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
  uint8_t *buf = VdmStore_serialize(w->ptr, &size);
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

// vdmStoreRestoreBlob(store, bytes) -> boolean. Refills an EXISTING store
// (instance kept — meshlog chunks hold non-owning pointers into it).
napi_value NapiRuntime::VdmStoreRestoreBlob(napi_env env, napi_callback_info info)
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
  }
  if (!bytes || byteLen == 0) {
    return out;
  }
  int ok = VdmStore_restoreBlob(
      w->ptr, static_cast<const uint8_t *>(bytes), static_cast<int>(byteLen));
  napi_get_boolean(env, ok != 0, &out);
  return out;
}

// vdmStoreDeserialize(bytes) -> bound VdmStore wrapper | undefined. Params
// (backend/tile size/resolution/Ptex tables) all ride the blob.
napi_value NapiRuntime::VdmStoreDeserialize(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  void *data;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);
  napi_value out;
  napi_get_undefined(env, &out);
  void *bytes = nullptr;
  size_t byteLen = 0;
  bool isTa = false;
  if (argc >= 1) {
    napi_is_typedarray(env, argv[0], &isTa);
  }
  if (isTa) {
    napi_typedarray_type t;
    napi_value ab;
    size_t off = 0;
    napi_get_typedarray_info(env, argv[0], &t, &byteLen, &bytes, &ab, &off);
  }
  if (!bytes || byteLen == 0) {
    return out;
  }
  void *store = VdmStore_deserialize(static_cast<const uint8_t *>(bytes),
                                     static_cast<int>(byteLen));
  const binding::BindingBase *st = rt->lookup("sculptcore::vdm::VdmStore");
  if (!store || !st || st->type != BindingType::Struct) {
    if (store) {
      VdmStore_free(store);
    }
    return out;
  }
  return rt->instantiate(
      static_cast<const types::_StructBase *>(st), store, /*owning=*/false);
}

// vdmLastSplatClamped() -> texelsClamped of this thread's most recent
// meshVdmSplatDab (the X1 add-a-level prompt signal).
napi_value NapiRuntime::VdmLastSplatClamped(napi_env env, napi_callback_info)
{
  napi_value out;
  napi_create_int32(env, Vdm_lastSplatClamped(), &out);
  return out;
}

} // namespace sculptcore::napi
