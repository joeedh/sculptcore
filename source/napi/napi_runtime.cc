#include "napi_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "napi_log.h"

#include "litestl/binding/binding_constructor.h"
#include "litestl/binding/binding_method.h"
#include "litestl/binding/binding_number.h"
#include "litestl/binding/binding_types.h"

// Native engine factory free-functions (extern "C"; defined in the linked
// mesh/spatial libs — source/mesh/mesh_shapes.cc, source/spatial/c-api/
// spatial_c_api.cc). void* stands in for the opaque Mesh*/SpatialTree* — ABI
// identical for an extern "C" pointer.
extern "C" {

size_t LSTL_GetMemSize(bool includePermanent);
void LSTL_PrintAllocBlocks(bool includePermanent);
void LSTL_FreeFormatBlocks(char *s);
char *LSTL_FormatBlock(void *mem);
char *LSTL_FormatBlocks(bool printPermanent);

void IntVector_assign(litestl::util::Vector<int> *vec, const int *data, int count);
void FloatVector_assign(litestl::util::Vector<float> *vec, const float *data, int count);

void *Mesh_createCube(int dimen, float size, float sphereFac);
void *Mesh_makeGrid(int nx, int ny, float size);
void *Mesh_makeUVSphere(int rings, int segs, float radius);
void *Mesh_buildSpatialTree(void *mesh, int leafLimit, int depthLimit, int gpuTriTarget);
void SpatialTree_free(void *tree);
void Mesh_free(void *mesh);
void Mesh_triangulate(void *mesh);
// Feature-aligned quad remesh (source/remesh/c-api/remesh_c_api.cc). Returns a
// new Mesh* (input untouched); null on clean failure.
void *Mesh_quadRemesh(void *mesh, void *params);
// Versioned, lz4hc-compressed mesh blob (source/mesh/c-api/mesh_c_api.cc).
uint8_t *serializeMesh(void *mesh, int *out_size);
// Uncompressed column payload only (autosave worker compresses off-thread).
uint8_t *serializeMeshRaw(void *mesh, int *out_size);
void *deserializeMesh(const uint8_t *data, int size);
void freeMeshBuffer(uint8_t *buf);
// M5 requested-attribute bridge (source/spatial/c-api/spatial_c_api.cc).
void setTreeRequestedAttrs(void *tree,
                           int count,
                           const char *namesJoined,
                           const int *srcTypes,
                           const int *elemSizes,
                           const int *slots,
                           const int *domains,
                           const int *defaultKinds);
void setTreeDrawShader(void *tree, const char *wgsl);
int getTreeMissingAttrSlots(void *tree, int *out, int maxOut);
void refreshTreeRequestedAttrs(void *tree);
// GPU brush-stroke seam (source/brush/c-api/gpu_brush_c_api.cc).
void *GpuBrush_beginStroke(void *mesh, void *tree, void *brush, void *meshLog, int tool);
void GpuBrush_free(void *session);
const char *GpuBrush_kernelName(void *session);
int GpuBrush_info(void *session, int which);
int GpuBrush_marshalDab(void *session,
                        float cx,
                        float cy,
                        float cz,
                        float nx,
                        float ny,
                        float nz,
                        float radius,
                        float filterRadius,
                        int mirrorIdx,
                        int nonaccum);
int GpuBrush_dataSize(void *session, int which);
const void *GpuBrush_dataPtr(void *session, int which);
void GpuBrush_applyCo(void *session, const float *co, int elemCount);
void GpuBrush_endStroke(void *session, const float *co, const float *no, int elemCount);
// VDM engine seam (source/vdm/c-api/vdm_c_api.cc).
void *VdmStore_new(int resolution, int tileSize);
void VdmStore_free(void *store);
int Mesh_vdmSplatDab(void *mesh,
                     void *tree,
                     void *store,
                     float cx,
                     float cy,
                     float cz,
                     float nx,
                     float ny,
                     float nz,
                     float radius,
                     float strength,
                     float alpha,
                     int invert);
int Mesh_vdmSplatDabLogged(void *mesh,
                           void *tree,
                           void *store,
                           void *log,
                           float cx,
                           float cy,
                           float cz,
                           float nx,
                           float ny,
                           float nz,
                           float radius,
                           float strength,
                           float alpha,
                           int invert);
uint8_t *VdmStore_serialize(void *store, int *out_size);
int Mesh_vdmApplyToVerts(void *mesh, void *store, int clearStore);
int VdmStore_restoreBlob(void *store, const uint8_t *data, int size);
void *VdmStore_deserialize(const uint8_t *data, int size);
int Vdm_lastSplatClamped();
void SpatialTree_fillDetailCarrier(void *tree, int carrier);
void Mesh_updateFrames(void *mesh);
// Sculpt-layer settings mutators (source/displace/c-api/displace_c_api.cc).
void Mesh_layerSetWeight(void *mesh, int li, float weight);
void Mesh_layerSetEnabled(void *mesh, int li, int enabled);
void Mesh_layerSetFrozen(void *mesh, int li, int frozen);
void Mesh_layerRemove(void *mesh, int li);
int Mesh_setActiveEditLayer(void *mesh, int li);
void Mesh_layerFold(void *mesh);
// Multires seam (source/subdiv/c-api/subdiv_c_api.cc).
void *
Multires_new(void *cage, int levels, int leafLimit, int depthLimit, int gpuTriTarget);
void Multires_free(void *mr);
int Multires_setActiveLevel(void *mr, int level);
void *Multires_activeMesh(void *mr);
void *Multires_activeTree(void *mr);
int Multires_writeback(void *mr, int level);
int Multires_downRefit(void *mr, int level);
uint8_t *Multires_serializeStore(void *mr, int *out_size);
int Multires_captureToVdm(void *mr, void *vstore, int level);
int Multires_restoreStore(void *mr, const uint8_t *data, int size);
}

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

using binding::BindingType;
using binding::NumberFlags;
using binding::NumberType;

// All Number<T> instantiations share an identical layout (the template param is
// only a `using`, never a stored field), so we read subtype/flags through any
// one of them.
static const types::Number<char> *asNumber(const binding::BindingBase *b)
{
  return reinterpret_cast<const types::Number<char> *>(b);
}
static bool isUnsigned(const types::Number<char> *n)
{
  return (static_cast<int>(n->flags) & static_cast<int>(NumberFlags::Unsigned)) != 0;
}

// Read a JS value as the C++ number described by `num` and write it to `dst`
// (which must point at storage of the matching size). Shared by member setters
// and method-argument marshalling.
static void
writeNumberValue(napi_env env, napi_value v, const types::Number<char> *num, void *dst)
{
  const bool uns = isUnsigned(num);
  if (num->subtype == NumberType::Int64) {
    bool lossless = false;
    if (uns) {
      uint64_t x = 0;
      napi_get_value_bigint_uint64(env, v, &x, &lossless);
      *static_cast<uint64_t *>(dst) = x;
    } else {
      int64_t x = 0;
      napi_get_value_bigint_int64(env, v, &x, &lossless);
      *static_cast<int64_t *>(dst) = x;
    }
    return;
  }
  double d = 0.0;
  napi_get_value_double(env, v, &d);
  switch (num->subtype) {
  case NumberType::Int8:
    uns ? (*static_cast<uint8_t *>(dst) = static_cast<uint8_t>(d))
        : (*static_cast<int8_t *>(dst) = static_cast<int8_t>(d));
    break;
  case NumberType::Int16:
    uns ? (*static_cast<uint16_t *>(dst) = static_cast<uint16_t>(d))
        : (*static_cast<int16_t *>(dst) = static_cast<int16_t>(d));
    break;
  case NumberType::Int32:
    uns ? (*static_cast<uint32_t *>(dst) = static_cast<uint32_t>(d))
        : (*static_cast<int32_t *>(dst) = static_cast<int32_t>(d));
    break;
  case NumberType::Float32:
    *static_cast<float *>(dst) = static_cast<float>(d);
    break;
  case NumberType::Float64:
    *static_cast<double *>(dst) = d;
    break;
  case NumberType::Int64:
    break; // handled above
  }
}

// Extract the raw C++ pointer behind a bound JS wrapper (or null for
// null/undefined / non-wrapped values).
static void *unwrapPtr(napi_env env, napi_value v)
{
  if (!v)
    return nullptr;
  napi_valuetype t;
  napi_typeof(env, v, &t);
  if (t == napi_null || t == napi_undefined)
    return nullptr;
  Wrapped *w = nullptr;
  if (napi_unwrap(env, v, reinterpret_cast<void **>(&w)) == napi_ok && w) {
    return w->ptr;
  }
  return nullptr;
}

