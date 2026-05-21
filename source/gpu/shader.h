#pragma once

#include "binding/binding_literal.h"
#include "math/math_bindings.h"
#include "math/vector.h"

#include "util/alloc.h"
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

enum class _UniformBindType {
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
MAKE_ENUM_CLASS(UniformBindType, _UniformBindType, int32_t);

namespace detail {

extern litestl::binding::types::Enum *uniformBindTypeEnum;
extern litestl::binding::types::Union *uniformBindTypeUnion;
} // namespace detail

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
    st->add("defaultValue",
            offsetof(UniformDef, defaultValue),
            new types::ParentTemplateParam(
                "T", 0, Bind<T>(), detail::uniformBindTypeEnum));
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
  string wgslSource;
  util::Vector<AttrDef> attrs;
  util::Vector<UniformDefBase *> uniforms;
  util::Map<string, string> defines;

  /* Pre-compiled SPIR-V module containing all entry points from the WGSL
   * source (typically `vs_main` + `fs_main`). Set by the shader-owning
   * module when constructing a ShaderDef on native; null on WASM where the
   * WebGPU backend consumes wgslSource directly. Points at static
   * constexpr data emitted by tools/wgsl-to-spirv.mjs. */
  const uint32_t *spirv = nullptr;
  size_t spirvSize = 0;

  ShaderDef()
  {
  }
  /* uniforms is a vector of heap-owned pointers that the destructor frees,
   * so copying would alias the owned pointers and cause a double-free.
   * Move-only; consumers always reference ShaderDefs through pointers. */
  ShaderDef(const ShaderDef &) = delete;
  ShaderDef &operator=(const ShaderDef &) = delete;
  ShaderDef(ShaderDef &&b) noexcept
      : name(std::move(b.name)),
        wgslSource(std::move(b.wgslSource)),
        attrs(std::move(b.attrs)),
        uniforms(std::move(b.uniforms)),
        defines(std::move(b.defines)),
        spirv(b.spirv),
        spirvSize(b.spirvSize)
  {
    b.spirv = nullptr;
    b.spirvSize = 0;
  }
  ShaderDef &operator=(ShaderDef &&b) noexcept
  {
    if (this == &b) return *this;
    this->~ShaderDef();
    new (static_cast<void *>(this)) ShaderDef(std::move(b));
    return *this;
  }
  ShaderDef(string name,
            string wgslSource,
            util::Vector<AttrDef> attrs,
            util::Vector<UniformDefBase *> uniforms,
            util::Vector<ShaderDefDefine> defines)
      : name(std::move(name)),
        wgslSource(std::move(wgslSource)),
        attrs(std::move(attrs)),
        uniforms(std::move(uniforms))
  {
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
    BIND_STRUCT_MEMBER(st, wgslSource);
    BIND_STRUCT_MEMBER(st, attrs);

    //  deal with defines later
    //  BIND_STRUCT_MEMBER(st, defines);

    using namespace litestl::math;

    // build uniformDef pointer vector
    types::Pointer *ptrType = new types::Pointer(detail::uniformBindTypeUnion);
    ptrType->isNonNull = true;

    types::Struct<Vector<UniformDefBase *>> *vecSt =
        new types::Struct<Vector<UniformDefBase *>>("litestl::util::Vector",
                                                    sizeof(Vector<UniformDefBase *>));

    // vector type
    vecSt->addTemplateParam(ptrType, "T");

    // vector static size
    vecSt->addTemplateParam(
        new types::NumLitType(
            decltype(ShaderDef::uniforms)::staticSize, "N", Bind<int>()),
        "N");

    st->add("uniforms", offsetof(ShaderDef, uniforms), vecSt);
    return st;
  }
};
} // namespace sculptcore::gpu