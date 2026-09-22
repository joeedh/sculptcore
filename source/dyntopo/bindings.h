#pragma once

#include "litestl/binding/manager.h"

namespace sculptcore::dyntopo {
enum class DynTopoMode;
enum class DynTopoRegion;

void registerBindings(litestl::binding::BindingManager &manager);
} // namespace sculptcore::dyntopo

namespace litestl::binding {
// bind() bodies live in dyntopo/bindings.cc; declared here so every TU that
// binds one of these members agrees on this spec (vs the generic primary).
template <> struct Binder<sculptcore::dyntopo::DynTopoMode> {
  static const BindingBase *bind();
};
template <> struct Binder<sculptcore::dyntopo::DynTopoRegion> {
  static const BindingBase *bind();
};
} // namespace litestl::binding
