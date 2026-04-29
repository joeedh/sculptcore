#include "litestl/binding/binding.h"
#include "litestl/binding/manager.h"
#include "vbo.h"

namespace litestl::binding {
}
namespace sculptcore::gpu {
  static void registerBindings(litestl::binding::BindingManager &manager) {
    using namespace litestl::binding;
    manager.add(Bind<sculptcore::gpu::Buffer>());
    manager.add(Bind<sculptcore::gpu::GPUBufferType>());
  }
}