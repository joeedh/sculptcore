#include "mesh/bindings.h"
#include "brush/bindings.h"
#include "dyntopo/bindings.h"
#include "gpu/bindings.h"
#include "meshlog/bindings.h"
#include "props/bindings.h"
#include "remesh/bindings.h"
#include "spatial/bindings.h"
#include "vdm/bindings.h"
#include "util/vector.h"
#include "wasm/wasmManager.h"


extern "C" void initBindings()
{
  using namespace litestl::binding;
  using litestl::util::Vector;

  auto &manager = sculptcore::wasm::manager;
  sculptcore::mesh::registerBindings(manager);
  sculptcore::gpu::registerBindings(manager);
  sculptcore::spatial::registerBindings(manager);
  sculptcore::props::registerBindings(manager);
  sculptcore::brush::bindings::registerBindings(manager);
  sculptcore::dyntopo::registerBindings(manager);
  sculptcore::remesh::registerBindings(manager);
  sculptcore::meshlog::registerBindings(manager);
  sculptcore::vdm::registerBindings(manager);

  // add various vector types
  manager.add(Bind<Vector<char>>());
  manager.add(Bind<Vector<signed char>>());
  manager.add(Bind<Vector<unsigned char>>());
  manager.add(Bind<Vector<short>>());
  manager.add(Bind<Vector<unsigned short>>());
  manager.add(Bind<Vector<int>>());
  manager.add(Bind<Vector<unsigned int>>());
  manager.add(Bind<Vector<int64_t>>());
  manager.add(Bind<Vector<uint64_t>>());
  manager.add(Bind<Vector<float>>());
  manager.add(Bind<Vector<double>>());
  manager.add(Bind<Vector<void *>>());
}