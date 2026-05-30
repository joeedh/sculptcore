#include "brush.h"
#include "brush_executor.h"
#include "litestl/binding/manager.h"

namespace sculptcore::brush::bindings {
void registerBindings(litestl::binding::BindingManager &manager)
{
  using namespace litestl::binding;
  manager.add(Bind<Brush>());
  manager.add(Bind<BrushProgram>());
  manager.add(Bind<CommandExecutor>());
  manager.add(Bind<util::Vector<spatial::SpatialNode *>>());
}
} // namespace sculptcore::brush::bindings
