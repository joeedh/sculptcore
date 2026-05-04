#pragma once

#include "binding/binding_literal.h"
#include "math/vector.h"

#include "util/alloc.h"
#include "util/boolvector.h"
#include "util/compiler_util.h"
#include "util/map.h"
#include "util/string.h"
#include "util/vector.h"

#include "litestl/binding/binding.h"

#include "gpu/types.h"
using namespace litestl;

namespace sculptcore::gpu {
using litestl::util::string;
using litestl::util::stringref;

enum class _UniformType {
  FLOAT = 0,
  DOUBLE = 1,
  BYTE = 2,
  UBYTE = 3,
  SHORT = 4,
  USHORT = 5,
  INT = 6,
  UINT = 7,
  FLOAT2 = 8,
  FLOAT3 = 9,
  FLOAT4 = 10,
  INT2 = 11,
  INT3 = 12,
  INT4 = 13,
  UINT2 = 14,
  UINT3 = 15,
  UINT4 = 16,
  DOUBLE2 = 17,
  DOUBLE3 = 18,
  DOUBLE4 = 19,
  SHORT2 = 20,
  SHORT3 = 21,
  SHORT4 = 22,
  USHORT2 = 23,
  USHORT3 = 24,
  USHORT4 = 25,
  BYTE2 = 26,
  BYTE3 = 27,
  BYTE4 = 28,
  UBYTE2 = 29,
  UBYTE3 = 30,
  UBYTE4 = 31
};
MAKE_ENUM_CLASS(UniformType, _UniformType, int32_t);

struct UniformDefBase {
  string name;
  GPUType type;
  int elemSize;

  UniformDefBase(string name, GPUType type, int elemSize)
      : name(name), type(type), elemSize(elemSize)
  {
  }
};

template <typename T> struct UniformDef : public UniformDefBase {
  T defaultValue;

  UniformDef(string name, GPUType type, int elemSize, T defaultValue)
      : UniformDefBase{name, type, elemSize}, defaultValue(defaultValue)
  {
  }

  static litestl::binding::types::Struct<UniformDef> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<UniformDef> *st =
        new types::Struct<UniformDef>("sculptcore::gpu::UniformDef", sizeof(UniformDef));
    // st->addTempMember("T", Bind<T>());
    st->addTemplateParam(Bind<T>(), "T");
    BIND_STRUCT_MEMBER(st, name);
    BIND_STRUCT_MEMBER(st, type);
    BIND_STRUCT_MEMBER(st, elemSize);
    BIND_STRUCT_MEMBER(st, defaultValue);
    return st;
  }
};

struct AttrDef {
  string name;
  GPUType type;
  int elemSize;

  static litestl::binding::types::Struct<AttrDef> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<AttrDef> *st =
        new types::Struct<AttrDef>("sculptcore::gpu::AttrDef", sizeof(AttrDef));
    BIND_STRUCT_MEMBER(st, name);
    BIND_STRUCT_MEMBER(st, type);
    BIND_STRUCT_MEMBER(st, elemSize);
    return st;
  }
};

struct ShaderDefDefine {
  string key;
  string value;
  bool isSetByDefault;
};

struct ShaderDef {
  string name;
  string vertexSource, fragmentSource;
  util::Vector<AttrDef> attrs;
  util::Vector<UniformDefBase *> uniforms;
  util::Map<string, string> defines;

  ShaderDef()
  {
  }
  ShaderDef(const ShaderDef &b) = default;
  ShaderDef &operator=(const ShaderDef &b) = default;
  ShaderDef(string name,
            string vertexSource,
            string fragmentSource,
            util::Vector<AttrDef> attrs,
            util::Vector<UniformDefBase *> uniforms,
            util::Vector<ShaderDefDefine> defines)
  {
    this->name = name;
    this->vertexSource = vertexSource;
    this->fragmentSource = fragmentSource;
    this->attrs = attrs;
    this->uniforms = uniforms;

    for (auto &item : defines) {
      if (!item.isSetByDefault) {
        continue;
      }
      this->defines[item.key] = item.value;
    }
  }

  ~ShaderDef()
  {
    for (auto *uniform : uniforms) {
      alloc::Delete(uniform);
    }
  }

  static litestl::binding::types::Struct<ShaderDef> *defineBindings()
  {
    using namespace litestl::binding;
    using litestl::util::Vector;

    /* ShaderDef bindings deferred — opaque struct for now. */
    types::Struct<ShaderDef> *st =
        new types::Struct<ShaderDef>("sculptcore::gpu::ShaderDef", sizeof(ShaderDef));

    BIND_STRUCT_MEMBER(st, name);
    BIND_STRUCT_MEMBER(st, vertexSource);
    BIND_STRUCT_MEMBER(st, fragmentSource);
    BIND_STRUCT_MEMBER(st, attrs);
    //  deal with defines later
    //  BIND_STRUCT_MEMBER(st, defines);

#if 0 // TODO afterm more cleanup in union code
    // build type union of uniformDef
    types::Union *unionType = new types::Union("type", Bind<GPUType>());
    unionType->add("FLOAT32",
                   GPUType::FLOAT32,
                   static_cast<const types::_StructBase *>(Bind<UniformDef<float>>()));
    unionType->add("FLOAT64",
                   GPUType::FLOAT64,
                   static_cast<const types::_StructBase *>(Bind<UniformDef<double>>()));
    unionType->add("INT8",
                   GPUType::INT8,
                   static_cast<const types::_StructBase *>(Bind<UniformDef<int8_t>>()));
    unionType->add("UINT8",
                   GPUType::UINT8,
                   static_cast<const types::_StructBase *>(Bind<UniformDef<uint8_t>>()));
    // build uniformDef pointer vector
    types::Pointer *ptrType = new types::Pointer(unionType);
    ptrType->isNonNull = true;

    types::Struct<Vector<UniformDefBase *>> *vecSt =
        new types::Struct<Vector<UniformDefBase *>>("sculptcore::util::Vector",
                                                    sizeof(Vector<UniformDefBase *>));

    // vector type
    vecSt->addTemplateParam(ptrType, "T");

    // vector static size
    vecSt->addTemplateParam(
        new types::NumLitType(
            decltype(ShaderDef::uniforms)::staticSize, "N", Bind<int>()),
        "N");

    st->add("uniforms", offsetof(ShaderDef, uniforms), vecSt);

#endif

    return st;
  }
};
} // namespace sculptcore::gpu