const binding::BindingBase *NapiRuntime::lookup(const char *name) const
{
  const auto &m = mgr_->getBindings();
  binding::string key(name);
  if (!m.contains(key))
    return nullptr;
  return m.lookup(key);
}

// Shared by every instance of one (elem, count) array class; lives on the
// class's ctor data.
struct ArrayClassCtx {
  NapiRuntime *rt;
  const binding::BindingBase *elem;
  size_t elemSize;
  uint32_t count;
};
// Per-array-instance; attached via napi_wrap. Holds the element base address.
struct ArrayInstData {
  ArrayClassCtx *cls;
  void *base;
};
// Per-index accessor data (lives on the class prototype's [i] property). The
// base address is per-instance, so it comes from unwrapping `this`, not here.
struct ArrayIndexCtx {
  ArrayClassCtx *cls;
  uint32_t index;
};

// ---------------------------------------------------------------------------
// getBoundPointer — the heart of the runtime.
// ---------------------------------------------------------------------------
napi_value NapiRuntime::getBoundPointer(const binding::BindingBase *binding, void *addr)
{
  napi_value out;
  if (!binding || !addr) {
    napi_get_undefined(env_, &out);
    return out;
  }

  switch (binding->type) {
  case BindingType::Number: {
    const types::Number<char> *num = asNumber(binding);
    const bool uns = isUnsigned(num);
    switch (num->subtype) {
    case NumberType::Int8:
      uns ? napi_create_uint32(env_, *static_cast<uint8_t *>(addr), &out)
          : napi_create_int32(env_, *static_cast<int8_t *>(addr), &out);
      break;
    case NumberType::Int16:
      uns ? napi_create_uint32(env_, *static_cast<uint16_t *>(addr), &out)
          : napi_create_int32(env_, *static_cast<int16_t *>(addr), &out);
      break;
    case NumberType::Int32:
      uns ? napi_create_uint32(env_, *static_cast<uint32_t *>(addr), &out)
          : napi_create_int32(env_, *static_cast<int32_t *>(addr), &out);
      break;
    case NumberType::Int64:
      uns ? napi_create_bigint_uint64(env_, *static_cast<uint64_t *>(addr), &out)
          : napi_create_bigint_int64(env_, *static_cast<int64_t *>(addr), &out);
      break;
    case NumberType::Float32:
      napi_create_double(env_, *static_cast<float *>(addr), &out);
      break;
    case NumberType::Float64:
      napi_create_double(env_, *static_cast<double *>(addr), &out);
      break;
    }
    return out;
  }
  case BindingType::Boolean:
    napi_get_boolean(env_, *static_cast<bool *>(addr), &out);
    return out;
  case BindingType::Enum:
    // Enums are stored int-wide; report the underlying value.
    napi_create_int32(env_, *static_cast<int32_t *>(addr), &out);
    return out;
  case BindingType::Struct: {
    const types::_StructBase *sb = static_cast<const types::_StructBase *>(binding);
    // litestl::util::String<char> is bound as a member-less Struct
    // ("litestl::util::String", see binding.h). Read its null-terminated
    // contents (Char *data_ at offset 0) directly as a JS string rather than
    // handing back an opaque empty wrapper. Needed e.g. for gpu::Buffer.name /
    // ShaderDef attr names, which the renderer matches by string equality.
    if (std::strcmp(sb->name.c_str(), "litestl::util::String") == 0) {
      const char *s = *reinterpret_cast<const char *const *>(addr);
      if (!s) {
        napi_get_undefined(env_, &out);
        return out;
      }
      napi_create_string_utf8(env_, s, NAPI_AUTO_LENGTH, &out);
      return out;
    }
    // Embedded struct: a non-owning wrapper over the inline storage.
    return instantiate(sb, addr, false);
  }
  case BindingType::Pointer: {
    void *indirect = *static_cast<void **>(addr);
    if (!indirect) {
      napi_get_undefined(env_, &out);
      return out;
    }
    return getBoundPointer(static_cast<const types::Pointer *>(binding)->ptrType,
                           indirect);
  }
  case BindingType::Reference: {
    void *indirect = *static_cast<void **>(addr);
    if (!indirect) {
      napi_get_undefined(env_, &out);
      return out;
    }
    return getBoundPointer(static_cast<const types::Reference *>(binding)->refType,
                           indirect);
  }
  case BindingType::Array: {
    // Fixed-size inline array (e.g. float3.vec): a live indexable wrapper.
    // Array<T> layout is identical for any T (arrayType + arraySize after
    // BindingBase), so read through Array<char>.
    const types::Array<char> *arr = reinterpret_cast<const types::Array<char> *>(binding);
    return arrayWrapper(arr->arrayType, addr, static_cast<uint32_t>(arr->arraySize));
  }
  default:
    // Union / Method / string: later slices.
    napi_get_undefined(env_, &out);
    return out;
  }
}

// ---------------------------------------------------------------------------
// Class building + instantiation.
// ---------------------------------------------------------------------------
napi_value NapiRuntime::getBoundClass(const types::_StructBase *st)
{
  // Key by the *full* templated name: every litestl::util::Vector<T,N> shares
  // the bare name "litestl::util::Vector", so keying by st->name would alias all
  // element types onto one class (and one wrong CtorCtx). buildFullName()
  // disambiguates ("...Vector<float,4>").
  std::string key(st->buildFullName().c_str());
  auto it = classRefs_.find(key);
  if (it != classRefs_.end()) {
    napi_value cls;
    napi_get_reference_value(env_, it->second, &cls);
    return cls;
  }

  std::vector<napi_property_descriptor> props;
  props.reserve(st->members.size());
  for (const auto &member : st->members) {
    AccessorCtx *actx = new AccessorCtx{this, &member};
    napi_property_descriptor d = {};
    d.utf8name = member.name.c_str();
    d.getter = &NapiRuntime::memberGetter;
    d.setter = &NapiRuntime::memberSetter;
    d.attributes = napi_enumerable;
    d.data = actx;
    props.push_back(d);
  }

  // Methods on the prototype. Skip statics (later slice) and names that collide
  // with a member or an already-added method (overloads share a name; first wins
  // until signature-based overload resolution lands).
  std::unordered_set<std::string> taken;
  for (const auto &member : st->members)
    taken.insert(std::string(member.name.c_str()));
  for (const types::Method *m : st->methods) {
    if (!m || m->isStatic)
      continue;
    std::string mn(m->name.c_str());
    if (taken.count(mn))
      continue;
    taken.insert(mn);
    MethodCtx *mctx = new MethodCtx{this, m};
    napi_property_descriptor d = {};
    d.utf8name = m->name.c_str();
    d.method = &NapiRuntime::methodInvoker;
    d.attributes = napi_default;
    d.data = mctx;
    props.push_back(d);
  }

  // [Symbol.dispose]() for deterministic teardown (mirrors the WASM bound-class
  // dispose). Keyed by the well-known symbol via the descriptor's `name` field;
  // skipped on runtimes too old to expose Symbol.dispose.
  napi_value disposeSym = nullptr;
  {
    napi_value global, symbolCtor;
    napi_get_global(env_, &global);
    if (napi_get_named_property(env_, global, "Symbol", &symbolCtor) == napi_ok) {
      napi_get_named_property(env_, symbolCtor, "dispose", &disposeSym);
      napi_valuetype t = napi_undefined;
      if (disposeSym)
        napi_typeof(env_, disposeSym, &t);
      if (t == napi_symbol) {
        napi_property_descriptor d = {};
        d.name = disposeSym;
        d.method = &NapiRuntime::disposeCb;
        d.attributes = napi_default;
        d.data = this;
        props.push_back(d);
      }
    }
  }

  CtorCtx *cctx = new CtorCtx{this, st};
  napi_value cls;
  napi_define_class(env_,
                    key.c_str(),
                    NAPI_AUTO_LENGTH,
                    &NapiRuntime::ctorCb,
                    cctx,
                    props.size(),
                    props.data(),
                    &cls);

  napi_ref ref;
  napi_create_reference(env_, cls, 1, &ref);
  classRefs_[key] = ref;
  return cls;
}

napi_value NapiRuntime::instantiate(const types::_StructBase *st, void *ptr, bool owning)
{
  napi_value cls = getBoundClass(st);
  napi_value ext, owningv;
  napi_create_external(env_, ptr, nullptr, nullptr, &ext);
  napi_get_boolean(env_, owning, &owningv);
  napi_value argv[2] = {ext, owningv};
  napi_value inst;
  napi_new_instance(env_, cls, 2, argv, &inst);
  return inst;
}

