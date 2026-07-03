#include "bindings.h"
#include "vdm_store.h"

using namespace litestl::binding;

namespace sculptcore::vdm {
void registerBindings(BindingManager &manager)
{
  manager.add(Bind<VdmStore>());
}
} // namespace sculptcore::vdm
