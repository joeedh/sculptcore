#include "brush.h"
#include "brush_executor.h"
#include "litestl/binding/manager.h"

namespace litestl::binding {
const BindingBase *Binder<sculptcore::brush::FalloffKind>::bind()
{
  using namespace sculptcore::brush;
  types::Enum *e = new types::Enum("sculptcore::gpu::FalloffKind", sizeof(FalloffKind));

  e->addItem("LINEAR", FalloffKind::Linear);
  e->addItem("SMOOTHSTEP", FalloffKind::Smoothstep);
  e->addItem("GUASSIAN", FalloffKind::Gaussian);
  e->addItem("CURVE", FalloffKind::Curve);
  return e;
}
const BindingBase *Binder<sculptcore::brush::FalloffShape>::bind()
{
  using namespace sculptcore::brush;
  types::Enum *e = new types::Enum("sculptcore::gpu::FalloffShape", sizeof(FalloffShape));

  e->addItem("SPHERICAL", FalloffShape::Spherical);
  e->addItem("CUBE", FalloffShape::Cube);
  e->addItem("LINEAR", FalloffShape::Linear);
  e->addItem("BOX", FalloffShape::Box);
  return e;
}
} // namespace litestl::binding

namespace sculptcore::brush::bindings {
void registerBindings(litestl::binding::BindingManager &manager)
{
  using namespace litestl::binding;
  manager.add(Bind<Brush>());
  manager.add(Bind<BrushProgram>());
  manager.add(Bind<CommandExecutor>());
  manager.add(Bind<BrushUniformManifestEntry>());
  manager.add(Bind<util::Vector<spatial::SpatialNode *>>());
}
} // namespace sculptcore::brush::bindings