// Build (once, cached) a JS class for a fixed-size array of `count` elements of
// type `elem`. Index accessors [0..count) live on the prototype — same shape as
// struct member accessors, which work; the earlier plain-object +
// napi_define_properties form had a value-lifetime bug on element[0] under the
// V8 sandbox here. The per-instance base pointer is held by the wrapped
// instance (ArrayInstData), not by the accessor data.
napi_value NapiRuntime::getArrayClass(const binding::BindingBase *elem, uint32_t count)
{
  size_t elemSize = elem ? elem->getSize() : 0;
  // Key by element descriptor identity + count: descriptor pointers are stable
  // for the runtime's lifetime (the BindingManager owns them).
  std::string key =
      std::to_string(reinterpret_cast<uintptr_t>(elem)) + ":" + std::to_string(count);
  auto it = arrayClassRefs_.find(key);
  if (it != arrayClassRefs_.end()) {
    napi_value cls;
    napi_get_reference_value(env_, it->second, &cls);
    return cls;
  }

  ArrayClassCtx *cls = new ArrayClassCtx{this, elem, elemSize, count};

  // Index names must outlive the define_class call.
  std::vector<std::string> names;
  names.reserve(count);
  for (uint32_t i = 0; i < count; i++)
    names.push_back(std::to_string(i));

  std::vector<napi_property_descriptor> props;
  props.reserve(count + 1);
  for (uint32_t i = 0; i < count; i++) {
    ArrayIndexCtx *ic = new ArrayIndexCtx{cls, i};
    napi_property_descriptor d = {};
    d.utf8name = names[i].c_str();
    d.getter = &NapiRuntime::arrayGetter;
    d.setter = &NapiRuntime::arraySetter;
    d.attributes = napi_enumerable;
    d.data = ic;
    props.push_back(d);
  }
  // length: a constant data property on the prototype (read-only).
  napi_value len;
  napi_create_uint32(env_, count, &len);
  napi_property_descriptor lenD = {};
  lenD.utf8name = "length";
  lenD.value = len;
  lenD.attributes = napi_enumerable;
  props.push_back(lenD);

  napi_value jsCls;
  napi_define_class(env_,
                    key.c_str(),
                    NAPI_AUTO_LENGTH,
                    &NapiRuntime::arrayCtorCb,
                    cls,
                    props.size(),
                    props.data(),
                    &jsCls);
  napi_ref ref;
  napi_create_reference(env_, jsCls, 1, &ref);
  arrayClassRefs_[key] = ref;
  return jsCls;
}

napi_value
NapiRuntime::arrayWrapper(const binding::BindingBase *elem, void *base, uint32_t count)
{
  size_t elemSize = elem ? elem->getSize() : 0;
  if (!base || elemSize == 0) {
    napi_value obj;
    napi_create_object(env_, &obj);
    return obj;
  }
  napi_value cls = getArrayClass(elem, count);
  napi_value ext;
  napi_create_external(env_, base, nullptr, nullptr, &ext);
  napi_value argv[1] = {ext};
  napi_value inst;
  napi_new_instance(env_, cls, 1, argv, &inst);
  return inst;
}

napi_value NapiRuntime::arrayCtorCb(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_value thisArg;
  void *data;
  napi_get_cb_info(env, info, &argc, argv, &thisArg, &data);
  ArrayClassCtx *cls = static_cast<ArrayClassCtx *>(data);
  void *base = nullptr;
  if (argc >= 1)
    napi_get_value_external(env, argv[0], &base);
  ArrayInstData *inst = new ArrayInstData{cls, base};
  napi_wrap(env, thisArg, inst, &NapiRuntime::finalizeArrayInst, nullptr, nullptr);
  return thisArg;
}

void NapiRuntime::finalizeArrayInst(napi_env, void *data, void *)
{
  // `base` is borrowed from the parent struct's inline storage — never freed
  // here; only the per-instance bookkeeping is released.
  delete static_cast<ArrayInstData *>(data);
}

// Resolve (this, accessor-data) -> the element's address. Null on any failure.
static void *
arrayElemAddr(napi_env env, napi_value thisArg, void *data, ArrayIndexCtx **ctxOut)
{
  ArrayIndexCtx *ic = static_cast<ArrayIndexCtx *>(data);
  if (ctxOut)
    *ctxOut = ic;
  if (!ic)
    return nullptr;
  ArrayInstData *inst = nullptr;
  if (napi_unwrap(env, thisArg, reinterpret_cast<void **>(&inst)) != napi_ok || !inst ||
      !inst->base)
  {
    return nullptr;
  }
  return static_cast<char *>(inst->base) +
         static_cast<size_t>(ic->index) * ic->cls->elemSize;
}

napi_value NapiRuntime::arrayGetter(napi_env env, napi_callback_info info)
{
  napi_value thisArg;
  void *data = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisArg, &data);
  ArrayIndexCtx *ic = nullptr;
  void *addr = arrayElemAddr(env, thisArg, data, &ic);
  napi_value out;
  if (!addr) {
    napi_get_undefined(env, &out);
    return out;
  }
  return ic->cls->rt->getBoundPointer(ic->cls->elem, addr);
}

napi_value NapiRuntime::arraySetter(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_value thisArg;
  void *data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &thisArg, &data);
  ArrayIndexCtx *ic = nullptr;
  void *addr = arrayElemAddr(env, thisArg, data, &ic);
  napi_value undef;
  napi_get_undefined(env, &undef);
  if (!addr || argc < 1)
    return undef;
  const binding::BindingBase *elem = ic->cls->elem;
  if (elem->type == BindingType::Number) {
    writeNumberValue(env, argv[0], asNumber(elem), addr);
  } else if (elem->type == BindingType::Boolean) {
    bool b = false;
    napi_get_value_bool(env, argv[0], &b);
    *static_cast<bool *>(addr) = b;
  }
  return undef;
}

napi_value NapiRuntime::ctorCb(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  napi_value thisArg;
  void *data;
  napi_get_cb_info(env, info, &argc, argv, &thisArg, &data);
  CtorCtx *cctx = static_cast<CtorCtx *>(data);

  void *ptr = nullptr;
  bool owning = false;
  if (argc >= 1)
    napi_get_value_external(env, argv[0], &ptr);
  if (argc >= 2)
    napi_get_value_bool(env, argv[1], &owning);

  Wrapped *w = new Wrapped{ptr, cctx->st, owning};
  napi_wrap(env, thisArg, w, &NapiRuntime::finalizeWrapped, nullptr, nullptr);
  return thisArg;
}

void NapiRuntime::finalizeWrapped(napi_env, void *data, void *)
{
  Wrapped *w = static_cast<Wrapped *>(data);
  if (w->owning && w->ptr) {
    if (w->st->destructorThunk) {
      (*w->st->destructorThunk)(w->ptr);
    }
    std::free(w->ptr);
  }
  delete w;
}

// [Symbol.dispose]() on a bound instance: the deterministic counterpart of the
// GC finalizer above, matching the WASM runtime's bound-class dispose
// (manager.destroyInstance -> destructor + free). Destructs + frees an owning
// instance now and clears the wrapper so neither a later access nor the
// finalizer touches freed storage. A no-op for non-owning wrappers (engine-owned
// objects, embedded-struct/member views) — those must be released by their owner.
napi_value NapiRuntime::disposeCb(napi_env env, napi_callback_info info)
{
  napi_value thisArg;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisArg, nullptr);
  napi_value undef;
  napi_get_undefined(env, &undef);

  Wrapped *w = nullptr;
  if (napi_unwrap(env, thisArg, reinterpret_cast<void **>(&w)) != napi_ok || !w) {
    return undef;
  }
  if (w->owning && w->ptr) {
    if (w->st->destructorThunk) {
      (*w->st->destructorThunk)(w->ptr);
    }
    std::free(w->ptr);
  }
  w->ptr = nullptr;
  w->owning = false;
  return undef;
}

// ---------------------------------------------------------------------------
// Member accessors.
// ---------------------------------------------------------------------------
napi_value NapiRuntime::memberGetter(napi_env env, napi_callback_info info)
{
  napi_value thisArg;
  void *data;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisArg, &data);
  AccessorCtx *ctx = static_cast<AccessorCtx *>(data);

  Wrapped *w = nullptr;
  napi_unwrap(env, thisArg, reinterpret_cast<void **>(&w));
  void *addr = static_cast<char *>(w->ptr) + ctx->member->offset;
  return ctx->rt->getBoundPointer(ctx->member->type, addr);
}

