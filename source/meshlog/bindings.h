#pragma once

#include "litestl/binding/manager.h"
#include "meshlog_base.h"

namespace sculptcore::meshlog {
static void registerBindings(litestl::binding::BindingManager &mgr)
{
  using namespace litestl::binding;
  mgr.add(Bind<MeshLog>());
}
} // namespace sculptcore::meshlog
