#include "bindings.h"

using namespace litestl::binding;

namespace sculptcore::mesh {
void registerBindings(BindingManager &manager)
{
  manager.add(Bind<Mesh>());
}
} // namespace sculptcore::mesh