napi_value NapiRuntime::memberSetter(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_value thisArg;
  void *data;
  napi_get_cb_info(env, info, &argc, argv, &thisArg, &data);
  AccessorCtx *ctx = static_cast<AccessorCtx *>(data);

  Wrapped *w = nullptr;
  napi_unwrap(env, thisArg, reinterpret_cast<void **>(&w));
  void *addr = static_cast<char *>(w->ptr) + ctx->member->offset;
  const binding::BindingBase *b = ctx->member->type;

  napi_value undef;
  napi_get_undefined(env, &undef);
  if (argc < 1)
    return undef;

  if (b->type == BindingType::Boolean) {
    bool v = false;
    napi_get_value_bool(env, argv[0], &v);
    *static_cast<bool *>(addr) = v;
    return undef;
  }
  if (b->type == BindingType::Pointer) {
    // Rebind the pointer member to another bound object's address (e.g.
    // CommandExecutor.meshLog = meshLog). The pointee stays C++-owned; we only
    // overwrite the stored void*.
    *reinterpret_cast<void **>(addr) = unwrapPtr(env, argv[0]);
    return undef;
  }
  if (b->type == BindingType::Enum) {
    int32_t v = 0;
    napi_get_value_int32(env, argv[0], &v);
    *reinterpret_cast<int32_t *>(addr) = v;
    return undef;
  }
  if (b->type != BindingType::Number) {
    // set on a by-value struct member is a later slice (needs copy-ctor).
    return undef;
  }

  writeNumberValue(env, argv[0], asNumber(b), addr);
  return undef;
}

// Marshal one JS arg into the C++ thunk ABI, per the param's binding type, and
// return the void* to store in args[i]. `slot` is the 8-byte backing storage for
// by-value scalars / enums / pointer params (the returned pointer points into
// it); reference and by-value-struct params return the wrapped object's address
// directly (the thunk reads *(T*)args[i]). Shared by methodInvoker and
// ConstructWith — the constructor thunk uses the identical arg_t ABI.
static void *
marshalArg(napi_env env, const binding::BindingBase *pt, napi_value a, uint64_t *slot)
{
  switch (pt->type) {
  case BindingType::Number:
    if (a)
      writeNumberValue(env, a, asNumber(pt), slot);
    return slot;
  case BindingType::Enum: {
    // Enums cross as their integer value (backing type is int); the thunk
    // reads *(EnumClass*)args[i], a 4-byte int, so point at the slot.
    int32_t v = 0;
    if (a)
      napi_get_value_int32(env, a, &v);
    *reinterpret_cast<int32_t *>(slot) = v;
    return slot;
  }
  case BindingType::Boolean: {
    bool bv = false;
    if (a)
      napi_get_value_bool(env, a, &bv);
    *reinterpret_cast<bool *>(slot) = bv;
    return slot;
  }
  case BindingType::Pointer:
    // For a pointer-typed argument, the thunk reads *(T**)args[i], so args[i] holds a void* slot.
    *slot = reinterpret_cast<uint64_t>(unwrapPtr(env, a));
    return slot;
  case BindingType::Reference:
  case BindingType::Struct:
    // arg_t is T& / T (by value): the thunk reads *(T*)args[i], so args[i] is
    // the object address itself.
    return unwrapPtr(env, a);
  default:
    return nullptr; // unsupported param kind (later slice)
  }
}

// ---------------------------------------------------------------------------
// Method invocation: marshal JS args -> C++ via the method's thunk, then
// marshal the return. The thunk ABI is void(*)(void* self, void** args, void*
// ret); args[i] points at the i-th argument value (see binding_method.h
// MethodBuilder::invokeImpl for how each arg_t is read).
// ---------------------------------------------------------------------------
napi_value NapiRuntime::methodInvoker(napi_env env, napi_callback_info info)
{
  // argc must be the argv capacity: napi_get_cb_info writes min(capacity, actual)
  // args and sets argc to the actual count. A buffer smaller than the method's
  // param count (e.g. castScreenRect's 10 / selectScreenRect's 12 float3+vec
  // args) leaves the per-param loop reading past argv -> stack corruption / crash.
  size_t argc = 32;
  napi_value argv[32];
  napi_value thisArg;
  void *data;
  napi_get_cb_info(env, info, &argc, argv, &thisArg, &data);
  MethodCtx *ctx = static_cast<MethodCtx *>(data);
  NapiRuntime *rt = ctx->rt;
  const types::Method *m = ctx->method;

  Wrapped *w = nullptr;
  napi_unwrap(env, thisArg, reinterpret_cast<void **>(&w));
  void *self = w ? w->ptr : nullptr;

  const size_t nparams = m->params.size();
  std::vector<void *> args(nparams, nullptr);
  // Uniform 8-byte scalar/pointer slots; args[i] points into here for
  // by-value scalars and pointer params (reference/by-value-struct args point
  // straight at the wrapped object instead).
  std::vector<uint64_t> slots(nparams, 0);

  for (size_t i = 0; i < nparams; i++) {
    napi_value a = (i < argc) ? argv[i] : nullptr;
    args[i] = marshalArg(env, m->params[i].type, a, &slots[i]);
  }

  napi_value undef;
  napi_get_undefined(env, &undef);
  if (!m->thunk)
    return undef;

  if (m->returnType == nullptr) {
    m->thunk(self, args.data(), nullptr);
    return undef;
  }

  size_t rsize = m->returnType->getSize();
  if (rsize == 0)
    rsize = sizeof(void *);
  void *retbuf = std::malloc(rsize);
  m->thunk(self, args.data(), retbuf);

  if (m->returnType->type == BindingType::Struct) {
    // By-value struct return: the caller owns it; wrap owning (finalizer
    // destructs + frees retbuf).
    return rt->instantiate(
        static_cast<const types::_StructBase *>(m->returnType), retbuf, true);
  }
  napi_value result = rt->getBoundPointer(m->returnType, retbuf);
  std::free(retbuf);
  return result;
}

// ---------------------------------------------------------------------------
// Exported functions.
// ---------------------------------------------------------------------------
static const char *bindingTypeName(BindingType t)
{
  switch (t) {
  case BindingType::Boolean:
    return "boolean";
  case BindingType::Number:
    return "number";
  case BindingType::Pointer:
    return "pointer";
  case BindingType::Reference:
    return "reference";
  case BindingType::Struct:
    return "struct";
  case BindingType::Array:
    return "array";
  case BindingType::Method:
    return "method";
  case BindingType::Literal:
    return "literal";
  case BindingType::Constructor:
    return "constructor";
  case BindingType::Enum:
    return "enum";
  case BindingType::Union:
    return "union";
  case BindingType::ParentTemplParam:
    return "templParam";
  }
  return "unknown";
}

napi_value NapiRuntime::Version(napi_env env, napi_callback_info)
{
  napi_value v;
  napi_create_string_utf8(
      env,
      "sculptcore native N-API runtime (Workstream B: structs+members+methods)",
      NAPI_AUTO_LENGTH,
      &v);
  return v;
}

napi_value NapiRuntime::BindingCount(napi_env env, napi_callback_info info)
{
  void *data;
  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);
  napi_value v;
  napi_create_uint32(env, static_cast<uint32_t>(rt->mgr_->getBindings().size()), &v);
  return v;
}

napi_value NapiRuntime::StructNames(napi_env env, napi_callback_info info)
{
  void *data;
  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);

  napi_value arr;
  napi_create_array(env, &arr);
  uint32_t i = 0;
  for (const auto &pair : rt->mgr_->getBindings()) {
    if (pair.value && pair.value->type == BindingType::Struct) {
      napi_value name;
      napi_create_string_utf8(env, pair.key.c_str(), NAPI_AUTO_LENGTH, &name);
      napi_set_element(env, arr, i++, name);
    }
  }
  return arr;
}

