#include "brush.h"
#include "litestl/binding/manager.h"

namespace sculptcore::brush::bindings {
void registerBindings(litestl::binding::BindingManager &manager)
{
  using namespace litestl::binding;
  manager.add(Bind<Brush>());
}
} // namespace sculptcore::brush::bindings
