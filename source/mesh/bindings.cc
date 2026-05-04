#include "bindings.h"
#include "gpu/mesh_drawbatch.h"

using namespace litestl::binding;

namespace sculptcore::mesh {
void registerBindings(BindingManager &manager)
{
  manager.add(Bind((Mesh *)nullptr));
  manager.add(BindAttrData());
  manager.add(Bind((gpu::MeshBatchManager *)nullptr));
}
} // namespace sculptcore::mesh