// structInfo(name) -> {size, hasDefaultCtor, members:[{name,type,offset}]}
napi_value NapiRuntime::StructInfo(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  void *data;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);

  char buf[512];
  size_t len = 0;
  if (argc >= 1)
    napi_get_value_string_utf8(env, argv[0], buf, sizeof(buf), &len);

  const binding::BindingBase *b = rt->lookup(buf);
  napi_value out;
  if (!b || b->type != BindingType::Struct) {
    napi_get_undefined(env, &out);
    return out;
  }
  const types::_StructBase *st = static_cast<const types::_StructBase *>(b);

  napi_create_object(env, &out);
  napi_value sz;
  napi_create_uint32(env, static_cast<uint32_t>(st->getSize()), &sz);
  napi_set_named_property(env, out, "size", sz);

  bool hasDefault = false;
  for (const auto *c : st->constructors) {
    if (c->params.size() == 0)
      hasDefault = true;
  }
  napi_value hd;
  napi_get_boolean(env, hasDefault, &hd);
  napi_set_named_property(env, out, "hasDefaultCtor", hd);

  napi_value members;
  napi_create_array(env, &members);
  uint32_t i = 0;
  for (const auto &m : st->members) {
    napi_value mo;
    napi_create_object(env, &mo);
    napi_value n, t, off;
    napi_create_string_utf8(env, m.name.c_str(), NAPI_AUTO_LENGTH, &n);
    napi_create_string_utf8(env, bindingTypeName(m.type->type), NAPI_AUTO_LENGTH, &t);
    napi_create_uint32(env, static_cast<uint32_t>(m.offset), &off);
    napi_set_named_property(env, mo, "name", n);
    napi_set_named_property(env, mo, "type", t);
    napi_set_named_property(env, mo, "offset", off);
    napi_set_element(env, members, i++, mo);
  }
  napi_set_named_property(env, out, "members", members);

  napi_value methods;
  napi_create_array(env, &methods);
  uint32_t mi = 0;
  for (const types::Method *m : st->methods) {
    napi_value mo;
    napi_create_object(env, &mo);
    napi_value mn, mp, mr, ms;
    napi_create_string_utf8(env, m->name.c_str(), NAPI_AUTO_LENGTH, &mn);
    napi_create_uint32(env, static_cast<uint32_t>(m->params.size()), &mp);
    napi_create_string_utf8(env,
                            m->returnType ? bindingTypeName(m->returnType->type) : "void",
                            NAPI_AUTO_LENGTH,
                            &mr);
    napi_get_boolean(env, m->isStatic, &ms);
    napi_set_named_property(env, mo, "name", mn);
    napi_set_named_property(env, mo, "params", mp);
    napi_set_named_property(env, mo, "ret", mr);
    napi_set_named_property(env, mo, "static", ms);
    napi_set_element(env, methods, mi++, mo);
  }
  napi_set_named_property(env, out, "methods", methods);
  return out;
}

// construct(name) -> bound instance (owning). Throws if no default constructor.
napi_value NapiRuntime::Construct(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  void *data;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);

  char buf[512];
  size_t len = 0;
  if (argc >= 1)
    napi_get_value_string_utf8(env, argv[0], buf, sizeof(buf), &len);

  const binding::BindingBase *b = rt->lookup(buf);
  if (!b || b->type != BindingType::Struct) {
    napi_throw_error(env, nullptr, "construct: unknown struct type");
    return nullptr;
  }
  const types::_StructBase *st = static_cast<const types::_StructBase *>(b);

  const types::Constructor *ctor = nullptr;
  for (const auto *c : st->constructors) {
    if (c->params.size() == 0) {
      ctor = c;
      break;
    }
  }
  if (!ctor || !ctor->thunk) {
    napi_throw_error(env, nullptr, "construct: no default constructor");
    return nullptr;
  }

  void *bufobj = std::malloc(st->getSize());
  ctor->thunk(bufobj, nullptr);
  return rt->instantiate(st, bufobj, /*owning=*/true);
}

// constructWith(structName, ctorName, ...args) -> bound owning instance built
// with a *named, parameterized* constructor (e.g.
// CommandExecutor "main"(SpatialTree*, Brush*)). Marshals args via the shared
// thunk ABI (marshalArg); the constructor thunk reads them identically to a
// method thunk (ConstructorBuilder::invokeImpl). Up to 6 ctor args.
napi_value NapiRuntime::ConstructWith(napi_env env, napi_callback_info info)
{
  // Capacity must cover (structName, ctorName, ...ctorArgs); see methodInvoker.
  size_t argc = 32;
  napi_value argv[32];
  void *data;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);

  if (argc < 2) {
    napi_throw_error(env, nullptr, "constructWith: need (structName, ctorName, ...args)");
    return nullptr;
  }
  char structName[512] = {0}, ctorName[256] = {0};
  size_t len = 0;
  napi_get_value_string_utf8(env, argv[0], structName, sizeof(structName), &len);
  napi_get_value_string_utf8(env, argv[1], ctorName, sizeof(ctorName), &len);

  const binding::BindingBase *b = rt->lookup(structName);
  if (!b || b->type != BindingType::Struct) {
    napi_throw_error(env, nullptr, "constructWith: unknown struct type");
    return nullptr;
  }
  const types::_StructBase *st = static_cast<const types::_StructBase *>(b);
  const types::Constructor *ctor = st->findConstructor(ctorName);
  if (!ctor || !ctor->thunk) {
    napi_throw_error(env, nullptr, "constructWith: no such constructor");
    return nullptr;
  }

  const size_t nparams = ctor->params.size();
  std::vector<void *> args(nparams, nullptr);
  std::vector<uint64_t> slots(nparams, 0);
  for (size_t i = 0; i < nparams; i++) {
    // ctor args start at argv[2].
    napi_value a = (i + 2 < argc) ? argv[i + 2] : nullptr;
    args[i] = marshalArg(env, ctor->params[i].type, a, &slots[i]);
  }

  void *bufobj = std::malloc(st->getSize());
  ctor->thunk(bufobj, args.data());
  return rt->instantiate(st, bufobj, /*owning=*/true);
}

// makeNodeVector() -> a fresh owning, empty Vector<SpatialNode*>, ready to pass
// to SpatialTree.filterNodes(co, radius, &out) and then CommandExecutor.execBrush.
// The Vector<SpatialNode*> descriptor (bare name "litestl::util::Vector", shared
// across specializations) can't be looked up by element type, so we recover the
// exact specialization from SpatialTree::leaves()'s by-value return type and use
// its default constructor. instantiate(owning) runs ~Vector() on finalize.
napi_value NapiRuntime::MakeNodeVector(napi_env env, napi_callback_info info)
{
  void *data;
  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);
  napi_value out;
  napi_get_undefined(env, &out);

  const binding::BindingBase *tb = rt->lookup("sculptcore::spatial::SpatialTree");
  if (!tb || tb->type != BindingType::Struct)
    return out;
  const types::_StructBase *ts = static_cast<const types::_StructBase *>(tb);

  const types::Method *leaves = nullptr;
  for (const types::Method *m : ts->methods) {
    if (std::strcmp(m->name.c_str(), "leaves") == 0) {
      leaves = m;
      break;
    }
  }
  if (!leaves || !leaves->returnType || leaves->returnType->type != BindingType::Struct) {
    return out;
  }
  const types::_StructBase *vecSt =
      static_cast<const types::_StructBase *>(leaves->returnType);

  const types::Constructor *ctor = nullptr;
  for (const auto *c : vecSt->constructors) {
    if (c->params.size() == 0) {
      ctor = c;
      break;
    }
  }
  if (!ctor || !ctor->thunk)
    return out;

  void *bufobj = std::malloc(vecSt->getSize());
  ctor->thunk(bufobj, nullptr);
  return rt->instantiate(vecSt, bufobj, /*owning=*/true);
}

// makeIntVector() -> a fresh owning, empty Vector<int>, ready to pass as a
// faces/verts out-param to SpatialTree.castScreenCircle / castScreenRect. Same
// recovery trick as MakeNodeVector, but the Vector<int> specialization is
// recovered from castScreenCircle's out-param type (a Reference/Pointer to the
// Vector<int> Struct) since no method returns Vector<int> by value.
napi_value NapiRuntime::MakeIntVector(napi_env env, napi_callback_info info)
{
  void *data;
  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);
  napi_value out;
  napi_get_undefined(env, &out);

  const binding::BindingBase *tb = rt->lookup("sculptcore::spatial::SpatialTree");
  if (!tb || tb->type != BindingType::Struct)
    return out;
  const types::_StructBase *ts = static_cast<const types::_StructBase *>(tb);

  const types::Method *method = nullptr;
  for (const types::Method *m : ts->methods) {
    if (std::strcmp(m->name.c_str(), "castScreenCircle") == 0) {
      method = m;
      break;
    }
  }
  if (!method)
    return out;

  // Find a param resolving to a Vector struct (faces/verts are Vector<int>&).
  const types::_StructBase *vecSt = nullptr;
  for (const auto &param : method->params) {
    const binding::BindingBase *t = param.type;
    if (t && t->type == BindingType::Pointer) {
      t = static_cast<const types::Pointer *>(t)->ptrType;
    } else if (t && t->type == BindingType::Reference) {
      t = static_cast<const types::Reference *>(t)->refType;
    }
    if (t && t->type == BindingType::Struct) {
      const types::_StructBase *st = static_cast<const types::_StructBase *>(t);
      if (std::strcmp(st->name.c_str(), "litestl::util::Vector") == 0) {
        vecSt = st;
        break;
      }
    }
  }
  if (!vecSt)
    return out;

  const types::Constructor *ctor = nullptr;
  for (const auto *c : vecSt->constructors) {
    if (c->params.size() == 0) {
      ctor = c;
      break;
    }
  }
  if (!ctor || !ctor->thunk)
    return out;

  void *bufobj = std::malloc(vecSt->getSize());
  ctor->thunk(bufobj, nullptr);
  return rt->instantiate(vecSt, bufobj, /*owning=*/true);
}

