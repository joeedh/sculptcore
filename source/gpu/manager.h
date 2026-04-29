#pragma once
#include "binding/binding_types.h"
#include "litestl/binding/binding_struct.h"
#include "litestl/util/vector.h"

namespace sculptcore::gpu {
struct ShaderDef;
struct Buffer;
struct DrawBatch;
struct DrawCommand;

using litestl::util::Vector;
struct GPUManager {
  Vector<ShaderDef *> shaders;
  Vector<Buffer *> buffers;
  Vector<DrawBatch *> batches;
  Vector<DrawCommand *> commands;
  ~GPUManager();

  /** used by move constructors/operators in resource classes */
  template <typename T> void transferOwnership(Vector<T *> &vec, T *dst, T *src)
  {
    if (vec.contains(src)) {
      vec.remove(src);
    }
    if (!vec.contains(dst)) {
      vec.append(dst);
    }
  }

  static litestl::binding::types::Struct<GPUManager> *defineBindings()
  {
    using litestl::binding::types::Constructor;
    using litestl::binding::types::Struct;
    using namespace litestl::binding;
    Struct<GPUManager> *st =
        new Struct<GPUManager>("sculptcore::gpu::GPUManager", sizeof(GPUManager));

    return st;
  }
};
} // namespace sculptcore::gpu

namespace litestl::binding {
template <std::same_as<sculptcore::gpu::GPUManager &> T> const BindingBase *Bind()
{
  return new types::Reference(Bind<sculptcore::gpu::GPUManager>(),
                              "sculptcore::gpu::GPUManager");
}
} // namespace litestl::binding
