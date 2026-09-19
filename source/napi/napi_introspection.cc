// Method/constructor invocation and the reflection exports built on it
// (version, bindingCount, structNames, structInfo, construct, constructWith,
// the makeXVector out-param helpers). napi_object_model.cc covers the class
// building / member accessor half this is layered on.

#include "napi_runtime.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "napi_util.h"

#include "litestl/binding/binding_constructor.h"
#include "litestl/binding/binding_method.h"
#include "litestl/binding/binding_number.h"
#include "litestl/binding/binding_types.h"

namespace sculptcore::napi {

using binding::BindingType;

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

} // namespace sculptcore::napi
