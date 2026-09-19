// JS class construction, per-instance wrapping, and member/array element
// accessors — the object-model half of the N-API reflection runtime.
// napi_introspection.cc covers method/constructor invocation and the
// version/structInfo/construct exports built on top of this.

#include "napi_runtime.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "napi_util.h"

#include "litestl/binding/binding_method.h"
#include "litestl/binding/binding_number.h"
#include "litestl/binding/binding_types.h"

namespace sculptcore::napi {

using binding::BindingType;
using binding::NumberType;

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

const binding::BindingBase *NapiRuntime::lookup(const char *name) const
{
  const auto &m = mgr_->getBindings();
  binding::string key(name);
  if (!m.contains(key))
    return nullptr;
  return m.lookup(key);
}

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

} // namespace sculptcore::napi
