#pragma once

#include "binding/binding_literal.h"
#include "binding/binding_struct.h"
#include "math/math_bindings.h"
#include "math/vector.h"

#include "util/alloc.h"
#include "util/compiler_util.h"
#include "util/map.h"
#include "util/string.h"
#include "util/vector.h"

#include "litestl/binding/binding.h"

#include "gpu/types.h"

#include <cstring>
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
    st->add(
        "defaultValue",
        offsetof(UniformDef, defaultValue),
        new types::ParentTemplateParam("T", 0, Bind<T>(), detail::uniformBindTypeEnum));
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

/* A named group of uniform fields — the unit that maps to a UBO binding on
 * the GPU. ShaderDef declares the schema (one UniformBlockDef per UBO it
 * reads). DrawPipeline / DrawBatch / DrawCommand declare which blocks they
 * provide. The link pass resolves layer blocks → shader blocks by `name`
 * and stamps `set`, `binding`, `packedBytes`, and `fieldOffsets`. */
struct UniformBlockDef {
  string name;
  util::Vector<UniformDefBase *> fields;

  /* Resolved by uniform_link::linkPipeline. Sentinel ~0u = unresolved. */
  uint32_t set = ~uint32_t(0);
  uint32_t binding = ~uint32_t(0);
  uint32_t packedBytes = 0;
  util::Vector<uint32_t> fieldOffsets;

  /* Backend hint: route through push constants if the API supports them and
   * `packedBytes` fits the device limit. Otherwise falls back to a UBO. */
  bool preferPushConstant = false;

  UniformBlockDef() = default;
  explicit UniformBlockDef(string name) : name(std::move(name))
  {
  }
  UniformBlockDef(string name, util::Vector<UniformDefBase *> fields)
      : name(std::move(name)), fields(std::move(fields))
  {
  }
  UniformBlockDef(const UniformBlockDef &) = delete;
  UniformBlockDef &operator=(const UniformBlockDef &) = delete;
  UniformBlockDef(UniformBlockDef &&b) noexcept
      : name(std::move(b.name)), fields(std::move(b.fields)), set(b.set),
        binding(b.binding), packedBytes(b.packedBytes),
        fieldOffsets(std::move(b.fieldOffsets)), preferPushConstant(b.preferPushConstant)
  {
  }
  ~UniformBlockDef()
  {
    for (auto *f : fields) {
      alloc::Delete(f);
    }
  }

  static litestl::binding::types::Struct<UniformBlockDef> *defineBindings()
  {
    using namespace litestl::binding;
    using litestl::util::Vector;

    types::Struct<UniformBlockDef> *st = new types::Struct<UniformBlockDef>(
        "sculptcore::gpu::UniformBlockDef", sizeof(UniformBlockDef));

    BIND_STRUCT_MEMBER(st, name);

    /* fields: Vector<UniformDefBase*> with the union discriminant — mirrors
     * the descriptor previously inlined in ShaderDef. */
    types::Pointer *ptrType = new types::Pointer(detail::uniformBindTypeUnion);
    ptrType->isNonNull = true;

    types::Struct<Vector<UniformDefBase *>> *vecSt =
        new types::Struct<Vector<UniformDefBase *>>("litestl::util::Vector",
                                                    sizeof(Vector<UniformDefBase *>));
    vecSt->addTemplateParam(ptrType, "T");
    vecSt->addTemplateParam(
        new types::NumLitType(
            decltype(UniformBlockDef::fields)::staticSize, "N", Bind<int>()),
        "N");
    st->add("fields", offsetof(UniformBlockDef, fields), vecSt);

    BIND_STRUCT_MEMBER(st, set);
    BIND_STRUCT_MEMBER(st, binding);
    BIND_STRUCT_MEMBER(st, packedBytes);
    BIND_STRUCT_MEMBER(st, preferPushConstant);
    return st;
  }
};