// makeFloatVector() -> a fresh owning, empty Vector<float>, ready to pass as
// the out-param of Mesh.edgePathCoords. Same recovery trick as MakeIntVector:
// the Vector<float> specialization is recovered from edgePathCoords' out-param
// type since no method returns Vector<float> by value.
napi_value NapiRuntime::MakeFloatVector(napi_env env, napi_callback_info info)
{
  void *data;
  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);
  napi_value out;
  napi_get_undefined(env, &out);

  const binding::BindingBase *tb = rt->lookup("sculptcore::mesh::Mesh");
  if (!tb || tb->type != BindingType::Struct)
    return out;
  const types::_StructBase *ts = static_cast<const types::_StructBase *>(tb);

  const types::Method *method = nullptr;
  for (const types::Method *m : ts->methods) {
    if (std::strcmp(m->name.c_str(), "edgePathCoords") == 0) {
      method = m;
      break;
    }
  }
  if (!method)
    return out;

  // Find the param resolving to a Vector struct (out is Vector<float>&).
  const types::_StructBase *vecSt = nullptr;
  for (const auto &param : method->params) {
    const binding::BindingBase *t = param.type;
    if (t && t->type == BindingType::Pointer) {
      t = static_cast<const types::Pointer *>(t)->ptrType;
    } else if (t && t->type == BindingType::Reference) {
      t = static_cast<const types::Reference *>(t)->refType;
    }
    if (t && t->type == BindingType::Struct) {
      const types::_StructBase *st = static_cast<const types::_StructBase *>(t);
      if (std::strcmp(st->name.c_str(), "litestl::util::Vector") == 0) {
        vecSt = st;
        break;
      }
    }
  }
  if (!vecSt)
    return out;

  const types::Constructor *ctor = nullptr;
  for (const auto *c : vecSt->constructors) {
    if (c->params.size() == 0) {
      ctor = c;
      break;
    }
  }
  if (!ctor || !ctor->thunk)
    return out;

  void *bufobj = std::malloc(vecSt->getSize());
  ctor->thunk(bufobj, nullptr);
  return rt->instantiate(vecSt, bufobj, /*owning=*/true);
}

// ---------------------------------------------------------------------------
// Bulk-data fast path / minimal Vector surface.
// litestl::util::Vector layout (native): T* data_ @0, size_t size_ @8.
// ---------------------------------------------------------------------------
static const size_t kVecDataOffset = 0;
static const size_t kVecSizeOffset = sizeof(void *);

static bool isVectorStruct(const types::_StructBase *st)
{
  // strcmp avoids the ambiguous util::string == const char* overload.
  return st && std::strcmp(st->name.c_str(), "litestl::util::Vector") == 0 &&
         st->templateParams.size() >= 1;
}

// Map a Number element descriptor to a typed-array kind; false for non-numbers.
static bool numberTypedArrayKind(const binding::BindingBase *elem,
                                 napi_typedarray_type *out)
{
  if (!elem || elem->type != BindingType::Number)
    return false;
  const types::Number<char> *n = asNumber(elem);
  const bool u = isUnsigned(n);
  switch (n->subtype) {
  case NumberType::Int8:
    *out = u ? napi_uint8_array : napi_int8_array;
    return true;
  case NumberType::Int16:
    *out = u ? napi_uint16_array : napi_int16_array;
    return true;
  case NumberType::Int32:
    *out = u ? napi_uint32_array : napi_int32_array;
    return true;
  case NumberType::Int64:
    *out = u ? napi_biguint64_array : napi_bigint64_array;
    return true;
  case NumberType::Float32:
    *out = napi_float32_array;
    return true;
  case NumberType::Float64:
    *out = napi_float64_array;
    return true;
  }
  return false;
}

static Wrapped *unwrapVector(napi_env env, size_t argc, napi_value *argv)
{
  Wrapped *w = nullptr;
  if (argc < 1 || napi_unwrap(env, argv[0], reinterpret_cast<void **>(&w)) != napi_ok) {
    return nullptr;
  }
  return (w && isVectorStruct(w->st)) ? w : nullptr;
}

napi_value NapiRuntime::VectorLength(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  Wrapped *w = unwrapVector(env, argc, argv);
  if (!w) {
    napi_get_undefined(env, &out);
    return out;
  }
  size_t count =
      *reinterpret_cast<size_t *>(static_cast<char *>(w->ptr) + kVecSizeOffset);
  napi_create_uint32(env, static_cast<uint32_t>(count), &out);
  return out;
}

// vectorView(vec) -> a typed array over the Vector's contiguous storage (the
// bulk-data fast path: zero per-element napi calls). External ArrayBuffer, no
// finalizer — the C++ Vector owns the memory, so the caller must keep the bound
// Vector alive while the view is in use.
napi_value NapiRuntime::VectorView(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  Wrapped *w = unwrapVector(env, argc, argv);
  if (!w) {
    napi_get_undefined(env, &out);
    return out;
  }
  void *dataPtr =
      *reinterpret_cast<void **>(static_cast<char *>(w->ptr) + kVecDataOffset);
  size_t count =
      *reinterpret_cast<size_t *>(static_cast<char *>(w->ptr) + kVecSizeOffset);
  const binding::BindingBase *elem = w->st->templateParams[0].type;
  size_t elemSize = elem ? elem->getSize() : 0;
  if (!dataPtr || elemSize == 0) {
    napi_get_undefined(env, &out);
    return out;
  }

  const size_t byteLen = count * elemSize;
  napi_value ab;
  // Prefer a true zero-copy external ArrayBuffer over the C++ storage. Electron
  // enables the V8 sandbox, which forbids ArrayBuffers backed by memory outside
  // the sandbox (returns napi_no_external_buffers_allowed) — so fall back to a
  // one-shot copy into a sandbox-internal ArrayBuffer. Still O(1) napi calls per
  // buffer (one memcpy) vs O(n) per-element getters.
  napi_status st =
      napi_create_external_arraybuffer(env, dataPtr, byteLen, nullptr, nullptr, &ab);
  if (st != napi_ok) {
    bool pending = false;
    napi_is_exception_pending(env, &pending);
    if (pending) {
      napi_value e;
      napi_get_and_clear_last_exception(env, &e);
    }
    void *abData = nullptr;
    napi_create_arraybuffer(env, byteLen, &abData, &ab);
    if (abData)
      std::memcpy(abData, dataPtr, byteLen);
  }

  napi_typedarray_type ta;
  if (numberTypedArrayKind(elem, &ta)) {
    napi_create_typedarray(env, ta, count, ab, 0, &out);
  } else {
    napi_create_typedarray(env, napi_uint8_array, count * elemSize, ab, 0, &out);
  }
  return out;
}

// pointerBytes(boundObj, memberName, byteLen, byteOffset?) -> a Uint8Array over
// the bytes a raw-pointer member points at, starting `byteOffset` (default 0)
// bytes in. The native equivalent of the WASM
// `new Uint8Array(HEAPU8.buffer, buf.data + off, n)` bulk-data read
// (gpuExecutor.ts): the pointer (e.g. gpu::Buffer.data, a void*) deliberately
// never crosses into JS as a number, so C++ reads it off the descriptor here
// and views it. Same external->copy fallback as VectorView (V8 sandbox forbids
// external buffers in Electron). The C++ object owns the storage; the caller
// must keep the bound object alive while the view is used.
napi_value NapiRuntime::PointerBytes(napi_env env, napi_callback_info info)
{
  size_t argc = 4;
  napi_value argv[4];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);
  if (argc < 3)
    return out;

  Wrapped *w = nullptr;
  if (napi_unwrap(env, argv[0], reinterpret_cast<void **>(&w)) != napi_ok || !w ||
      !w->ptr || !w->st)
  {
    return out;
  }

  char nameBuf[128] = {};
  size_t nameLen = 0;
  if (napi_get_value_string_utf8(env, argv[1], nameBuf, sizeof(nameBuf), &nameLen) !=
      napi_ok)
  {
    return out;
  }
  double dBytes = 0;
  napi_get_value_double(env, argv[2], &dBytes);
  const size_t byteLen = static_cast<size_t>(dBytes);
  if (byteLen == 0)
    return out;

  // Resolve the member offset by name from the descriptor (same source the
  // member accessors use), then read the pointer field directly.
  const types::StructMember *found = nullptr;
  for (const auto &m : w->st->members) {
    if (std::strcmp(m.name.c_str(), nameBuf) == 0) {
      found = &m;
      break;
    }
  }
  if (!found)
    return out;

  void *dataPtr = *reinterpret_cast<void **>(static_cast<char *>(w->ptr) + found->offset);
  if (!dataPtr)
    return out;

  if (argc >= 4) {
    double dOff = 0;
    if (napi_get_value_double(env, argv[3], &dOff) == napi_ok && dOff > 0) {
      dataPtr = static_cast<char *>(dataPtr) + static_cast<size_t>(dOff);
    }
  }

  napi_value ab;
  napi_status st =
      napi_create_external_arraybuffer(env, dataPtr, byteLen, nullptr, nullptr, &ab);
  if (st != napi_ok) {
    bool pending = false;
    napi_is_exception_pending(env, &pending);
    if (pending) {
      napi_value e;
      napi_get_and_clear_last_exception(env, &e);
    }
    void *abData = nullptr;
    napi_create_arraybuffer(env, byteLen, &abData, &ab);
    if (abData)
      std::memcpy(abData, dataPtr, byteLen);
  }
  napi_create_typedarray(env, napi_uint8_array, byteLen, ab, 0, &out);
  return out;
}

