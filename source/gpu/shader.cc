#include "shader.h"

#include "math/vector.h"
#include "util/alloc.h"

namespace sculptcore::gpu::detail {
static litestl::binding::types::Enum *createUniformBindType()
{
  using namespace litestl::binding;
  litestl::alloc::PermanentGuard permGuard;

  types::Enum *et =
      new types::Enum("sculptcore::gpu::UniformBindType", sizeof(UniformBindType));
  et->addItem("FLOAT", UniformBindType::FLOAT);
  et->addItem("DOUBLE", UniformBindType::DOUBLE);
  et->addItem("BYTE", UniformBindType::BYTE);
  et->addItem("UBYTE", UniformBindType::UBYTE);
  et->addItem("SHORT", UniformBindType::SHORT);
  et->addItem("USHORT", UniformBindType::USHORT);
  et->addItem("INT", UniformBindType::INT);
  et->addItem("UINT", UniformBindType::UINT);
  et->addItem("FLOAT2", UniformBindType::FLOAT2);
  et->addItem("FLOAT3", UniformBindType::FLOAT3);
  et->addItem("FLOAT4", UniformBindType::FLOAT4);
  et->addItem("INT2", UniformBindType::INT2);
  et->addItem("INT3", UniformBindType::INT3);
  et->addItem("INT4", UniformBindType::INT4);
  et->addItem("UINT2", UniformBindType::UINT2);
  et->addItem("UINT3", UniformBindType::UINT3);
  et->addItem("UINT4", UniformBindType::UINT4);
  et->addItem("DOUBLE2", UniformBindType::DOUBLE2);
  et->addItem("DOUBLE3", UniformBindType::DOUBLE3);
  et->addItem("DOUBLE4", UniformBindType::DOUBLE4);
  et->addItem("SHORT2", UniformBindType::SHORT2);
  et->addItem("SHORT3", UniformBindType::SHORT3);
  et->addItem("SHORT4", UniformBindType::SHORT4);
  et->addItem("USHORT2", UniformBindType::USHORT2);
  et->addItem("USHORT3", UniformBindType::USHORT3);
  et->addItem("USHORT4", UniformBindType::USHORT4);
  et->addItem("BYTE2", UniformBindType::BYTE2);
  et->addItem("BYTE3", UniformBindType::BYTE3);
  et->addItem("BYTE4", UniformBindType::BYTE4);
  et->addItem("UBYTE2", UniformBindType::UBYTE2);
  et->addItem("UBYTE3", UniformBindType::UBYTE3);
  et->addItem("UBYTE4", UniformBindType::UBYTE4);
  return et;
}
litestl::binding::types::Enum *uniformBindTypeEnum = createUniformBindType();

static litestl::binding::types::Union *createUniformBindTypeUnion()
{
  using namespace litestl::binding;
  using namespace litestl::math;
  litestl::alloc::PermanentGuard permGuard;

  types::Union *u = new types::Union(
      "sculptcore::gpu::UniformBindTypeMap", "type", uniformBindTypeEnum);

  u->add("float",
         UniformBindType::FLOAT,
         static_cast<const types::_StructBase *>(Bind<UniformDef<float>>()));
  u->add("float2",
         UniformBindType::FLOAT2,
         static_cast<const types::_StructBase *>(Bind<UniformDef<float2>>()));
  u->add("float3",
         UniformBindType::FLOAT3,
         static_cast<const types::_StructBase *>(Bind<UniformDef<float3>>()));

  u->disPropFunc = [](const void *thisptr) -> int32_t {
    const UniformDefBase *base = static_cast<const UniformDefBase *>(thisptr);
    if (base->type == GPUType::FLOAT32 && base->elemSize == 1) {
      return int32_t(UniformBindType::FLOAT);
    } else if (base->type == GPUType::FLOAT32 && base->elemSize == 2) {
      return int32_t(UniformBindType::FLOAT2);
    } else if (base->type == GPUType::FLOAT32 && base->elemSize == 3) {
      return int32_t(UniformBindType::FLOAT3);
    } else if (base->type == GPUType::FLOAT32 && base->elemSize == 4) {
      return int32_t(UniformBindType::FLOAT4);
    }
    // should be unreachable
    return -1;
  };
  return u;
}

litestl::binding::types::Union *uniformBindTypeUnion = createUniformBindTypeUnion();
} // namespace sculptcore::gpu::detail
