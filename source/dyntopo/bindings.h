#pragma once

#include "litestl/binding/manager.h"

namespace sculptcore::dyntopo {
enum class DynTopoMode;

void registerBindings(litestl::binding::BindingManager &manager);
} // namespace sculptcore::dyntopo

namespace litestl::binding {
// bind() body lives in dyntopo/bindings.cc; declared here so every TU that
// binds a DynTopoMode member agrees on this spec (vs the generic primary).
template <> struct Binder<sculptcore::dyntopo::DynTopoMode> {
  static const BindingBase *bind();
};
} // namespace litestl::binding