// objectAddress(boundObj) -> the wrapped C++ object's address as a JS number, an
// *opaque identity key* only (e.g. gpuExecutor's per-Buffer GL-buffer cache).
// It is never dereferenced in JS; the address is < 2^48 on win64 so a double is
// exact. WASM uses the numeric `.ptr` for the same purpose.
napi_value NapiRuntime::ObjectAddress(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);
  if (argc < 1)
    return out;
  Wrapped *w = nullptr;
  if (napi_unwrap(env, argv[0], reinterpret_cast<void **>(&w)) != napi_ok || !w)
    return out;
  napi_create_double(env, static_cast<double>(reinterpret_cast<uintptr_t>(w->ptr)), &out);
  return out;
}

// ---------------------------------------------------------------------------
// Native factory free-functions: create/operate on real engine objects and
// hand JS bound wrappers. The engine owns the returned objects (non-owning
// wrappers); SpatialTree is freed via spatialTreeFree and Mesh via meshFree
// (both extern "C" disposers — the binding system's GC finalizer only frees
// `owning` wrappers, and these factory wrappers are non-owning).
// ---------------------------------------------------------------------------
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

// spatialTreeFillDetailCarrier(tree, carrier) -> void. Tags every live face's
// `.detail.carrier` (0 = GEOM, 1 = VDM) — the V3 harness whole-mesh fill.
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

// vectorGet(vec, i) — i-th element as a bound value/wrapper, via getBoundPointer
// on the element's storage. Enables iteration of a bound Vector (what the
// getBoundVector use site in sculptcore_ops needs).
napi_value NapiRuntime::VectorGet(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  void *data;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  NapiRuntime *rt = static_cast<NapiRuntime *>(data);
  napi_value out;
  Wrapped *w = unwrapVector(env, argc, argv);
  if (!w) {
    napi_get_undefined(env, &out);
    return out;
  }
  int32_t i = -1;
  if (argc >= 2)
    napi_get_value_int32(env, argv[1], &i);
  void *dataPtr =
      *reinterpret_cast<void **>(static_cast<char *>(w->ptr) + kVecDataOffset);
  size_t count =
      *reinterpret_cast<size_t *>(static_cast<char *>(w->ptr) + kVecSizeOffset);
  const binding::BindingBase *elem = w->st->templateParams[0].type;
  size_t elemSize = elem ? elem->getSize() : 0;
  if (i < 0 || static_cast<size_t>(i) >= count || !dataPtr || elemSize == 0) {
    napi_get_undefined(env, &out);
    return out;
  }
  void *elemAddr = static_cast<char *>(dataPtr) + static_cast<size_t>(i) * elemSize;
  return rt->getBoundPointer(elem, elemAddr);
}

// intVectorAssign(vec, data) — replace a Vector<int>'s contents from a JS
// array. The JS->C++ direction the seam otherwise lacks: bound Vector params
// are out-params only, so an app that computed an index set had no way to hand
// it back. Delegates to binding.cc's IntVector_assign so the Vector's own
// clear/reserve/append run -- writing the size field from here would overrun
// capacity and trample its inline storage.
napi_value NapiRuntime::IntVectorAssign(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);

  Wrapped *w = unwrapVector(env, argc, argv);
  if (!w || argc < 2) {
    return out;
  }

  bool isArray = false;
  napi_is_array(env, argv[1], &isArray);
  if (!isArray) {
    return out;
  }

  uint32_t count = 0;
  napi_get_array_length(env, argv[1], &count);

  litestl::util::Vector<int> scratch;
  scratch.ensure_capacity(size_t(count));
  for (uint32_t i = 0; i < count; i++) {
    napi_value elem;
    int32_t v = 0;
    if (napi_get_element(env, argv[1], i, &elem) == napi_ok) {
      napi_get_value_int32(env, elem, &v);
    }
    scratch.append(v);
  }

  IntVector_assign(reinterpret_cast<litestl::util::Vector<int> *>(w->ptr),
                   count ? &scratch[0] : nullptr,
                   int(count));
  return out;
}

// floatVectorAssign(vec, data) — IntVectorAssign for Vector<float>. Same
// delegate-to-binding.cc discipline, so the Vector's own clear/reserve/append
// run rather than the size field being written from here.
napi_value NapiRuntime::FloatVectorAssign(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);

  Wrapped *w = unwrapVector(env, argc, argv);
  if (!w || argc < 2) {
    return out;
  }

  bool isArray = false;
  napi_is_array(env, argv[1], &isArray);
  if (!isArray) {
    return out;
  }

  uint32_t count = 0;
  napi_get_array_length(env, argv[1], &count);

  litestl::util::Vector<float> scratch;
  scratch.ensure_capacity(size_t(count));
  for (uint32_t i = 0; i < count; i++) {
    napi_value el;
    napi_get_element(env, argv[1], i, &el);
    double v = 0.0;
    napi_get_value_double(env, el, &v);
    scratch.append(float(v));
  }

  FloatVector_assign(reinterpret_cast<litestl::util::Vector<float> *>(w->ptr),
                     count ? &scratch[0] : nullptr,
                     int(count));
  return out;
}

// ---------------------------------------------------------------------------
// litestl allocator introspection (binding.cc LSTL_*).
// ---------------------------------------------------------------------------
// getMemSize(includePermanent) -> tracked allocation size in bytes. size_t is
// returned as a double (exact below 2^53, so fine for any realistic heap).
napi_value NapiRuntime::GetMemSize(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool includePermanent = false;
  if (argc >= 1)
    napi_get_value_bool(env, argv[0], &includePermanent);
  napi_value out;
  napi_create_double(env, static_cast<double>(LSTL_GetMemSize(includePermanent)), &out);
  return out;
}

// printAllocBlocks(includePermanent) -> void. Dumps every live block to the log
// sink (the renderer DevTools console, via consoleSink).
napi_value NapiRuntime::PrintAllocBlocks(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool includePermanent = false;
  if (argc >= 1)
    napi_get_value_bool(env, argv[0], &includePermanent);
  LSTL_PrintAllocBlocks(includePermanent);
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// formatBlock(boundObj) -> a string describing the allocation backing the
// wrapped C++ object. LSTL_FormatBlock returns a raw-heap char* the caller must
// release; we copy it into a JS string and free it with LSTL_FreeFormatBlocks
// (the char* deliberately never crosses into JS as a number).
napi_value NapiRuntime::FormatBlock(napi_env env, napi_callback_info info)
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
  char *s = LSTL_FormatBlock(w->ptr);
  if (!s)
    return out;
  napi_create_string_utf8(env, s, NAPI_AUTO_LENGTH, &out);
  LSTL_FreeFormatBlocks(s);
  return out;
}

// formatBlocks(printPermanent) -> a string describing every live allocation
// block (the whole-heap counterpart of formatBlock). Same raw-heap-string /
// LSTL_FreeFormatBlocks release pattern.
napi_value NapiRuntime::FormatBlocks(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);

  bool printPermanent = false;
  if (argc >= 1)
    napi_get_value_bool(env, argv[0], &printPermanent);
  char *s = LSTL_FormatBlocks(printPermanent);
  if (!s)
    return out;
  napi_create_string_utf8(env, s, NAPI_AUTO_LENGTH, &out);
  LSTL_FreeFormatBlocks(s);
  return out;
}

