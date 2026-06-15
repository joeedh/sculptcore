// C++ N-API reflection runtime for sculptcore (Workstream B of
// documentation/plans/native-electron.md).
//
// Ports the WASM-side TS runtime (litestl/binding/typescriptRuntime/manager.ts
// + bind.ts) to C++, but reads the `litestl::binding` descriptors directly and
// dereferences real `void*`s instead of indexing a serialized heap. Pointers
// never cross into JS as numbers — C++ does every dereference.
//
// Slice B1 (this file): build a JS class per struct (member get/set accessors),
// getBoundPointer (struct/pointer/reference/number/bool/enum), and construct()
// via a struct's default constructor. Methods, the bulk-data external
// ArrayBuffer fast path, Vector, and strings are deferred to later slices.

#pragma once

#include <node_api.h>

#include <string>
#include <unordered_map>

#include "litestl/binding/binding_base.h"
#include "litestl/binding/binding_struct.h"
#include "litestl/binding/manager.h"

namespace sculptcore::napi {

namespace binding = litestl::binding;
namespace types = litestl::binding::types;

class NapiRuntime {
 public:
  NapiRuntime(napi_env env, binding::BindingManager *mgr) : env_(env), mgr_(mgr) {}

  // Installs the runtime's functions (version, bindingCount, structNames,
  // structInfo, construct) onto `exports`.
  void installExports(napi_value exports);

  // Wrap a C++ value of descriptor `binding` living at `addr` as a JS value.
  // For Pointer/Reference, `addr` is the address of the pointer; null -> undefined.
  napi_value getBoundPointer(const binding::BindingBase *binding, void *addr);

 private:
  napi_env env_;
  binding::BindingManager *mgr_;
  std::unordered_map<std::string, napi_ref> classRefs_;
  // One JS class per (element type, length) for fixed-size Array members
  // (e.g. float[3]). Index accessors live on the prototype (like struct member
  // accessors) — a plain object with napi_define_properties accessors had a
  // value-lifetime bug on the first element.
  std::unordered_map<std::string, napi_ref> arrayClassRefs_;

  // Per-struct JS constructor (built once, accessors on the prototype).
  napi_value getBoundClass(const types::_StructBase *st);
  // Make a JS instance wrapping `ptr`. owning => finalizer destructs + frees it.
  napi_value instantiate(const types::_StructBase *st, void *ptr, bool owning);
  // Wrap a fixed-size inline array (e.g. float3.vec) as a JS class *instance*
  // with live per-index get/set (reads/writes base + i*elemSize directly — no
  // ArrayBuffer, so it survives the V8 sandbox) plus a `length`. Index accessors
  // live on the per-(elem,count) class prototype.
  napi_value arrayWrapper(const binding::BindingBase *elem, void *base, uint32_t count);
  napi_value getArrayClass(const binding::BindingBase *elem, uint32_t count);

  const binding::BindingBase *lookup(const char *name) const;

  // --- napi callbacks (static; receive the runtime / descriptor via data) ---
  static napi_value ctorCb(napi_env, napi_callback_info);
  static void finalizeWrapped(napi_env, void *data, void *hint);
  // [Symbol.dispose]() — deterministic teardown mirroring the WASM runtime's
  // bound-class dispose: destructs + frees an owning instance and nulls the
  // pointer so the GC finalizer can't double-free. No-op on non-owning wrappers.
  static napi_value disposeCb(napi_env, napi_callback_info);
  static napi_value memberGetter(napi_env, napi_callback_info);
  static napi_value memberSetter(napi_env, napi_callback_info);
  static napi_value methodInvoker(napi_env, napi_callback_info);
  static napi_value arrayCtorCb(napi_env, napi_callback_info);
  static void finalizeArrayInst(napi_env, void *data, void *hint);
  static napi_value arrayGetter(napi_env, napi_callback_info);
  static napi_value arraySetter(napi_env, napi_callback_info);

