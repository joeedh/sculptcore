#include "dyntopo/bindings.h"
#include "dyntopo/dyntopo.h"

#include "litestl/binding/binding.h"
#include "litestl/binding/binding_constructor_builder.h"
#include "litestl/binding/binding_enum.h"

// DynTopoParams / DynTopoStats binding bodies live here (out-of-line) so
// dyntopo.h stays free of the binding headers — it sits on the brush hot path.

namespace litestl::binding {
// DynTopoMode is a plain (non-bitmask) enum class; bind it so the `mode` member
// of DynTopoParams resolves through BIND_STRUCT_MEMBER's Bind<decltype(field)>().
const BindingBase *Binder<sculptcore::dyntopo::DynTopoMode>::bind()
{
  using sculptcore::dyntopo::DynTopoMode;
  types::Enum *e =
      new types::Enum("sculptcore::dyntopo::DynTopoMode", sizeof(DynTopoMode));
  e->addItem("Subdivide", int(DynTopoMode::Subdivide));
  e->addItem("Collapse", int(DynTopoMode::Collapse));
  e->addItem("Both", int(DynTopoMode::Both));
  return e;
}

// Same treatment for the region-gating enum (the Sphere / Blender-port A/B).
const BindingBase *Binder<sculptcore::dyntopo::DynTopoRegion>::bind()
{
  using sculptcore::dyntopo::DynTopoRegion;
  types::Enum *e =
      new types::Enum("sculptcore::dyntopo::DynTopoRegion", sizeof(DynTopoRegion));
  e->addItem("Sphere", int(DynTopoRegion::Sphere));
  e->addItem("GradedRecursive", int(DynTopoRegion::GradedRecursive));
  return e;
}
} // namespace litestl::binding

namespace sculptcore::dyntopo {

litestl::binding::types::Struct<DynTopoParams> *DynTopoParams::defineBindings()
{
  using namespace litestl::binding;
  types::Struct<DynTopoParams> *st = new types::Struct<DynTopoParams>(
      "sculptcore::dyntopo::DynTopoParams", sizeof(DynTopoParams));

  BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
  BIND_STRUCT_COPY_CONSTRUCTOR(st); // crosses the seam by value

  BIND_STRUCT_MEMBER(st, l_max);
  BIND_STRUCT_MEMBER(st, l_min);
  BIND_STRUCT_MEMBER(st, mode);
  BIND_STRUCT_MEMBER(st, grade);
  BIND_STRUCT_MEMBER(st, region);
  BIND_STRUCT_MEMBER(st, graded_generation_scale);
  BIND_STRUCT_MEMBER(st, graded_len_sq_factor);
  BIND_STRUCT_MEMBER(st, graded_max_hops);
  BIND_STRUCT_MEMBER(st, graded_valence_relief);
  BIND_STRUCT_MEMBER(st, max_rounds);
  BIND_STRUCT_MEMBER(st, do_flips);
  BIND_STRUCT_MEMBER(st, max_splits);
  BIND_STRUCT_MEMBER(st, max_collapses);
  BIND_STRUCT_MEMBER(st, do_smooth);
  BIND_STRUCT_MEMBER(st, smooth_lambda);
  BIND_STRUCT_MEMBER(st, reproject_uvs);
  BIND_STRUCT_MEMBER(st, preserve_features);
  BIND_STRUCT_MEMBER(st, pinch_thin);
  BIND_STRUCT_MEMBER(st, pinch_ring_factor);
  BIND_STRUCT_MEMBER(st, cull_max_faces);
  BIND_STRUCT_MEMBER(st, cull_size);
  BIND_STRUCT_MEMBER(st, max_pinches);

  return st;
}

litestl::binding::types::Struct<DynTopoStats> *DynTopoStats::defineBindings()
{
  using namespace litestl::binding;
  types::Struct<DynTopoStats> *st = new types::Struct<DynTopoStats>(
      "sculptcore::dyntopo::DynTopoStats", sizeof(DynTopoStats));

  BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
  BIND_STRUCT_COPY_CONSTRUCTOR(st);

  BIND_STRUCT_MEMBER(st, splits);
  BIND_STRUCT_MEMBER(st, collapses);
  BIND_STRUCT_MEMBER(st, flips);
  BIND_STRUCT_MEMBER(st, smooths);
  BIND_STRUCT_MEMBER(st, rounds);
  BIND_STRUCT_MEMBER(st, capped);
  BIND_STRUCT_MEMBER(st, budget_hit);
  BIND_STRUCT_MEMBER(st, graded_hops);
  BIND_STRUCT_MEMBER(st, pinches);
  BIND_STRUCT_MEMBER(st, culled_faces);
  BIND_STRUCT_MEMBER(st, trivial_dissolves);

  return st;
}

void registerBindings(litestl::binding::BindingManager &manager)
{
  using namespace litestl::binding;
  manager.add(Bind<DynTopoMode>());
  manager.add(Bind<DynTopoRegion>());
  manager.add(Bind<DynTopoParams>());
  manager.add(Bind<DynTopoStats>());
}

} // namespace sculptcore::dyntopo