/* Per-layer (DrawPipeline / DrawBatch / DrawCommand) instance of a uniform
 * block — owns a descriptor and a packed CPU blob sized to `def->packedBytes`
 * after linking. Producers write field values via `set(name, value)`; the
 * backend uploads `data` into a UBO or push constant. */
struct UniformBlockInstance {
  UniformBlockDef *def = nullptr;
  util::Vector<uint8_t> data;

  UniformBlockInstance() = default;
  explicit UniformBlockInstance(UniformBlockDef *d) : def(d)
  {
  }
  UniformBlockInstance(const UniformBlockInstance &) = delete;
  UniformBlockInstance &operator=(const UniformBlockInstance &) = delete;
  UniformBlockInstance(UniformBlockInstance &&b) noexcept
      : def(b.def), data(std::move(b.data))
  {
    b.def = nullptr;
  }
  ~UniformBlockInstance()
  {
    if (def) {
      alloc::Delete(def);
    }
  }

  /* Find a field by name and memcpy `value` into `data` at its offset. Returns
   * false if `def` is null, the field doesn't exist, or the blob is too small
   * (e.g. linker hasn't run yet). */
  template <typename T> bool set(stringref fieldName, const T &value)
  {
    if (!def) {
      return false;
    }
    for (size_t i = 0; i < def->fields.size(); i++) {
      if (def->fields[i]->name == fieldName) {
        if (i >= def->fieldOffsets.size()) {
          return false;
        }
        uint32_t off = def->fieldOffsets[i];
        if (off + sizeof(T) > data.size()) {
          return false;
        }
        std::memcpy(data.data() + off, &value, sizeof(T));
        return true;
      }
    }
    return false;
  }

  static litestl::binding::types::Struct<UniformBlockInstance> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<UniformBlockInstance> *st = new types::Struct<UniformBlockInstance>(
        "sculptcore::gpu::UniformBlockInstance", sizeof(UniformBlockInstance));

    types::Pointer *ptr = new types::Pointer(Bind<UniformBlockDef>());
    ptr->isNonNull = false;
    st->add("def", offsetof(UniformBlockInstance, def), ptr);
    BIND_STRUCT_MEMBER(st, data);
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
  /* Schema of UBOs the shader reads. The link pass fills in (set, binding,
   * packedBytes, fieldOffsets) on each block based on the SPIR-V / WGSL
   * reflection (currently stamped by hand in the producer). */
  util::Vector<UniformBlockDef *> uniforms;
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
      : name(std::move(b.name)), wgslSource(std::move(b.wgslSource)),
        attrs(std::move(b.attrs)), uniforms(std::move(b.uniforms)),
        defines(std::move(b.defines)), spirv(b.spirv), spirvSize(b.spirvSize)
  {
    b.spirv = nullptr;
    b.spirvSize = 0;
  }
  ShaderDef &operator=(ShaderDef &&b) noexcept
  {
    if (this == &b)
      return *this;
    this->~ShaderDef();
    new (static_cast<void *>(this)) ShaderDef(std::move(b));
    return *this;
  }
  ShaderDef(string name,
            string wgslSource,
            util::Vector<AttrDef> attrs,
            util::Vector<UniformBlockDef *> uniforms,
            util::Vector<ShaderDefDefine> defines)
      : name(std::move(name)), wgslSource(std::move(wgslSource)), attrs(std::move(attrs)),
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
    for (auto *block : uniforms) {
      alloc::Delete(block);
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

    /* uniforms is Vector<UniformBlockDef*> — each block declares one UBO the
     * shader reads. */
    types::Pointer *ptrType = new types::Pointer(Bind<UniformBlockDef>());
    ptrType->isNonNull = true;

    types::Struct<Vector<UniformBlockDef *>> *vecSt =
        new types::Struct<Vector<UniformBlockDef *>>("litestl::util::Vector",
                                                     sizeof(Vector<UniformBlockDef *>));

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