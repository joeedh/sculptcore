#include "remesh/bindings.h"
#include "remesh/remesh_params.h"

#include "litestl/binding/binding.h"
#include "litestl/binding/binding_constructor_builder.h"

// RemeshParams binding body lives here (out-of-line) so remesh_params.h stays
// free of the binding headers (model: dyntopo/bindings.cc).

// The BIND_STRUCT_* macros expand to `binding::Bind<...>`, so the `litestl`
// namespace must be visible (dyntopo's bindings.cc gets this transitively
// through mesh.h; we don't include it).
using namespace litestl;

namespace sculptcore::remesh {

litestl::binding::types::Struct<RemeshParams> *RemeshParams::defineBindings()
{
  using namespace litestl::binding;
  types::Struct<RemeshParams> *st = new types::Struct<RemeshParams>(
      "sculptcore::remesh::RemeshParams", sizeof(RemeshParams));

  BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
  BIND_STRUCT_COPY_CONSTRUCTOR(st); // crosses the seam by value

  BIND_STRUCT_MEMBER(st, target_edge_length);
  BIND_STRUCT_MEMBER(st, solve_edge_length);
  BIND_STRUCT_MEMBER(st, use_curvature);
  BIND_STRUCT_MEMBER(st, use_sharp_features);
  BIND_STRUCT_MEMBER(st, sharp_angle);
  BIND_STRUCT_MEMBER(st, use_density);
  BIND_STRUCT_MEMBER(st, reproject);
  BIND_STRUCT_MEMBER(st, cap_odd_holes);
  BIND_STRUCT_MEMBER(st, smooth_iterations);
  BIND_STRUCT_MEMBER(st, smooth_strength);
  BIND_STRUCT_MEMBER(st, seed);
  BIND_STRUCT_MEMBER(st, triage);
  BIND_STRUCT_MEMBER(st, triage_weld_rel);
  BIND_STRUCT_MEMBER(st, triage_min_component_frac);

  return st;
}

void registerBindings(litestl::binding::BindingManager &manager)
{
  manager.add(RemeshParams::defineBindings());
}

} // namespace sculptcore::remesh