// testPrint(msg?) -> void. Writes the (optional) message to the process stdout
// directly from C++ (not the console.log sink) and flushes, so a headless test
// can assert whether native-side stdout reaches the launched NW.js process's
// captured output. Defaults to a fixed marker when called with no argument.
napi_value NapiRuntime::TestPrint(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value undef;
  napi_get_undefined(env, &undef);

  std::string msg = "[sculptcore::testPrint] native stdout OK";
  if (argc >= 1) {
    napi_valuetype t = napi_undefined;
    napi_typeof(env, argv[0], &t);
    if (t == napi_string) {
      size_t len = 0;
      napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
      std::vector<char> buf(len + 1, 0);
      napi_get_value_string_utf8(env, argv[0], buf.data(), len + 1, &len);
      msg.assign(buf.data(), len);
    }
  }

  std::fputs(msg.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
  return undef;
}

// crashTest() -> void. Crashpad self-test: derefs a null pointer to fault inside
// sculptcore_node, so a minidump carries a native stack symbolicated by the
// addon's CodeView PDB. Driven by the harness --apptest-crash flag; see
// documentation/plans/crashpad.md.
napi_value NapiRuntime::CrashTest(napi_env env, napi_callback_info info)
{
  (void)info;
  volatile int *sculptcoreCrashTestNullDeref = nullptr;
  *sculptcoreCrashTestNullDeref = 0xC0FFEE;
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// redirectStdout(path) -> boolean. freopen()s the C stdout stream onto `path`
// (unbuffered). The NW.js renderer starts with fd 0/1/2 closed (EBADF), so a
// plain printf is written to a dead fd and lost; pointing stdout at a launcher-
// supplied file gives later TestPrint output a real destination the wrapper
// reads back — the standard Windows GUI-subsystem stdout workaround.
napi_value NapiRuntime::RedirectStdout(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  bool ok = false;
  if (argc >= 1) {
    size_t len = 0;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    std::vector<char> buf(len + 1, 0);
    napi_get_value_string_utf8(env, argv[0], buf.data(), len + 1, &len);
    FILE *f = std::freopen(buf.data(), "w", stdout);
    if (f) {
      std::setvbuf(stdout, nullptr, _IONBF, 0);
      ok = true;
    }
  }
  napi_get_boolean(env, ok, &out);
  return out;
}

void NapiRuntime::define(napi_value exports, const char *name, napi_callback cb)
{
  napi_value fn;
  napi_create_function(env_, name, NAPI_AUTO_LENGTH, cb, this, &fn);
  napi_set_named_property(env_, exports, name, fn);
}

// --- GPU brush-stroke seam (gpu_brush_c_api.cc) ------------------------------
// The session pointer crosses as a napi external: opaque, never dereferenced by
// JS, lifetime owned by GpuBrush_endStroke/GpuBrush_free (no GC finalizer — a
// leaked session is a bug the debug surface should expose, not silently free).

namespace {

void *externalArg(napi_env env, napi_value v)
{
  void *p = nullptr;
  if (napi_get_value_external(env, v, &p) != napi_ok) {
    return nullptr;
  }
  return p;
}

// Borrow a Float32Array argument's backing store; null for non-typed-array
// args (undefined/null cross as "absent").
const float *floatsArg(napi_env env, napi_value v, size_t *outLen)
{
  *outLen = 0;
  bool isTa = false;
  napi_is_typedarray(env, v, &isTa);
  if (!isTa) {
    return nullptr;
  }
  napi_typedarray_type type;
  size_t len = 0;
  void *data = nullptr;
  if (napi_get_typedarray_info(env, v, &type, &len, &data, nullptr, nullptr) != napi_ok ||
      type != napi_float32_array)
  {
    return nullptr;
  }
  *outLen = len;
  return static_cast<const float *>(data);
}

} // namespace

napi_value NapiRuntime::GpuBrushBeginStroke(napi_env env, napi_callback_info info)
{
  size_t argc = 5;
  napi_value argv[5];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);
  if (argc < 5) {
    return out;
  }
  Wrapped *mw = nullptr, *tw = nullptr, *bw = nullptr, *lw = nullptr;
  if (napi_unwrap(env, argv[0], reinterpret_cast<void **>(&mw)) != napi_ok || !mw ||
      napi_unwrap(env, argv[1], reinterpret_cast<void **>(&tw)) != napi_ok || !tw ||
      napi_unwrap(env, argv[2], reinterpret_cast<void **>(&bw)) != napi_ok || !bw ||
      napi_unwrap(env, argv[3], reinterpret_cast<void **>(&lw)) != napi_ok || !lw)
  {
    return out;
  }
  int32_t tool = 0;
  napi_get_value_int32(env, argv[4], &tool);
  void *s = GpuBrush_beginStroke(mw->ptr, tw->ptr, bw->ptr, lw->ptr, tool);
  if (!s) {
    return out;
  }
  napi_create_external(env, s, nullptr, nullptr, &out);
  return out;
}

napi_value NapiRuntime::GpuBrushFree(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc >= 1) {
    GpuBrush_free(externalArg(env, argv[0]));
  }
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value NapiRuntime::GpuBrushKernelName(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  const char *name = argc >= 1 ? GpuBrush_kernelName(externalArg(env, argv[0])) : "";
  napi_value out;
  napi_create_string_utf8(env, name ? name : "", NAPI_AUTO_LENGTH, &out);
  return out;
}

napi_value NapiRuntime::GpuBrushInfo(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t which = 0;
  int v = 0;
  if (argc >= 2) {
    napi_get_value_int32(env, argv[1], &which);
    v = GpuBrush_info(externalArg(env, argv[0]), which);
  }
  napi_value out;
  napi_create_int32(env, v, &out);
  return out;
}

napi_value NapiRuntime::GpuBrushMarshalDab(napi_env env, napi_callback_info info)
{
  size_t argc = 11;
  napi_value argv[11];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_create_int32(env, 0, &out);
  if (argc < 11) {
    return out;
  }
  void *s = externalArg(env, argv[0]);
  double f[8] = {};
  for (int i = 0; i < 8; i++) {
    napi_get_value_double(env, argv[1 + i], &f[i]);
  }
  int32_t mirrorIdx = 0, nonaccum = 0;
  napi_get_value_int32(env, argv[9], &mirrorIdx);
  napi_get_value_int32(env, argv[10], &nonaccum);
  int n = GpuBrush_marshalDab(s,
                              float(f[0]),
                              float(f[1]),
                              float(f[2]),
                              float(f[3]),
                              float(f[4]),
                              float(f[5]),
                              float(f[6]),
                              float(f[7]),
                              mirrorIdx,
                              nonaccum);
  napi_create_int32(env, n, &out);
  return out;
}

napi_value NapiRuntime::GpuBrushData(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value out;
  napi_get_undefined(env, &out);
  if (argc < 2) {
    return out;
  }
  void *s = externalArg(env, argv[0]);
  int32_t which = 0;
  napi_get_value_int32(env, argv[1], &which);
  const int size = GpuBrush_dataSize(s, which);
  const void *src = GpuBrush_dataPtr(s, which);
  // Never throw on the bulk-data seam: an empty blob crosses as a 0-length view.
  const size_t n = (size > 0 && src) ? size_t(size) : 0;
  napi_value ab;
  void *abData = nullptr;
  napi_create_arraybuffer(env, n, &abData, &ab);
  if (abData && n) {
    std::memcpy(abData, src, n);
  }
  napi_create_typedarray(env, napi_uint8_array, n, ab, 0, &out);
  return out;
}

napi_value NapiRuntime::GpuBrushApplyCo(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value undef;
  napi_get_undefined(env, &undef);
  if (argc < 2) {
    return undef;
  }
  size_t len = 0;
  const float *co = floatsArg(env, argv[1], &len);
  if (co && len % 3 == 0) {
    GpuBrush_applyCo(externalArg(env, argv[0]), co, int(len / 3));
  }
  return undef;
}

napi_value NapiRuntime::GpuBrushEndStroke(napi_env env, napi_callback_info info)
{
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value undef;
  napi_get_undefined(env, &undef);
  if (argc < 1) {
    return undef;
  }
  size_t coLen = 0, noLen = 0;
  const float *co = argc >= 2 ? floatsArg(env, argv[1], &coLen) : nullptr;
  const float *no = argc >= 3 ? floatsArg(env, argv[2], &noLen) : nullptr;
  (void)noLen;
  GpuBrush_endStroke(externalArg(env, argv[0]), co, no, int(coLen / 3));
  return undef;
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