  static napi_value Version(napi_env, napi_callback_info);
  static napi_value BindingCount(napi_env, napi_callback_info);
  static napi_value StructNames(napi_env, napi_callback_info);
  static napi_value StructInfo(napi_env, napi_callback_info);
  static napi_value Construct(napi_env, napi_callback_info);
  // constructWith(structName, ctorName, ...args) -> bound owning instance built
  // with a named, parameterized constructor (e.g. CommandExecutor "main").
  static napi_value ConstructWith(napi_env, napi_callback_info);
  // makeNodeVector() -> a fresh owning, empty Vector<SpatialNode*> (the brush
  // path's filterNodes out-param; descriptor recovered from leaves()'s return).
  static napi_value MakeNodeVector(napi_env, napi_callback_info);
  // makeIntVector() -> a fresh owning, empty Vector<int> (the screen-pick
  // faces/verts out-params; descriptor recovered from castScreenCircle's param).
  static napi_value MakeIntVector(napi_env, napi_callback_info);
  // makeFloatVector() -> a fresh owning, empty Vector<float> (edgePathCoords'
  // out-param; descriptor recovered from that method's param).
  static napi_value MakeFloatVector(napi_env, napi_callback_info);
  // Bulk-data fast path / minimal Vector surface (litestl::util::Vector).
  static napi_value VectorLength(napi_env, napi_callback_info);
  static napi_value VectorView(napi_env, napi_callback_info);
  // Indexed element access: vectorGet(vec, i) -> the i-th element as a bound
  // value/wrapper (number/bool by value; struct/pointer as a wrapper).
  static napi_value VectorGet(napi_env, napi_callback_info);
  // pointerBytes(boundObj, memberName, byteLen) -> Uint8Array over the bytes a
  // raw-pointer member (e.g. gpu::Buffer.data) points at — the native bulk-data
  // read (the pointer never crosses to JS as a number).
  static napi_value PointerBytes(napi_env, napi_callback_info);
  // objectAddress(boundObj) -> the wrapped C++ address as an opaque JS-number
  // identity key (never dereferenced; for caches keyed by object identity).
  static napi_value ObjectAddress(napi_env, napi_callback_info);
  // Native factory free-functions (Workstream C): create/operate on real engine
  // objects, wrapping the returned pointers as bound objects.
  static napi_value MeshCreateCube(napi_env, napi_callback_info);
  // meshMakeUVSphere(rings, segs, radius) -> Mesh; UV-sphere primitive.
  static napi_value MeshMakeUVSphere(napi_env, napi_callback_info);
  static napi_value MeshBuildSpatialTree(napi_env, napi_callback_info);
  static napi_value SpatialTreeFree(napi_env, napi_callback_info);
  static napi_value MeshFree(napi_env, napi_callback_info);
  // meshTriangulate(mesh) -> void; fan-triangulate every n-gon in place.
  static napi_value MeshTriangulate(napi_env, napi_callback_info);
  // meshQuadRemesh(mesh, params) -> Mesh; feature-aligned quad remesh.
  static napi_value MeshQuadRemesh(napi_env, napi_callback_info);
  // meshSerialize(mesh) -> Uint8Array; meshDeserialize(bytes) -> Mesh wrapper.
  // The versioned, lz4hc-compressed blob (mesh/c-api serializeMesh/deserializeMesh).
  static napi_value MeshSerialize(napi_env, napi_callback_info);
  static napi_value MeshDeserialize(napi_env, napi_callback_info);
  // M5 requested-attribute bridge (spatial/c-api setTree*). Strings + JS arrays
  // can't cross the generic method binding (marshalArg), so these route through
  // dedicated extern "C" calls like the Mesh_* factories.
  // spatialTreeSetRequestedAttrs(tree, count, namesJoined, srcTypes, elemSizes,
  //   slots, domains, defaultKinds) — the int args are Int32Arrays.
  static napi_value SpatialTreeSetRequestedAttrs(napi_env, napi_callback_info);
  // spatialTreeSetDrawShader(tree, wgsl:string)
  static napi_value SpatialTreeSetDrawShader(napi_env, napi_callback_info);
  // spatialTreeGetMissingAttrSlots(tree) -> number[]
  static napi_value SpatialTreeGetMissingAttrSlots(napi_env, napi_callback_info);
  // spatialTreeRefreshRequestedAttrs(tree) -> void (force buffer rebuild on layer change)
  static napi_value SpatialTreeRefreshRequestedAttrs(napi_env, napi_callback_info);

  // litestl allocator introspection (binding.cc LSTL_*).
  // getMemSize(includePermanent) -> number (tracked allocation size in bytes).
  static napi_value GetMemSize(napi_env, napi_callback_info);
  // printAllocBlocks(includePermanent) -> void (dumps live blocks to the log sink).
  static napi_value PrintAllocBlocks(napi_env, napi_callback_info);
  // formatBlock(boundObj) -> string describing the allocation backing the wrapped
  // object. Wraps the LSTL_FormatBlock/LSTL_FreeFormatBlocks heap-string pair (the
  // raw char* never crosses into JS).
  static napi_value FormatBlock(napi_env, napi_callback_info);
  // formatBlocks(printPermanent) -> string describing every live block (same
  // heap-string pair as formatBlock).
  static napi_value FormatBlocks(napi_env, napi_callback_info);

  void define(napi_value exports, const char *name, napi_callback cb);
};

// Per-instance data attached via napi_wrap.
struct Wrapped {
  void *ptr;
  const types::_StructBase *st;
  bool owning;
};

// Per-member-accessor data (data field of the property descriptor).
struct AccessorCtx {
  NapiRuntime *rt;
  const types::StructMember *member;
};

// Per-class constructor data.
struct CtorCtx {
  NapiRuntime *rt;
  const types::_StructBase *st;
};

// Per-method-property data.
struct MethodCtx {
  NapiRuntime *rt;
  const types::Method *method;
};

}  // namespace sculptcore::napi
