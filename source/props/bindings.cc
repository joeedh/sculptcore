#include "bindings.h"
#include "prop_base.h"
#include "prop_struct.h"
#include "prop_types.h"

using namespace litestl::binding;
namespace sculptcore::props {
void registerBindings(BindingManager &manager)
{
  manager.add(Bind<StructProp>());
}
} // namespace sculptcore::props
