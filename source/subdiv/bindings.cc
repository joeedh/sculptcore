#include "bindings.h"
#include "multires.h"

using namespace litestl::binding;

namespace sculptcore::subdiv {
void registerBindings(BindingManager &manager)
{
  manager.add(Bind<Multires>());
}
} // namespace sculptcore::subdiv
